#pragma once

#include "scope/ads/AdsTypeNames.h"
#include "scope/core/IAdsClient.h"

#include <QHash>
#include <QString>
#include <QStringList>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace scope::ads {

// One member of a structure or function block.
struct AdsTypeMember {
    QString       name;
    QString       typeName;
    // Bytes from the start of the parent — EXCEPT when bitValue is set, in
    // which case it is a bit index (0..7 within the parent's first byte, and
    // upward), because TwinCAT addresses single bits differently.
    std::uint32_t offset{0};
    std::uint32_t size{0};
    std::uint32_t adsDataType{0};
    // A single bit inside a bit-field (declared BIT, e.g. AXIS_REF's
    // ControlDWord.Enable). Addressed at index group 0x4041 with the offset
    // in BITS — recording it at a byte offset reads a whole neighbouring byte
    // instead.
    bool          bitValue{false};
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
// What an expansion could not deliver. `truncated` is the one a caller can act
// on: it means `maxLeaves` stopped the walk and a larger cap yields more, which
// is what lets a caller grow the cap to fit instead of guessing one up front.
struct ExpansionReport {
    bool        truncated{false};   // the leaf cap cut it short
    bool        tooDeep{false};     // nested past the depth ceiling
    bool        recursive{false};   // a type that contains itself
    QStringList skipped;            // "member (type)" left out, one per entry
    QString     note;               // all of the above in one sentence or two,
                                    // empty when there is nothing to report
};

// Refuses rather than guesses: a member whose type isn't in the table or isn't
// a recordable scalar is skipped and named in the report.
//
// Everything left out is reported — the leaf cap being hit, a branch too deep,
// a recursive type, and each skipped member. A member that vanishes from the
// browser without a word is indistinguishable from one the PLC never
// published, which is the bug this reporting exists to prevent.
//
// Nesting is not capped at any fixed depth. A type that contains itself is
// caught by the chain of types being expanded, so the only remaining bound is
// the one the C++ stack needs; a legitimate path cannot be longer than the
// number of distinct types anyway, and that is what the cap is derived from.
//
// `maxLeaves` bounds the result. It is the caller's budget, not a guess at
// what a PLC holds: the caller can re-run with a larger one whenever the
// report comes back truncated.
std::vector<scope::core::AdsSymbol> expandWithTypeTable(
    const scope::core::AdsSymbol& sym, const AdsTypeTable& types,
    int maxLeaves = 32768, ExpansionReport* reportOut = nullptr);

// How far a whole symbol listing may be expanded.
//
// `budget` is what the browser can hold, not what a PLC contains: one leaf
// costs roughly 23 us to put in the symbol tree and ~1.5 kB of memory, so the
// default is about 4.5 s and 310 MB on a refresh. `firstCap` is what each
// aggregate gets in the first round, before any of them is grown.
struct ListingLimits {
    int firstCap{32768};
    int budget{200000};
};

// Expand every aggregate in `symbols` into recordable leaves, growing each
// symbol's cap to fit rather than cutting it at a fixed one.
//
// Two rounds: every aggregate is expanded at `firstCap`, then whatever is
// left of the budget is shared between the ones that ran past it — recomputed
// as it goes, so what one doesn't use flows to the next. That ordering is the
// point: one 100k-leaf function block cannot swallow the budget before the
// small structures beside it have been listed at all.
//
// Leaves whose name a symbol in `symbols` already carries are dropped: the
// PLC's own entry is authoritative about the address, and TwinCAT does
// publish some structure members in their own right.
//
// The two kinds of "left out" are kept apart on purpose. `limits` is the one
// worth interrupting for — a budget spent, a structure cut short — and is
// rare. `skipped` is the everyday kind: a STRING, a POINTER, an interface
// has no single numeric value to record, and every PLC has dozens. Mixing
// them would bury the first kind under the second.
struct ListingReport {
    QStringList limits;    // this listing could not hold everything
    QStringList skipped;   // "member (type)" that has no numeric value
};

std::vector<scope::core::AdsSymbol> expandAggregates(
    const std::vector<scope::core::AdsSymbol>& symbols,
    const AdsTypeTable& types, ListingLimits limits = {},
    ListingReport* reportOut = nullptr);

}  // namespace scope::ads
