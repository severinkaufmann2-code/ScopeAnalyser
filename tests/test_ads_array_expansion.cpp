// Expanding an ARRAY symbol into one recordable channel per element.
//
// TwinCAT's symbol upload lists one entry per declared variable, so an
// `aData : ARRAY [0..99] OF LREAL` appears once and its elements never do.
// Reading the ADS data-type table (0xF00E) would enumerate them, but nothing
// in the vendored Beckhoff headers describes that table's wire format.
//
// It turns out not to be needed for arrays: the upload already carries the
// declaration verbatim as the type NAME, and that is a documented, stable,
// human-readable format. Elements sit at the same indexGroup at
// indexOffset + i*elemSize with a scalar element type — exactly what the
// notification path already handles.
//
// The safety property that makes this trustworthy without a PLC: the declared
// element count times the element size must equal the byte size the PLC
// reports. When it doesn't, our reading of one of them is wrong, so expansion
// emits NOTHING rather than plausible-looking offsets.

#include "scope/ads/AdsTypeNames.h"

#include <gtest/gtest.h>

using namespace scope::ads;
using namespace scope::core;

namespace {

// An array symbol as listSymbols would build it.
AdsSymbol arraySym(const char* name, const char* typeName, std::uint32_t bytes,
                   std::uint32_t group = 0x4020, std::uint32_t offset = 0x1000) {
    AdsSymbol s;
    s.name = name;
    s.typeName = typeName;
    s.size = bytes;
    s.indexGroup = group;
    s.indexOffset = offset;
    s.adsDataType = 65;      // ADST_BIGTYPE, what TwinCAT reports for arrays
    s.unsupported = true;
    return s;
}

}  // namespace

TEST(PlcTypeNames, MapsElementaryTypesAndRefusesTheRest) {
    EXPECT_EQ(dataTypeFromPlcTypeName("LREAL"), std::optional<DataType>(DataType::Float64));
    EXPECT_EQ(dataTypeFromPlcTypeName("real"),  std::optional<DataType>(DataType::Float32));
    EXPECT_EQ(dataTypeFromPlcTypeName(" DINT "), std::optional<DataType>(DataType::Int32));
    EXPECT_EQ(dataTypeFromPlcTypeName("BYTE"),  std::optional<DataType>(DataType::Uint8));
    EXPECT_EQ(dataTypeFromPlcTypeName("WORD"),  std::optional<DataType>(DataType::Uint16));
    EXPECT_EQ(dataTypeFromPlcTypeName("LWORD"), std::optional<DataType>(DataType::Uint64));
    EXPECT_EQ(dataTypeFromPlcTypeName("BOOL"),  std::optional<DataType>(DataType::Bool));

    for (const char* t : {"STRING(80)", "ST_Axis", "FB_Motor", "TIME", "", "POINTER TO INT"})
        EXPECT_FALSE(dataTypeFromPlcTypeName(t).has_value()) << t;
}

TEST(PlcArrayTypeName, ParsesASingleDimension) {
    const auto info = parseArrayTypeName("ARRAY [0..99] OF LREAL");
    ASSERT_TRUE(info.has_value());
    ASSERT_EQ(info->dims.size(), 1u);
    EXPECT_EQ(info->dims[0].lower, 0);
    EXPECT_EQ(info->dims[0].upper, 99);
    EXPECT_EQ(info->totalElements, 100);
    EXPECT_EQ(info->elementTypeName.toStdString(), "LREAL");
}

TEST(PlcArrayTypeName, ToleratesSpacingAndCaseAndNegativeBounds) {
    for (const char* t : {"ARRAY[0..9] OF INT", "array [ 0 .. 9 ] of Int",
                          "ARRAY  [0..9]  OF  INT"}) {
        const auto i = parseArrayTypeName(t);
        ASSERT_TRUE(i.has_value()) << t;
        EXPECT_EQ(i->totalElements, 10) << t;
    }
    const auto neg = parseArrayTypeName("ARRAY [-5..5] OF INT");
    ASSERT_TRUE(neg.has_value());
    EXPECT_EQ(neg->totalElements, 11);
    EXPECT_EQ(neg->dims[0].lower, -5);
}

TEST(PlcArrayTypeName, ParsesMultipleDimensions) {
    const auto info = parseArrayTypeName("ARRAY [1..2, 0..3] OF INT");
    ASSERT_TRUE(info.has_value());
    ASSERT_EQ(info->dims.size(), 2u);
    EXPECT_EQ(info->totalElements, 8);
}

TEST(PlcArrayTypeName, RejectsWhatIsNotAnArray) {
    for (const char* t : {"LREAL", "ST_Axis", "FB_Motor", "", "ARRAY OF INT",
                          "ARRAY [] OF INT", "ARRAY [a..b] OF INT",
                          "ARRAY [5..1] OF INT"})
        EXPECT_FALSE(parseArrayTypeName(t).has_value()) << t;
}

