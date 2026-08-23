#include "scope/ads/AdsTypeTable.h"

#include <QStringList>

#include <cstring>

namespace scope::ads {

using scope::core::AdsSymbol;
using scope::core::DataType;

namespace {

constexpr std::size_t kHeaderBytes = 42;
// Bit-addressed access. A BIT member is reached at this group with the offset
// expressed in bits, not bytes — confirmed against the PLC's own answer for
// AXIS_REF's ControlDWord.Enable (byte 0x60A98 -> 0x4041:0x3054C0 = 0x60A98*8).
constexpr std::uint32_t kAdsigrpPlcRwBit = 0x4041;
constexpr int kMaxDepth = 16;   // a self-referential type must still terminate

std::uint16_t u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
std::uint32_t u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8)
         | (static_cast<std::uint32_t>(p[2]) << 16)
         | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::int32_t i32(const std::uint8_t* p) {
    return static_cast<std::int32_t>(u32(p));
}

// Fields we read out of one entry; `end` is where its own payload stopped,
// which is <= start + entryLength.
struct RawEntry {
    QString       name, typeName;
    std::uint32_t size{0}, offset{0}, adsDataType{0}, entryLength{0};
    std::vector<ArrayDim>      dims;
    std::vector<AdsTypeMember> members;
    bool ok{false};
};

// Parse one entry (and, recursively, its sub-items) at `p`.
RawEntry parseEntry(const std::uint8_t* base, std::size_t total, std::size_t p,
                    int depth, QString* warnOut) {
    RawEntry e;
    if (depth > kMaxDepth || p + kHeaderBytes > total) return e;

    const std::uint8_t* h = base + p;
    e.entryLength      = u32(h + 0);
    e.size             = u32(h + 16);
    e.offset           = u32(h + 20);
    e.adsDataType      = u32(h + 24);
    const std::uint16_t nameLen = u16(h + 32);
    const std::uint16_t typeLen = u16(h + 34);
    const std::uint16_t commLen = u16(h + 36);
    const std::uint16_t arrayDim = u16(h + 38);
    const std::uint16_t subItems = u16(h + 40);

    if (e.entryLength < kHeaderBytes || p + e.entryLength > total) return e;

    // The three NUL-terminated strings.
    std::size_t b = p + kHeaderBytes;
    const std::size_t strBytes =
        static_cast<std::size_t>(nameLen) + 1 + typeLen + 1 + commLen + 1;
    if (b + strBytes > p + e.entryLength) return e;
    e.name     = QString::fromLatin1(reinterpret_cast<const char*>(base + b), nameLen);
    b += nameLen + 1;
    e.typeName = QString::fromLatin1(reinterpret_cast<const char*>(base + b), typeLen);
    b += typeLen + 1 + commLen + 1;

    // Array bounds: lower bound is SIGNED (ARRAY [-5..5] is legal IEC).
    for (std::uint16_t i = 0; i < arrayDim; ++i) {
        if (b + 8 > p + e.entryLength) return e;
        ArrayDim d;
        d.lower = i32(base + b);
        const std::uint32_t count = u32(base + b + 4);
        d.upper = d.lower + static_cast<long long>(count) - 1;
        e.dims.push_back(d);
        b += 8;
    }

    // Sub-items are complete nested entries.
    for (std::uint16_t i = 0; i < subItems; ++i) {
        if (b + kHeaderBytes > p + e.entryLength) return e;
        const RawEntry sub = parseEntry(base, total, b, depth + 1, warnOut);
        if (!sub.ok || sub.entryLength == 0) return e;
        AdsTypeMember m;
        m.name        = sub.name;
        m.typeName    = sub.typeName;
        m.offset      = sub.offset;
        m.size        = sub.size;
        m.adsDataType = sub.adsDataType;
        m.bitValue    = sub.typeName.compare(QLatin1String("BIT"),
                                             Qt::CaseInsensitive) == 0;
        e.members.push_back(std::move(m));
        b += sub.entryLength;
    }

    e.ok = true;
    return e;
}

}  // namespace

AdsTypeTable parseAdsTypeTable(std::span<const std::byte> blob, QString* warnOut) {
    AdsTypeTable out;
    const auto* base = reinterpret_cast<const std::uint8_t*>(blob.data());
    const std::size_t total = blob.size();

    std::size_t p = 0;
    while (p + kHeaderBytes <= total) {
        const RawEntry e = parseEntry(base, total, p, 0, warnOut);
        if (!e.ok || e.entryLength == 0) {
            if (warnOut)
                *warnOut = QString("The PLC's data-type table stopped making sense "
                                   "%1 bytes in; %2 types were read.")
                               .arg(p).arg(out.size());
            break;
        }
        AdsTypeEntry t;
        t.name            = e.name;
        t.elementTypeName = e.typeName;
        t.size            = e.size;
        t.adsDataType     = e.adsDataType;
        t.dims            = e.dims;
        t.members         = e.members;
        out.insert(t.name.toUpper(), std::move(t));
        p += e.entryLength;   // never a computed size
    }
    return out;
}

