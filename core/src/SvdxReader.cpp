#include "scope/core/SvdxReader.h"

#include "scope/core/Types.h"

#include <spdlog/spdlog.h>

#include <QByteArray>
#include <QXmlStreamReader>

#include <algorithm>
#include <cstring>
#include <fstream>

namespace scope::core {

namespace {

// FILETIME (100 ns ticks since 1601-01-01) → ns since the Unix epoch.
constexpr std::uint64_t kFiletimeUnixDiff = 116444736000000000ULL;  // 100 ns
inline TimestampNs filetimeToUnixNs(std::uint64_t ft) {
    if (ft < kFiletimeUnixDiff) return 0;
    return static_cast<TimestampNs>((ft - kFiletimeUnixDiff) * 100ULL);
}

template <class T>
inline T rd(const std::byte* p) {
    T v{};
    std::memcpy(&v, p, sizeof(T));
    return v;
}

DataType mapType(QString t) {
    t = t.trimmed().toUpper();
    if (t == "BIT" || t == "BOOL") return DataType::Bool;
    if (t == "INT8" || t == "SINT") return DataType::Int8;
    if (t == "UINT8" || t == "USINT" || t == "BYTE") return DataType::Uint8;
    if (t == "INT16" || t == "INT") return DataType::Int16;
    if (t == "UINT16" || t == "UINT" || t == "WORD") return DataType::Uint16;
    if (t == "INT32" || t == "DINT") return DataType::Int32;
    if (t == "UINT32" || t == "UDINT" || t == "DWORD") return DataType::Uint32;
    if (t == "INT64" || t == "LINT") return DataType::Int64;
    if (t == "UINT64" || t == "ULINT" || t == "LWORD") return DataType::Uint64;
    if (t == "REAL32" || t == "REAL") return DataType::Float32;
    if (t == "REAL64" || t == "LREAL") return DataType::Float64;
    return DataType::Float64;
}

struct ChanMeta {
    QString  name;
    QString  unit;
    DataType dataType{DataType::Float64};
    bool     gotType{false};
};

// Parse the <ScopeProject> footer for per-channel metadata, in document order.
// The recorded channels live in the DataPool as `*Acquisition` elements (e.g.
// <AdsAcquisition>), each carrying a <Name> and <DataType>. The decorative
// <Channel> nodes elsewhere are chart styling, not data, so we ignore them.
std::vector<ChanMeta> parseChannels(const QByteArray& xml) {
    QXmlStreamReader xr(xml);
    std::vector<ChanMeta> all;
    std::vector<std::size_t> stack;     // indices into `all` for open acquisitions
    std::vector<QString> elems;         // element-name stack

    auto isAcq = [](const QString& n) { return n.endsWith(QLatin1String("Acquisition")); };

    while (!xr.atEnd()) {
        switch (xr.readNext()) {
            case QXmlStreamReader::StartElement: {
                const QString n = xr.name().toString();
                elems.push_back(n);
                if (isAcq(n)) {
                    all.push_back(ChanMeta{});
                    stack.push_back(all.size() - 1);
                }
                break;
            }
            case QXmlStreamReader::Characters: {
                if (xr.isWhitespace() || stack.empty() || elems.empty()) break;
                ChanMeta& c = all[stack.back()];
                const QString& cur = elems.back();
                const QString parent =
                    elems.size() >= 2 ? elems[elems.size() - 2] : QString();
                const QString txt = xr.text().toString();
                if (cur == QLatin1String("Name") && isAcq(parent) && c.name.isEmpty()) {
                    c.name = txt;
                } else if (cur == QLatin1String("DataType") && !c.gotType) {
                    c.dataType = mapType(txt);
                    c.gotType = true;
                } else if (cur == QLatin1String("Symbol") &&
                           parent == QLatin1String("Unit") && c.unit.isEmpty()) {
                    c.unit = txt;
                }
                break;
            }
            case QXmlStreamReader::EndElement:
                if (isAcq(xr.name().toString()) && !stack.empty())
                    stack.pop_back();
                if (!elems.empty()) elems.pop_back();
                break;
            default:
                break;
        }
    }

    std::vector<ChanMeta> out;
    for (auto& c : all)
        if (c.gotType) out.push_back(std::move(c));
    return out;
}

// Length (in sub-segments) of the [FILETIME][count][count*rec] chain starting
// at `off`, or 0 if `off` is not a valid chain head.
int chainLen(const std::byte* b, std::size_t n, std::uint64_t lo, std::uint64_t hi,
             std::size_t rec, std::size_t off) {
    std::size_t o = off;
    int segs = 0;
    while (o + 12 <= n) {
        const std::uint64_t ft = rd<std::uint64_t>(b + o);
        const std::uint32_t cnt = rd<std::uint32_t>(b + o + 8);
        if (ft < lo || ft > hi || cnt == 0 || cnt > 1'000'000) break;
        const std::size_t next = o + 12 + static_cast<std::size_t>(cnt) * rec;
        if (next > n) { segs++; break; }   // last (possibly clamped) segment
        o = next;
        if (++segs >= 64) break;           // enough to be confident
    }
    return segs;
}

// Find where the full-resolution sub-segment chain begins. The data may sit
// far into the block (after a min/max overview layer), so scan the whole
// block, but cheaply: only attempt to validate at offsets whose first 8 bytes
// look like a plausible FILETIME.
std::size_t findChainStart(const std::byte* b, std::size_t n,
                           std::uint64_t lo, std::uint64_t hi, std::size_t rec) {
    for (std::size_t off = 43; off + 12 <= n; ++off) {
        const std::uint64_t ft = rd<std::uint64_t>(b + off);
        if (ft < lo || ft > hi) continue;
        if (chainLen(b, n, lo, hi, rec, off) >= 4) return off;
    }
    return 0;
}

// Decode one channel's data block into (timestamps, raw value bytes).
// `dt` is taken as a hint from the XML; if it doesn't yield a valid sub-segment
// chain (or no XML was present) the value size is auto-detected.
bool decodeBlock(const std::byte* b, std::size_t n, DataType& dt,
                 std::vector<TimestampNs>& ts, std::vector<std::byte>& vals) {
    if (n < 43) return false;
    const std::uint64_t startFT = rd<std::uint64_t>(b + 11);
    const std::uint64_t segEndFT = rd<std::uint64_t>(b + 35);
    const std::uint64_t lo = startFT > 10'000'000ULL ? startFT - 10'000'000ULL : 0;
    const std::uint64_t hi = segEndFT + 2'000'000'000ULL;  // +200 ms slack
    const TimestampNs endNs = filetimeToUnixNs(segEndFT);

    std::size_t vsize = sizeOf(dt);
    std::size_t start = findChainStart(b, n, lo, hi, 4 + vsize);
    if (start == 0) {
        // Auto-detect: pick the value size whose chain reaches furthest.
        static constexpr DataType kGuess[] = {DataType::Float64, DataType::Float32,
                                              DataType::Int16, DataType::Bool};
        int best = 0;
        for (DataType g : kGuess) {
            const std::size_t s = findChainStart(b, n, lo, hi, 4 + sizeOf(g));
            const int len = s ? chainLen(b, n, lo, hi, 4 + sizeOf(g), s) : 0;
            if (len > best) { best = len; dt = g; vsize = sizeOf(g); start = s; }
        }
        if (start == 0) return false;
    }
    const std::size_t rec = 4 + vsize;

    std::size_t o = start;
    while (o + 12 <= n) {
        const std::uint64_t subFT = rd<std::uint64_t>(b + o);
        std::uint32_t cnt = rd<std::uint32_t>(b + o + 8);
        if (cnt == 0 || cnt > 1'000'000) break;
        const std::size_t base = o + 12;
        if (base + static_cast<std::size_t>(cnt) * rec > n)
            cnt = static_cast<std::uint32_t>((n - base) / rec);
        const TimestampNs subNs = filetimeToUnixNs(subFT);
        for (std::uint32_t k = 0; k < cnt; ++k) {
            const std::size_t p = base + static_cast<std::size_t>(k) * rec;
            const std::uint32_t tick = rd<std::uint32_t>(b + p);
            const TimestampNs abs = subNs + static_cast<TimestampNs>(tick) * 100;
            if (abs > endNs) continue;  // trim to the segment's end
            ts.push_back(abs);
            const std::byte* vp = b + p + 4;
            vals.insert(vals.end(), vp, vp + vsize);
        }
        o = base + static_cast<std::size_t>(cnt) * rec;
    }
    return !ts.empty();
}

}  // namespace

std::vector<std::shared_ptr<Signal>> readSvdx(const std::filesystem::path& path,
                                              QString* errorOut) {
    auto fail = [&](const QString& m) {
        if (errorOut) *errorOut = m;
        return std::vector<std::shared_ptr<Signal>>{};
    };

    std::ifstream f(path, std::ios::binary);
    if (!f) return fail("Couldn't open file.");
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size < 0x40) return fail("File too small to be a .svdx.");