TEST(ArrayExpansion, EmitsOneRecordableSymbolPerElement) {
    const auto sym = arraySym("MAIN.aData", "ARRAY [0..99] OF LREAL", 800);
    QString warn;
    const auto out = expandArraySymbol(sym, 4096, &warn);

    ASSERT_EQ(out.size(), 100u) << warn.toStdString();
    EXPECT_TRUE(warn.isEmpty());

    EXPECT_EQ(out.front().name.toStdString(), "MAIN.aData[0]");
    EXPECT_EQ(out.back().name.toStdString(), "MAIN.aData[99]");
    for (const auto& e : out) {
        EXPECT_EQ(e.typeName.toStdString(), "LREAL");
        EXPECT_EQ(static_cast<int>(e.dataType), static_cast<int>(DataType::Float64));
        EXPECT_FALSE(e.unsupported) << "an element IS recordable";
        EXPECT_EQ(e.size, 8u);
        EXPECT_EQ(e.arrayLen, 1u);
        EXPECT_EQ(e.indexGroup, sym.indexGroup) << "elements share the group";
    }
    // Contiguous, starting at the array's own offset.
    EXPECT_EQ(out[0].indexOffset, sym.indexOffset);
    EXPECT_EQ(out[1].indexOffset, sym.indexOffset + 8);
    EXPECT_EQ(out[99].indexOffset, sym.indexOffset + 99 * 8);
}

TEST(ArrayExpansion, HonoursNonZeroLowerBounds) {
    const auto sym = arraySym("MAIN.a", "ARRAY [1..3] OF INT", 6);
    const auto out = expandArraySymbol(sym);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].name.toStdString(), "MAIN.a[1]");
    EXPECT_EQ(out[2].name.toStdString(), "MAIN.a[3]");
    // The declared index shifts the NAME, never the offset.
    EXPECT_EQ(out[0].indexOffset, sym.indexOffset);
    EXPECT_EQ(out[2].indexOffset, sym.indexOffset + 4);
}

TEST(ArrayExpansion, MultiDimensionalIsRowMajorLastIndexFastest) {
    const auto sym = arraySym("MAIN.m", "ARRAY [0..1, 0..2] OF INT", 12);
    const auto out = expandArraySymbol(sym);
    ASSERT_EQ(out.size(), 6u);
    EXPECT_EQ(out[0].name.toStdString(), "MAIN.m[0,0]");
    EXPECT_EQ(out[1].name.toStdString(), "MAIN.m[0,1]");
    EXPECT_EQ(out[2].name.toStdString(), "MAIN.m[0,2]");
    EXPECT_EQ(out[3].name.toStdString(), "MAIN.m[1,0]");
    EXPECT_EQ(out[5].name.toStdString(), "MAIN.m[1,2]");
    for (std::size_t i = 0; i < out.size(); ++i)
        EXPECT_EQ(out[i].indexOffset, sym.indexOffset + 2 * i);
}

// The property that lets this ship without PLC validation.
TEST(ArrayExpansion, EmitsNothingWhenTheDeclaredSizeDisagrees) {
    // 100 LREALs is 800 bytes; the PLC says 640. One of the two readings is
    // wrong, so every offset we'd compute is suspect.
    const auto sym = arraySym("MAIN.aData", "ARRAY [0..99] OF LREAL", 640);
    QString warn;
    const auto out = expandArraySymbol(sym, 4096, &warn);
    EXPECT_TRUE(out.empty()) << "must not guess";
    EXPECT_TRUE(warn.contains("800")) << warn.toStdString();
    EXPECT_TRUE(warn.contains("640")) << warn.toStdString();
}

TEST(ArrayExpansion, RefusesElementTypesItCannotRecord) {
    QString warn;
    const auto out = expandArraySymbol(
        arraySym("MAIN.aNames", "ARRAY [0..9] OF STRING(80)", 810), 4096, &warn);
    EXPECT_TRUE(out.empty());
    EXPECT_TRUE(warn.contains("STRING(80)")) << warn.toStdString();

    QString warn2;
    const auto structs = expandArraySymbol(
        arraySym("MAIN.aAxes", "ARRAY [0..9] OF ST_Axis", 400), 4096, &warn2);
    EXPECT_TRUE(structs.empty()) << "arrays of structs need the type table";
    EXPECT_TRUE(warn2.contains("ST_Axis")) << warn2.toStdString();
}

TEST(ArrayExpansion, CapsHugeArraysInsteadOfFloodingTheList) {
    QString warn;
    const auto out = expandArraySymbol(
        arraySym("MAIN.big", "ARRAY [0..99999] OF LREAL", 800000), 4096, &warn);
    EXPECT_TRUE(out.empty());
    EXPECT_TRUE(warn.contains("100000")) << warn.toStdString();
    EXPECT_TRUE(warn.contains("4096")) << warn.toStdString();
}

