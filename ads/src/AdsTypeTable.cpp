#include "scope/ads/AdsTypeTable.h"

#include <QSet>
#include <QStringList>

#include <algorithm>
#include <optional>

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
// Nesting inside ONE entry's bytes: sub-items of sub-items. This is NOT how
// deep a member path can go — a member names its type and that type is its own
// table entry, which is what kMaxExpandDepth below governs. TwinCAT nests only
// a level or two inside a single entry (a DWORD's BIT members), so this cap is
// a malformed-table guard and nothing else.
constexpr int kMaxDepth = 16;
// Nesting across NAMED types is what an OO PLC project actually stacks up: a
// struct in a struct in a function block in a function block. That is bounded
// on its own — the type stack stops a type that contains itself, so no path
// can be longer than the number of distinct types in the table — and the cap
// each expansion uses is derived from exactly that (see expandWithTypeTable).
// This is only the ceiling that keeps the recursion inside the C++ stack: two
// frames per level, so 512 levels is a few hundred KB, and no real project
// comes within an order of magnitude of it.
constexpr int kExpandDepthCeiling = 512;

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

// What one expansion needs to carry down every branch, plus what it has to
// report back. Nothing here is silent: a member left out, a branch cut short
// by the leaf cap and a type that nests into itself all end up in the note
// the caller shows, because a symbol quietly missing from the browser is
// indistinguishable from one the PLC never published.
struct Expansion {
    const AdsTypeTable&     types;
    std::vector<AdsSymbol>& out;
    const AdsSymbol&        root;
    int                     maxLeaves;
    int                     depthCap;
    QStringList             skipped;
    QStringList             typeStack;   // the branch's types, upper-cased
    bool                    truncated{false};
    bool                    tooDeep{false};
    bool                    recursive{false};

    bool full() const { return static_cast<int>(out.size()) >= maxLeaves; }
};

// Bytes one type occupies, from the table or from the elementary type name.
std::optional<std::uint32_t> sizeOfTypeName(const AdsTypeTable& types,
                                            const QString& typeName) {
    if (const auto dt = dataTypeFromPlcTypeName(typeName))
        return static_cast<std::uint32_t>(scope::core::sizeOf(*dt));
    const auto it = types.constFind(typeName.toUpper());
    if (it != types.constEnd() && it->size > 0) return it->size;
    return std::nullopt;
}

void expandInto(Expansion& ex, const QString& name, const QString& typeName,
                std::uint32_t offset, int depth);

// Walk `count` elements of `elemTypeName`, each `elemSize` bytes, appending
// "name[i]" (or "name[i,j]") for every one. Shared by the two ways an array
// is described: a table entry with dims, and a bare "ARRAY [..] OF x" name.
void expandArrayElements(Expansion& ex, const QString& name,
                         const std::vector<ArrayDim>& dims,
                         const QString& elemTypeName, std::uint32_t elemSize,
                         std::uint32_t offset, int depth) {
    long long count = 1;
    for (const auto& d : dims) count *= d.count();
    if (count <= 0 || elemSize == 0) { ex.skipped << name; return; }

    std::vector<long long> idx(dims.size(), 0);
    for (long long n = 0; n < count; ++n) {
        if (ex.full()) { ex.truncated = true; return; }
        QStringList sub;
        for (std::size_t d = 0; d < idx.size(); ++d)
            sub << QString::number(dims[d].lower + idx[d]);
        expandInto(ex, QString("%1[%2]").arg(name, sub.join(',')), elemTypeName,
                   offset + static_cast<std::uint32_t>(n) * elemSize, depth + 1);
        for (std::size_t d = idx.size(); d-- > 0;) {   // odometer, last fastest
            if (++idx[d] < dims[d].count()) break;
            idx[d] = 0;
        }
    }
}

