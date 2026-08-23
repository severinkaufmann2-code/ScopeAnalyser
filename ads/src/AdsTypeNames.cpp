#include "scope/ads/AdsTypeNames.h"

#include <QRegularExpression>
#include <QStringList>

namespace scope::ads {

using scope::core::AdsSymbol;
using scope::core::DataType;

std::optional<DataType> dataTypeFromPlcTypeName(const QString& typeName) {
    const QString t = typeName.trimmed().toUpper();
    // IEC 61131-3 elementary types, plus the TwinCAT bit-string aliases.
    if (t == "BOOL")                       return DataType::Bool;
    if (t == "SINT")                       return DataType::Int8;
    if (t == "USINT" || t == "BYTE")       return DataType::Uint8;
    if (t == "INT")                        return DataType::Int16;
    if (t == "UINT" || t == "WORD")        return DataType::Uint16;
    if (t == "DINT")                       return DataType::Int32;
    if (t == "UDINT" || t == "DWORD")      return DataType::Uint32;
    if (t == "LINT")                       return DataType::Int64;
    if (t == "ULINT" || t == "LWORD")      return DataType::Uint64;
    if (t == "REAL")                       return DataType::Float32;
    if (t == "LREAL")                      return DataType::Float64;
    // Deliberately unmapped: STRING/WSTRING (not numeric), TIME/DATE and
    // friends (encoded, not a plain number), pointers, references, and every
    // user-defined struct or function block.
    return std::nullopt;
}

std::optional<ArrayTypeInfo> parseArrayTypeName(const QString& typeName) {
    // "ARRAY [0..99] OF LREAL", "ARRAY[1..2,0..3] OF INT" — whitespace and
    // case vary between TwinCAT versions, so be liberal about both.
    static const QRegularExpression re(
        R"(^\s*ARRAY\s*\[(.+?)\]\s*OF\s+(.+?)\s*$)",
        QRegularExpression::CaseInsensitiveOption);
    const auto m = re.match(typeName);
    if (!m.hasMatch()) return std::nullopt;

    ArrayTypeInfo info;
    info.elementTypeName = m.captured(2).trimmed();
    if (info.elementTypeName.isEmpty()) return std::nullopt;

    static const QRegularExpression dimRe(R"(^\s*(-?\d+)\s*\.\.\s*(-?\d+)\s*$)");
    info.totalElements = 1;
    const QStringList parts = m.captured(1).split(',');
    for (const QString& part : parts) {
        const auto dm = dimRe.match(part);
        if (!dm.hasMatch()) return std::nullopt;
        bool okLo = false, okHi = false;
        ArrayDim d;
        d.lower = dm.captured(1).toLongLong(&okLo);
        d.upper = dm.captured(2).toLongLong(&okHi);
        if (!okLo || !okHi || d.upper < d.lower) return std::nullopt;
        info.dims.push_back(d);
        info.totalElements *= d.count();
        if (info.totalElements <= 0) return std::nullopt;   // overflow guard
    }
    if (info.dims.empty()) return std::nullopt;
    return info;
}

std::vector<AdsSymbol> expandArraySymbol(const AdsSymbol& sym, int maxElements,
                                         QString* warnOut) {
    auto warn = [&](const QString& m) {
        if (warnOut) *warnOut = m;
        return std::vector<AdsSymbol>{};
    };

    const auto info = parseArrayTypeName(sym.typeName);
    if (!info) return {};                     // not an array; nothing to say

    const auto elemType = dataTypeFromPlcTypeName(info->elementTypeName);
    if (!elemType)
        return warn(QString("%1: elements are '%2', which isn't a type we can "
                            "record.").arg(sym.name, info->elementTypeName));

    if (info->totalElements > maxElements)
        return warn(QString("%1 has %2 elements; only the first %3 would be "
                            "listed, so it was left collapsed.")
                        .arg(sym.name)
                        .arg(info->totalElements)
                        .arg(maxElements));

    // The consistency check that makes this safe without a live PLC: the
    // declaration and the declared byte size must agree. If they don't, our
    // reading of one of them is wrong and any offset we computed would be
    // wrong too — so emit nothing rather than plausible-looking rubbish.
    const auto elemSize = static_cast<long long>(scope::core::sizeOf(*elemType));
    const long long expected = info->totalElements * elemSize;
    if (static_cast<long long>(sym.size) != expected)
        return warn(QString("%1: '%2' implies %3 bytes but the PLC reports %4, "
                            "so its elements were not expanded.")
                        .arg(sym.name, sym.typeName)
                        .arg(expected)
                        .arg(sym.size));

    // Row-major, matching IEC: the LAST dimension varies fastest.
    std::vector<AdsSymbol> out;
    out.reserve(static_cast<std::size_t>(info->totalElements));
    std::vector<long long> idx(info->dims.size(), 0);
    for (long long n = 0; n < info->totalElements; ++n) {
        QStringList sub;
        sub.reserve(static_cast<int>(idx.size()));
        for (std::size_t d = 0; d < idx.size(); ++d)
            sub << QString::number(info->dims[d].lower + idx[d]);

        AdsSymbol e = sym;
        e.name        = QString("%1[%2]").arg(sym.name, sub.join(','));
        e.typeName    = info->elementTypeName;
        e.dataType    = *elemType;
        e.unsupported = false;
        e.adsDataType = 0;            // the element's own code isn't on the wire
        e.size        = static_cast<std::uint32_t>(elemSize);
        e.arrayLen    = 1;
        e.indexOffset = sym.indexOffset + static_cast<std::uint32_t>(n * elemSize);
        e.comment     = sym.comment;
        out.push_back(std::move(e));

        for (std::size_t d = idx.size(); d-- > 0;) {   // odometer, last fastest
            if (++idx[d] < info->dims[d].count()) break;
            idx[d] = 0;
        }
    }
    return out;
}

}  // namespace scope::ads