TEST(ArrayExpansion, LeavesNonArraysAloneSilently) {
    QString warn;
    AdsSymbol scalar;
    scalar.name = "MAIN.fSpeed";
    scalar.typeName = "LREAL";
    scalar.size = 8;
    EXPECT_TRUE(expandArraySymbol(scalar, 4096, &warn).empty());
    EXPECT_TRUE(warn.isEmpty()) << "a scalar isn't a problem to report";

    AdsSymbol st;
    st.name = "MAIN.stAxis";
    st.typeName = "ST_Axis";
    st.size = 40;
    EXPECT_TRUE(expandArraySymbol(st, 4096, &warn).empty());
    EXPECT_TRUE(warn.isEmpty()) << "a struct isn't an array; nothing to say here";
}

// ---------------------------------------------------------------------------
// Resolving a symbol by name (ADSIGRP_SYM_INFOBYNAMEEX).
//
// The route to structure members. A struct is ONE entry in the symbol upload —
// its member names and byte offsets appear only in the ADS data-type table,
// whose wire layout the vendored Beckhoff headers don't describe. Asking the
// PLC about "MAIN.stAxis.fActPos" sidesteps that entirely: it answers with an
// AdsSymbolEntry, a struct that IS fully defined in those headers.
//
// Driven here through MockAdsClient, which models the same asymmetry: the
// struct is listed but opaque, and its members exist only by name.
// ---------------------------------------------------------------------------

#include "scope/ads/MockAdsClient.h"

TEST(ResolveByName, StructMembersResolveThoughTheyAreInNoList) {
    scope::ads::MockAdsClient client;
    ASSERT_TRUE(client.connect({}, nullptr));

    // The member is genuinely absent from the listing…
    const auto listed = client.listSymbols(nullptr);
    for (const auto& s : listed)
        EXPECT_NE(s.name.toStdString(), "Mock.stAxis.fActPos")
            << "the upload must not enumerate members — that's the whole problem";

    // …yet resolves by name, with its own address and type.
    QString err;
    const auto m = client.resolveSymbol("Mock.stAxis.fActPos", &err);
    ASSERT_TRUE(m.has_value()) << err.toStdString();
    EXPECT_EQ(m->name.toStdString(), "Mock.stAxis.fActPos");
    EXPECT_EQ(m->typeName.toStdString(), "LREAL");
    EXPECT_EQ(static_cast<int>(m->dataType), static_cast<int>(DataType::Float64));
    EXPECT_EQ(m->size, 8u);
    EXPECT_FALSE(m->unsupported) << "a member IS recordable";
}

TEST(ResolveByName, TheStructItselfIsRefusedWithAUsefulMessage) {
    scope::ads::MockAdsClient client;
    ASSERT_TRUE(client.connect({}, nullptr));

    // It IS listed — the user can see it — but it is not one number.
    const auto listed = client.listSymbols(nullptr);
    bool sawStruct = false;
    for (const auto& s : listed)
        if (s.name == "Mock.stAxis") { sawStruct = true; EXPECT_TRUE(s.unsupported); }
    EXPECT_TRUE(sawStruct) << "an opaque struct should still be visible";

    QString err;
    EXPECT_FALSE(client.resolveSymbol("Mock.stAxis", &err).has_value());
    EXPECT_TRUE(err.contains("member")) << err.toStdString();
    EXPECT_TRUE(err.contains("Mock.stAxis.fActPos"))
        << "the message should show the way forward, got: " << err.toStdString();
}

TEST(ResolveByName, DifferentMemberTypesAndOffsetsComeBackDistinct) {
    scope::ads::MockAdsClient client;
    ASSERT_TRUE(client.connect({}, nullptr));

    const auto pos   = client.resolveSymbol("Mock.stAxis.fActPos", nullptr);
    const auto state = client.resolveSymbol("Mock.stAxis.nState", nullptr);
    const auto en    = client.resolveSymbol("Mock.stAxis.bEnabled", nullptr);
    ASSERT_TRUE(pos && state && en);

    EXPECT_EQ(static_cast<int>(state->dataType), static_cast<int>(DataType::Int32));
    EXPECT_EQ(state->size, 4u);
    EXPECT_EQ(static_cast<int>(en->dataType), static_cast<int>(DataType::Bool));
    EXPECT_EQ(en->size, 1u);
    // Each member has its own address — that is what makes it recordable.
    EXPECT_NE(pos->indexOffset, state->indexOffset);
    EXPECT_NE(state->indexOffset, en->indexOffset);
}

TEST(ResolveByName, UnknownNamesFailWithAMessageRatherThanASilentEmpty) {
    scope::ads::MockAdsClient client;
    ASSERT_TRUE(client.connect({}, nullptr));
    QString err;
    EXPECT_FALSE(client.resolveSymbol("MAIN.nope.nope", &err).has_value());
    EXPECT_FALSE(err.isEmpty());
    EXPECT_TRUE(err.contains("MAIN.nope.nope")) << err.toStdString();
}

TEST(ResolveByName, RefusesWhenNotConnected) {
    scope::ads::MockAdsClient client;
    QString err;
    EXPECT_FALSE(client.resolveSymbol("Mock.stAxis.fActPos", &err).has_value());
    EXPECT_FALSE(err.isEmpty());
}