// One recursive step of the expansion.
void expandInto(Expansion& ex, const QString& name, const QString& typeName,
                std::uint32_t offset, int depth) {
    if (ex.full()) { ex.truncated = true; return; }
    if (depth > ex.depthCap) {
        ex.tooDeep = true;
        ex.skipped << name;
        return;
    }

    const auto it = ex.types.constFind(typeName.toUpper());

    // Not in the table (or an alias the table resolves to a base type): if the
    // name is a scalar we know, it's a leaf.
    if (it == ex.types.constEnd()) {
        if (const auto dt = dataTypeFromPlcTypeName(typeName)) {
            AdsSymbol leaf = ex.root;
            leaf.name        = name;
            leaf.typeName    = typeName;
            leaf.dataType    = *dt;
            leaf.unsupported = false;
            leaf.size        = static_cast<std::uint32_t>(scope::core::sizeOf(*dt));
            leaf.arrayLen    = 1;
            leaf.indexOffset = offset;
            ex.out.push_back(std::move(leaf));
            return;
        }
        // An array whose own "ARRAY [0..9] OF ST_Foo" entry the table doesn't
        // carry — TwinCAT publishes those inconsistently, and dropping the
        // member would hide everything inside it. The declaration is a
        // documented format, so read the dimensions off it and take the
        // element's size from the element type instead.
        if (const auto arr = parseArrayTypeName(typeName)) {
            if (const auto elemSize = sizeOfTypeName(ex.types, arr->elementTypeName)) {
                expandArrayElements(ex, name, arr->dims, arr->elementTypeName,
                                    *elemSize, offset, depth);
                return;
            }
        }
        if (!typeName.isEmpty())
            ex.skipped << QString("%1 (%2)").arg(name, typeName);
        return;
    }
    const AdsTypeEntry& t = *it;

    // A composite type that contains itself — only reachable through a
    // malformed table, since IEC has no by-value recursion — would otherwise
    // recurse until the depth cap on every branch.
    const bool composite = !t.dims.empty() || !t.members.empty();
    const QString key = typeName.toUpper();
    if (composite) {
        if (ex.typeStack.contains(key)) {
            ex.recursive = true;
            ex.skipped << QString("%1 (%2, contains itself)").arg(name, typeName);
            return;
        }
        ex.typeStack.push_back(key);
    }
    struct Pop {
        Expansion& ex; bool on;
        ~Pop() { if (on) ex.typeStack.removeLast(); }
    } pop{ex, composite};

    if (!t.dims.empty()) {
        long long count = 1;
        for (const auto& d : t.dims) count *= d.count();
        if (count <= 0 || t.size == 0) { ex.skipped << name; return; }
        const auto elemSize = static_cast<std::uint32_t>(t.size / count);
        expandArrayElements(ex, name, t.dims, t.elementTypeName, elemSize,
                            offset, depth);
        return;
    }

    if (!t.members.empty()) {
        for (const auto& m : t.members) {
            if (m.bitValue) {
                // A single bit: its "offset" is a bit index, and it lives at
                // the bit-access group with the whole address in bits. Adding
                // it as a byte offset would silently read a neighbouring byte.
                if (ex.full()) { ex.truncated = true; return; }
                AdsSymbol leaf = ex.root;
                leaf.name        = name + "." + m.name;
                leaf.typeName    = QStringLiteral("BIT");
                leaf.dataType    = DataType::Bool;
                leaf.unsupported = false;
                leaf.size        = 1;
                leaf.arrayLen    = 1;
                leaf.indexGroup  = kAdsigrpPlcRwBit;
                leaf.indexOffset = offset * 8 + m.offset;
                ex.out.push_back(std::move(leaf));
                continue;
            }
            expandInto(ex, name + "." + m.name, m.typeName, offset + m.offset,
                       depth + 1);
        }
        return;
    }

    // A scalar or an alias: prefer the raw ADST code, fall back to the base
    // type name the table points at (BOOL, for instance, is listed as BYTE).
    auto dt = mapAdsDataTypeOptShared(t.adsDataType);
    if (!dt) dt = dataTypeFromPlcTypeName(t.name);
    if (!dt) dt = dataTypeFromPlcTypeName(t.elementTypeName);
    if (!dt) { ex.skipped << QString("%1 (%2)").arg(name, t.name); return; }

    AdsSymbol leaf = ex.root;
    leaf.name        = name;
    leaf.typeName    = t.name;
    leaf.dataType    = *dt;
    leaf.adsDataType = t.adsDataType;
    leaf.unsupported = false;
    leaf.size        = static_cast<std::uint32_t>(scope::core::sizeOf(*dt));
    leaf.arrayLen    = 1;
    leaf.indexOffset = offset;
    ex.out.push_back(std::move(leaf));
}

}  // namespace