namespace {

// One recursive step of the expansion.
void expandInto(std::vector<AdsSymbol>& out, const AdsSymbol& root,
                const QString& name, const QString& typeName,
                std::uint32_t offset, const AdsTypeTable& types, int depth,
                int maxLeaves, QStringList& skipped) {
    if (static_cast<int>(out.size()) >= maxLeaves || depth > kMaxDepth) return;

    const auto it = types.constFind(typeName.toUpper());

    // Not in the table (or an alias the table resolves to a base type): if the
    // name is a scalar we know, it's a leaf.
    if (it == types.constEnd()) {
        if (const auto dt = dataTypeFromPlcTypeName(typeName)) {
            AdsSymbol leaf = root;
            leaf.name        = name;
            leaf.typeName    = typeName;
            leaf.dataType    = *dt;
            leaf.unsupported = false;
            leaf.size        = static_cast<std::uint32_t>(scope::core::sizeOf(*dt));
            leaf.arrayLen    = 1;
            leaf.indexOffset = offset;
            out.push_back(std::move(leaf));
        } else if (!typeName.isEmpty()) {
            skipped << QString("%1 (%2)").arg(name, typeName);
        }
        return;
    }
    const AdsTypeEntry& t = *it;

    if (!t.dims.empty()) {
        long long count = 1;
        for (const auto& d : t.dims) count *= d.count();
        if (count <= 0 || t.size == 0) { skipped << name; return; }
        const auto elemSize = static_cast<std::uint32_t>(t.size / count);
        if (elemSize == 0) { skipped << name; return; }

        std::vector<long long> idx(t.dims.size(), 0);
        for (long long n = 0; n < count; ++n) {
            if (static_cast<int>(out.size()) >= maxLeaves) return;
            QStringList sub;
            for (std::size_t d = 0; d < idx.size(); ++d)
                sub << QString::number(t.dims[d].lower + idx[d]);
            expandInto(out, root, QString("%1[%2]").arg(name, sub.join(',')),
                       t.elementTypeName,
                       offset + static_cast<std::uint32_t>(n) * elemSize,
                       types, depth + 1, maxLeaves, skipped);
            for (std::size_t d = idx.size(); d-- > 0;) {
                if (++idx[d] < t.dims[d].count()) break;
                idx[d] = 0;
            }
        }
        return;
    }

    if (!t.members.empty()) {
        for (const auto& m : t.members) {
            if (m.bitValue) {
                // A single bit: its "offset" is a bit index, and it lives at
                // the bit-access group with the whole address in bits. Adding
                // it as a byte offset would silently read a neighbouring byte.
                if (static_cast<int>(out.size()) >= maxLeaves) return;
                AdsSymbol leaf = root;
                leaf.name        = name + "." + m.name;
                leaf.typeName    = QStringLiteral("BIT");
                leaf.dataType    = DataType::Bool;
                leaf.unsupported = false;
                leaf.size        = 1;
                leaf.arrayLen    = 1;
                leaf.indexGroup  = kAdsigrpPlcRwBit;
                leaf.indexOffset = offset * 8 + m.offset;
                out.push_back(std::move(leaf));
                continue;
            }
            expandInto(out, root, name + "." + m.name, m.typeName,
                       offset + m.offset, types, depth + 1, maxLeaves, skipped);
        }
        return;
    }

    // A scalar or an alias: prefer the raw ADST code, fall back to the base
    // type name the table points at (BOOL, for instance, is listed as BYTE).
    auto dt = mapAdsDataTypeOptShared(t.adsDataType);
    if (!dt) dt = dataTypeFromPlcTypeName(t.name);
    if (!dt) dt = dataTypeFromPlcTypeName(t.elementTypeName);
    if (!dt) { skipped << QString("%1 (%2)").arg(name, t.name); return; }

    AdsSymbol leaf = root;
    leaf.name        = name;
    leaf.typeName    = t.name;
    leaf.dataType    = *dt;
    leaf.adsDataType = t.adsDataType;
    leaf.unsupported = false;
    leaf.size        = static_cast<std::uint32_t>(scope::core::sizeOf(*dt));
    leaf.arrayLen    = 1;
    leaf.indexOffset = offset;
    out.push_back(std::move(leaf));
}

}  // namespace

std::vector<AdsSymbol> expandWithTypeTable(const AdsSymbol& sym,
                                           const AdsTypeTable& types,
                                           int maxLeaves, QString* warnOut) {
    std::vector<AdsSymbol> out;
    QStringList skipped;
    expandInto(out, sym, sym.name, sym.typeName, sym.indexOffset, types,
               /*depth=*/0, maxLeaves, skipped);

    // A symbol that expands to exactly itself isn't an expansion.
    if (out.size() == 1 && out.front().name == sym.name) out.clear();

    if (warnOut && !skipped.isEmpty())
        *warnOut = QString("Not recordable, so left out: %1")
                       .arg(skipped.mid(0, 8).join(", "))
                 + (skipped.size() > 8 ? QString(" (+%1 more)").arg(skipped.size() - 8)
                                       : QString());
    return out;
}

}  // namespace scope::ads
