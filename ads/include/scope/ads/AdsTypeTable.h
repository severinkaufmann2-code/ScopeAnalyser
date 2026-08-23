#pragma once

#include "scope/ads/AdsTypeNames.h"
#include "scope/core/IAdsClient.h"

#include <QHash>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace scope::ads {

// One member of a structure or function block.
struct AdsTypeMember {
    QString       name;
    QString       typeName;
    std::uint32_t offset{0};   // bytes from the start of the parent
    std::uint32_t size{0};
    std::uint32_t adsDataType{0};
};

// One entry of the ADS data-type table (ADSIGRP_SYM_DT_UPLOAD, 0xF00E).
// A type is either an array (dims non-empty, elementTypeName set), a
// structure (members non-empty), or a scalar/alias (neither).
struct AdsTypeEntry {
    QString       name;
    QString       elementTypeName;   // the "type" field: an array's element
                                     // type, or a scalar alias's base type
    std::uint32_t size{0};
    std::uint32_t adsDataType{0};
    std::vector<ArrayDim>     dims;
    std::vector<AdsTypeMember> members;
};

// Keyed by the type name, upper-cased — TwinCAT is not consistent about case
// between the symbol list and the type table.
using AdsTypeTable = QHash<QString, AdsTypeEntry>;

// Parse the raw 0xF00E reply.
//
// Entry layout, confirmed byte-for-byte against a live TwinCAT 3 capture (see
// tests/data/ads/): eight uint32 (entryLength, version, hashValue,
// typeHashValue, size, offs, dataType, flags), then five uint16 (nameLength,
// typeLength, commentLength, arrayDim, subItems) = 42 bytes; then the three
// NUL-terminated strings; then arrayDim x (int32 lower bound, uint32 count);
// then subItems nested entries of the same shape.
//
// Always advances by entryLength, never by a computed size, so an entry
// carrying trailing GUID/attribute data we don't read is still skipped
// correctly. Stops and reports via *warnOut on anything malformed rather than
// walking off the end.
AdsTypeTable parseAdsTypeTable(std::span<const std::byte> blob,
                               QString* warnOut = nullptr);

// Expand a symbol into one recordable leaf per scalar, walking the type table:
// a structure yields "sym.member", an array yields "sym[i]", and the two nest
// ("sym.axes[2].pos").
//
// Leaves keep the symbol's index group and sit at its offset plus the
// accumulated member/element offset — the same shape the notification path
// already handles, so nothing new is needed to record them.
//
// Refuses rather than guesses: a member whose type isn't in the table or isn't
// a recordable scalar is skipped with a note in *warnOut, and expansion stops
// at `maxLeaves` (a whole PLC's worth of leaves would be unusable) and at a
// recursion depth cap (a self-referential type would otherwise never end).
std::vector<scope::core::AdsSymbol> expandWithTypeTable(
    const scope::core::AdsSymbol& sym, const AdsTypeTable& types,
    int maxLeaves = 4096, QString* warnOut = nullptr);

}  // namespace scope::ads