std::vector<AdsSymbol> expandWithTypeTable(const AdsSymbol& sym,
                                           const AdsTypeTable& types,
                                           int maxLeaves,
                                           ExpansionReport* reportOut) {
    std::vector<AdsSymbol> out;
    // A path visits each type at most once — the type stack refuses a repeat —
    // so it can be no longer than the table itself. Deriving the cap from the
    // table means no real structure is ever cut for being deep; the ceiling is
    // only there to keep the recursion inside the stack.
    const int depthCap = std::clamp(static_cast<int>(types.size()) + 8, 64,
                                    kExpandDepthCeiling);
    Expansion ex{types, out, sym, maxLeaves, depthCap, {}, {}, false, false, false};
    expandInto(ex, sym.name, sym.typeName, sym.indexOffset, /*depth=*/0);

    // A symbol that expands to exactly itself isn't an expansion.
    if (out.size() == 1 && out.front().name == sym.name) out.clear();

    if (!reportOut) return out;
    reportOut->truncated = ex.truncated;
    reportOut->tooDeep   = ex.tooDeep;
    reportOut->recursive = ex.recursive;
    reportOut->skipped   = ex.skipped;

    QStringList notes;
    if (ex.truncated)
        notes << QString("%1 holds more than %2 recordable values; the rest "
                         "were left out of the list. Add them with “Add by "
                         "name”.").arg(sym.name).arg(maxLeaves);
    if (ex.tooDeep)
        notes << QString("%1 nests more than %2 levels deep; the deepest "
                         "members were left out.").arg(sym.name).arg(depthCap);
    if (ex.recursive)
        notes << QString("%1 has a type that contains itself, which cannot be "
                         "expanded.").arg(sym.name);
    if (!ex.skipped.isEmpty())
        notes << QString("Not recordable, so left out: %1")
                     .arg(ex.skipped.mid(0, 8).join(", "))
             + (ex.skipped.size() > 8
                    ? QString(" (+%1 more)").arg(ex.skipped.size() - 8)
                    : QString());
    reportOut->note = notes.join("  ");
    return out;
}

std::vector<AdsSymbol> expandAggregates(const std::vector<AdsSymbol>& symbols,
                                        const AdsTypeTable& types,
                                        ListingLimits limits,
                                        ListingReport* reportOut) {
    QSet<QString> known;
    known.reserve(static_cast<int>(symbols.size()));
    for (const auto& s : symbols) known.insert(s.name);

    std::vector<AdsSymbol> out;
    std::vector<const AdsSymbol*> pending;   // cut short by the first cap
    int budget = limits.budget;

    auto expandOne = [&](const AdsSymbol& s, int cap, ExpansionReport& rep) {
        std::vector<AdsSymbol> leaves;
        if (cap <= 0) { rep.truncated = true; return leaves; }
        if (!types.isEmpty()) leaves = expandWithTypeTable(s, types, cap, &rep);
        if (leaves.empty()) {
            // No type table, or nothing in it for this type: an array of
            // scalars is still expandable from its declaration alone.
            QString arrWarn;
            leaves = expandArraySymbol(s, cap, &arrWarn);
            if (!arrWarn.isEmpty()) {
                rep.note = arrWarn;
                // That path is all-or-nothing past its cap, so an array bigger
                // than `cap` yields nothing — which a larger cap would fix.
                // Saying so is what puts it in the round that grows.
                const auto info = parseArrayTypeName(s.typeName);
                if (info && info->totalElements > cap) rep.truncated = true;
                else rep.skipped << QString("%1 (%2)").arg(s.name, s.typeName);
            }
        }
        return leaves;
    };
    auto keep = [&](std::vector<AdsSymbol>& leaves) {
        for (auto& e : leaves) {
            if (known.contains(e.name)) continue;   // the PLC already lists it
            known.insert(e.name);
            --budget;
            out.push_back(std::move(e));
        }
    };
    // A limit is news; a STRING member is not. See ListingReport.
    auto record = [&](const ExpansionReport& rep) {
        if (!reportOut) return;
        reportOut->skipped += rep.skipped;
        if (rep.truncated || rep.tooDeep || rep.recursive)
            reportOut->limits << rep.note;
    };
    auto limitNote = [&](const QString& text) {
        if (reportOut) reportOut->limits << text;
    };

    for (const auto& s : symbols) {
        if (!s.unsupported) continue;
        ExpansionReport rep;
        auto leaves = expandOne(s, std::min(limits.firstCap, std::max(budget, 0)),
                                rep);
        // Held back rather than kept short: round two expands it in full, and
        // keeping this cut copy would only have to be undone.
        if (rep.truncated) { pending.push_back(&s); continue; }
        record(rep);
        keep(leaves);
    }

    for (std::size_t i = 0; i < pending.size(); ++i) {
        const int share = static_cast<int>(pending.size() - i);
        const int cap = std::max(budget, 0) / share;
        if (cap <= 0) {
            limitNote(QString("The symbol list is full at %1 values, so %2 was "
                              "left out of it. Use “Add by name” to record its "
                              "members.").arg(limits.budget).arg(pending[i]->name));
            continue;
        }
        ExpansionReport rep;
        auto leaves = expandOne(*pending[i], cap, rep);
        record(rep);
        keep(leaves);
    }
    return out;
}

}  // namespace scope::ads