    // --- block index ---------------------------------------------------------
    std::byte idxHdr[0x14];
    f.seekg(0);
    f.read(reinterpret_cast<char*>(idxHdr), sizeof(idxHdr));
    const std::uint32_t blockCount = rd<std::uint32_t>(idxHdr + 0x10);
    if (blockCount == 0 || blockCount > 1'000'000)
        return fail("Unrecognised .svdx (bad block count).");

    std::vector<std::byte> idx(static_cast<std::size_t>(blockCount) * 20);
    f.seekg(0x14);
    f.read(reinterpret_cast<char*>(idx.data()), static_cast<std::streamsize>(idx.size()));
    struct Blk { std::uint64_t off, len; std::uint32_t idx; };
    std::vector<Blk> blocks(blockCount);
    for (std::uint32_t i = 0; i < blockCount; ++i) {
        const std::byte* p = idx.data() + i * 20;
        blocks[i] = {rd<std::uint64_t>(p), rd<std::uint64_t>(p + 8), rd<std::uint32_t>(p + 16)};
        if (blocks[i].off + blocks[i].len > static_cast<std::uint64_t>(size))
            return fail("Corrupt .svdx (block out of range).");
    }
    std::sort(blocks.begin(), blocks.end(),
              [](const Blk& a, const Blk& b) { return a.idx < b.idx; });

