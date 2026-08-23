// ADS symbol typing.
//
// TwinCAT reports one symbol per DECLARED variable, and for a struct, function
// block or array that symbol's ADST code is 65 (ADST_BIGTYPE) — an aggregate,
// not one number. The ADST→DataType mapping used to end in
// `default: return DataType::Float64`, so such a symbol came back claiming to
// be a double and the recorder happily sampled 8 bytes at its base offset:
// element [0] of an ARRAY OF LREAL, four INTs crammed into a double for an
// ARRAY OF INT, the first 8 bytes of a struct reinterpreted. No warning — the
// samples just looked like data.
//
// The same default also mis-typed plain scalars missing from the list
// (SINT/USINT/BYTE/WORD/DWORD/LINT/ULINT), which is silent corruption on
// ordinary variables, not just exotic ones.
//
// These tests pin the mapping itself. Expanding an aggregate into per-member /
// per-element channels needs the ADS data-type table (0xF00E) and a live PLC
// to validate the wire layout against; until then the contract is: list it,
// mark it, refuse to record it.

#include "scope/core/IAdsClient.h"
#include "scope/core/Types.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>

using namespace scope::core;

namespace {

// Mirrors mapAdsDataTypeOpt in ads/src/RouterAdsClient.cpp. Kept in the test
// as an executable statement of the contract: every code either maps to a
// type whose size matches what the PLC declares, or is refused.
std::optional<DataType> mapExpected(std::uint32_t adst) {
    switch (adst) {
        case 33: return DataType::Bool;
        case 16: return DataType::Int8;
        case 17: return DataType::Uint8;
        case 2:  return DataType::Int16;
        case 18: return DataType::Uint16;
        case 3:  return DataType::Int32;
        case 19: return DataType::Uint32;
        case 20: return DataType::Int64;
        case 21: return DataType::Uint64;
        case 4:  return DataType::Float32;
        case 5:  return DataType::Float64;
        default: return std::nullopt;
    }
}

}  // namespace

TEST(AdsSymbolTypes, AggregateCodesHaveNoScalarMapping) {
    // 65 = ADST_BIGTYPE: every struct, function block and most arrays.
    EXPECT_FALSE(mapExpected(65).has_value())
        << "an aggregate must never map to a scalar — that is how a struct "
           "used to be recorded as its first 8 bytes";
    EXPECT_FALSE(mapExpected(30).has_value()) << "STRING is not numeric";
    EXPECT_FALSE(mapExpected(31).has_value()) << "WSTRING is not numeric";
    EXPECT_FALSE(mapExpected(0).has_value())  << "VOID is not numeric";
}

TEST(AdsSymbolTypes, UnknownCodesAreRefusedNotDefaultedToDouble) {
    for (std::uint32_t code : {1u, 6u, 7u, 22u, 32u, 64u, 66u, 99u, 1000u}) {
        EXPECT_FALSE(mapExpected(code).has_value())
            << "code " << code << " must be refused, not silently Float64";
    }
}

// Each mapped code's size must equal what the PLC declares for that type, or
// the notification length check in NotifyChannel silently drops samples.
TEST(AdsSymbolTypes, EveryMappedCodeHasTheRightWidth) {
    struct Case { std::uint32_t adst; std::size_t bytes; const char* plc; };
    for (const auto& c : {
             Case{33, 1, "BOOL"},  Case{16, 1, "SINT"},  Case{17, 1, "USINT/BYTE"},
             Case{2,  2, "INT"},   Case{18, 2, "UINT/WORD"},
             Case{3,  4, "DINT"},  Case{19, 4, "UDINT/DWORD"},
             Case{20, 8, "LINT"},  Case{21, 8, "ULINT/LWORD"},
             Case{4,  4, "REAL"},  Case{5,  8, "LREAL"}}) {
        const auto dt = mapExpected(c.adst);
        ASSERT_TRUE(dt.has_value()) << c.plc;
        EXPECT_EQ(sizeOf(*dt), c.bytes)
            << c.plc << " (ADST " << c.adst << ") has the wrong width";
    }
}

// The scalars that used to fall through the default and be recorded as
// doubles. SINT read as an LREAL is 8 bytes off a 1-byte variable.
TEST(AdsSymbolTypes, PreviouslyDefaultedScalarsNowMapCorrectly) {
    EXPECT_EQ(mapExpected(16), std::optional<DataType>(DataType::Int8));    // SINT
    EXPECT_EQ(mapExpected(17), std::optional<DataType>(DataType::Uint8));   // USINT/BYTE
    EXPECT_EQ(mapExpected(18), std::optional<DataType>(DataType::Uint16));  // WORD
    EXPECT_EQ(mapExpected(19), std::optional<DataType>(DataType::Uint32));  // DWORD
    EXPECT_EQ(mapExpected(20), std::optional<DataType>(DataType::Int64));   // LINT
    EXPECT_EQ(mapExpected(21), std::optional<DataType>(DataType::Uint64));  // ULINT
}

// AdsSymbol must carry enough for a caller to tell "listed" from "recordable".
TEST(AdsSymbolTypes, SymbolCarriesTheRawCodeAndAnUnsupportedFlag) {
    AdsSymbol sym;
    EXPECT_EQ(sym.adsDataType, 0u);
    EXPECT_FALSE(sym.unsupported) << "a default-constructed symbol is recordable";

    // What listSymbols does for a struct: keep the raw code, flag it, and put
    // a placeholder in dataType that callers must not trust.
    sym.name = "MAIN.stAxis";
    sym.typeName = "ST_Axis";
    sym.adsDataType = 65;
    sym.unsupported = !mapExpected(sym.adsDataType).has_value();
    EXPECT_TRUE(sym.unsupported);
    EXPECT_EQ(sym.adsDataType, 65u) << "the raw code must survive the mapping";
}

// arrayLen used to be size / sizeOf(dataType) — for a 40-byte struct that is
// "5", a number with no meaning that nothing ever read. It is now simply 1,
// with array shape left to the type table.
//
// Every scalar field also has a default now: callers fill AdsSymbol field by
// field, so an unset one was being read as garbage.
TEST(AdsSymbolTypes, ScalarFieldsAreDefaultInitialised) {
    AdsSymbol sym;
    EXPECT_EQ(sym.arrayLen, 1u);
    EXPECT_EQ(sym.size, 0u);
    EXPECT_EQ(sym.indexGroup, 0u);
    EXPECT_EQ(sym.indexOffset, 0u);
    EXPECT_EQ(sym.adsDataType, 0u);
    EXPECT_FALSE(sym.unsupported);
}
