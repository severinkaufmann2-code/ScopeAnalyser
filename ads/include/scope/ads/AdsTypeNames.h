#pragma once

#include "scope/core/IAdsClient.h"
#include "scope/core/Types.h"

#include <QString>

#include <optional>
#include <vector>

namespace scope::ads {

// Map a PLC type name ("LREAL", "DINT", …) to the scalar we record it as.
// Case-insensitive. Returns nullopt for aggregates, strings and anything we
// don't know — never a guess.
std::optional<scope::core::DataType> dataTypeFromPlcTypeName(const QString& typeName);

// One dimension of an IEC array declaration.
struct ArrayDim {
    long long lower{0};
    long long upper{0};
    long long count() const { return upper - lower + 1; }
};

// Parsed "ARRAY [0..99] OF LREAL" / "ARRAY [1..2, 0..3] OF INT".
struct ArrayTypeInfo {
    std::vector<ArrayDim> dims;
    QString elementTypeName;
    long long totalElements{0};
};

// Parse an IEC array type name. Returns nullopt when the string isn't an
// array declaration (a struct, a function block, a scalar).
//
// This is why array expansion needs no ADS data-type table: the symbol upload
// already hands us the declaration verbatim, and it is a documented, stable,
// human-readable format.
std::optional<ArrayTypeInfo> parseArrayTypeName(const QString& typeName);

// Expand an array symbol into one recordable symbol per element.
//
// Elements of an array live at the SAME indexGroup, at indexOffset + i *
// elementSize, with a scalar element type — exactly the shape the notification
// path already handles, so nothing new is needed to record them.
//
// Refuses (returns empty, sets *warnOut) rather than guessing when anything
// fails to add up: an unparseable declaration, an element type we can't record,
// or — the important one — a declared total size that is not
// elementCount * sizeof(elementType). That last check means a mistaken parse
// cannot silently produce wrong offsets; it produces nothing.
//
// `maxElements` caps the expansion so a huge array can't flood the symbol list;
// beyond it nothing is emitted and *warnOut says so.
std::vector<scope::core::AdsSymbol> expandArraySymbol(
    const scope::core::AdsSymbol& sym, int maxElements = 4096,
    QString* warnOut = nullptr);

}  // namespace scope::ads