    // --- XML footer: locate the last <ScopeProject> and read to EOF ----------
    // The two header variants store xml offset/length differently, so find the
    // footer by scanning the tail rather than trusting a header field.
    const std::streamoff tailLen = std::min<std::streamoff>(size, 16 * 1024 * 1024);
    std::vector<char> tail(static_cast<std::size_t>(tailLen));
    f.seekg(size - tailLen);
    f.read(tail.data(), tailLen);
    const QByteArray tailBuf(tail.data(), static_cast<int>(tail.size()));
    int rel = tailBuf.lastIndexOf("<ScopeProject");
    std::vector<ChanMeta> chans;
    if (rel >= 0) {
        const int decl = tailBuf.lastIndexOf("<?xml", rel);
        const int from = decl >= 0 && rel - decl < 200 ? decl : rel;
        chans = parseChannels(tailBuf.mid(from));
    }

    // --- decode blocks → signals --------------------------------------------
    std::vector<std::shared_ptr<Signal>> out;
    out.reserve(blocks.size());
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        std::vector<std::byte> buf(blocks[i].len);
        f.seekg(static_cast<std::streamoff>(blocks[i].off));
        f.read(reinterpret_cast<char*>(buf.data()),
               static_cast<std::streamsize>(buf.size()));

        Signal::Meta meta;
        if (i < chans.size()) {
            meta.name = chans[i].name;
            meta.unit = chans[i].unit;
            meta.dataType = chans[i].dataType;
        }
        if (meta.name.isEmpty()) meta.name = QString("Channel_%1").arg(i + 1);

        std::vector<TimestampNs> ts;
        std::vector<std::byte> vals;
        if (!decodeBlock(buf.data(), buf.size(), meta.dataType, ts, vals)) {
            spdlog::warn("svdx: could not decode block {} ('{}')", i,
                         meta.name.toStdString());
            continue;
        }
        auto sig = std::make_shared<Signal>(meta);
        sig->append(ts.data(), vals.data(), ts.size());
        out.push_back(std::move(sig));
    }

    if (out.empty())
        return fail("No decodable channels found. This .svdx may use a block "
                    "layout that isn't supported yet (e.g. high-rate continuous "
                    "channels).");
    if (errorOut) *errorOut = QString();
    return out;
}

}  // namespace scope::core
