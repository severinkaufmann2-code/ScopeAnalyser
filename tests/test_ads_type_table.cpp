// The ADS data-type table parser, tested against a real TwinCAT 3 capture.
//
// This is the point of tests/data/ads/: the table's wire layout is not in the
// Beckhoff headers this project vendors, so a parser written from documentation
// could only ever be tested against my own reading of the format. These bytes
// came off a live PLC (see tools/ads_dt_probe), so the assertions below are
// against ground truth — the member names, offsets and sizes here are what
// TwinCAT actually reports.
//
// The fixture PLC declares, among TwinCAT's own library types:
//   ST_LibVersion      6 members (iMajor/iMinor/iBuild/iRevision UINT,
//                      nFlags DWORD, sVersion STRING(23))
//   PlcTaskSystemInfo  12 members, incl. DcTaskTime LINT at +16 and
//                      TaskName STRING(63) at +64
//   TwinCAT_SystemInfoVarList._TaskInfo : ARRAY [1..2] OF PlcTaskSystemInfo
// plus the variables that make the hard cases real:
//   MAIN.testStruct : a small user struct (REAL, INT, BOOL)
//   MAIN.testAxis   : a Tc2_MC2.AXIS_REF — nested structures, an
//                     ARRAY [0..127] OF DWORD, and 200-odd BIT members

#include "scope/ads/AdsTypeTable.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QFile>

#include <cstddef>
#include <span>
#include <vector>

using namespace scope::ads;
using namespace scope::core;

namespace {

QByteArray fixture(const char* name) {
    QFile f(QStringLiteral(SCOPE_SOURCE_DIR) + "/tests/data/ads/" + name);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

std::span<const std::byte> bytesOf(const QByteArray& a) {
    return {reinterpret_cast<const std::byte*>(a.constData()),
            static_cast<std::size_t>(a.size())};
}

const AdsTypeTable& realTable() {
    static const QByteArray blob = fixture("ads_datatypes.bin");
    static const AdsTypeTable t = parseAdsTypeTable(bytesOf(blob));
    return t;
}

// A symbol as listSymbols builds it, matching the fixture's real addresses.
AdsSymbol sym(const char* name, const char* type, std::uint32_t size,
              std::uint32_t offset) {
    AdsSymbol s;
    s.name = name;
    s.typeName = type;
    s.size = size;
    s.indexGroup = 0x4040;      // what the fixture PLC reports
    s.indexOffset = offset;
    s.adsDataType = 65;         // ADST_BIGTYPE
    s.unsupported = true;
    return s;
}

}  // namespace

TEST(AdsTypeTable, ParsesTheWholeRealBlobExactly) {
    const QByteArray blob = fixture("ads_datatypes.bin");
    ASSERT_FALSE(blob.isEmpty()) << "tests/data/ads/ads_datatypes.bin missing";
    EXPECT_EQ(blob.size(), 64656) << "fixture changed";

    QString warn;
    const AdsTypeTable t = parseAdsTypeTable(bytesOf(blob), &warn);
    // The PLC's upload info said 106 entries. Consuming the blob and landing
    // on exactly that count is what proves the layout is right — a wrong
    // header size would desynchronise and stop early.
    EXPECT_EQ(t.size(), 106) << warn.toStdString();
    EXPECT_TRUE(warn.isEmpty()) << warn.toStdString();
}

TEST(AdsTypeTable, ReadsStructureMembersWithTheirRealOffsets) {
    const auto it = realTable().constFind("ST_LIBVERSION");
    ASSERT_NE(it, realTable().constEnd());
    EXPECT_EQ(it->size, 36u);
    ASSERT_EQ(it->members.size(), 6u);

    EXPECT_EQ(it->members[0].name.toStdString(), "iMajor");
    EXPECT_EQ(it->members[0].typeName.toStdString(), "UINT");
    EXPECT_EQ(it->members[0].offset, 0u);
    EXPECT_EQ(it->members[0].size, 2u);

    EXPECT_EQ(it->members[4].name.toStdString(), "nFlags");
    EXPECT_EQ(it->members[4].typeName.toStdString(), "DWORD");
    EXPECT_EQ(it->members[4].offset, 8u);

    // A string member: present in the table, but not recordable.
    EXPECT_EQ(it->members[5].name.toStdString(), "sVersion");
    EXPECT_EQ(it->members[5].offset, 12u);
}

TEST(AdsTypeTable, HandlesAMemberWithPaddingBeforeIt) {
    const auto it = realTable().constFind("PLCTASKSYSTEMINFO");
    ASSERT_NE(it, realTable().constEnd());
    EXPECT_EQ(it->size, 128u);
    ASSERT_EQ(it->members.size(), 12u);
    EXPECT_EQ(it->members[5].name.toStdString(), "DcTaskTime");
    EXPECT_EQ(it->members[5].offset, 16u);
    // TaskName sits at +64, well past the 32 bytes the fields before it use —
    // the offsets are the PLC's, never computed by accumulating sizes.
    EXPECT_EQ(it->members[11].name.toStdString(), "TaskName");
    EXPECT_EQ(it->members[11].offset, 64u);
}

TEST(AdsTypeTable, ReadsArrayTypesIncludingTheirLowerBound) {
    const auto it = realTable().constFind("ARRAY [1..2] OF PLCTASKSYSTEMINFO");
    ASSERT_NE(it, realTable().constEnd());
    ASSERT_EQ(it->dims.size(), 1u);
    EXPECT_EQ(it->dims[0].lower, 1) << "a 1-based array, as declared";
    EXPECT_EQ(it->dims[0].count(), 2);
    EXPECT_EQ(it->elementTypeName.toStdString(), "PlcTaskSystemInfo");
    EXPECT_EQ(it->size, 256u);
}

// The whole point: a structure symbol becomes recordable channels.
TEST(TypeTableExpansion, ExpandsARealStructureIntoItsMembers) {
    QString warn;
    const auto out = expandWithTypeTable(
        sym("Global_Version.stLibVersion_Tc2_Standard", "ST_LibVersion", 36,
            0x5DC00),
        realTable(), 4096, &warn);

    // Five numeric members; sVersion is a STRING and is left out.
    ASSERT_EQ(out.size(), 5u) << warn.toStdString();
    EXPECT_EQ(out[0].name.toStdString(),
              "Global_Version.stLibVersion_Tc2_Standard.iMajor");
    EXPECT_EQ(static_cast<int>(out[0].dataType), static_cast<int>(DataType::Uint16));
    EXPECT_EQ(out[0].indexOffset, 0x5DC00u) << "first member is at the base";
    EXPECT_EQ(out[3].indexOffset, 0x5DC00u + 6);
    EXPECT_EQ(out[4].name.toStdString(),
              "Global_Version.stLibVersion_Tc2_Standard.nFlags");
    EXPECT_EQ(static_cast<int>(out[4].dataType), static_cast<int>(DataType::Uint32));
    EXPECT_EQ(out[4].indexOffset, 0x5DC00u + 8);

    for (const auto& e : out) {
        EXPECT_FALSE(e.unsupported) << "every emitted leaf must be recordable";
        EXPECT_EQ(e.indexGroup, 0x4040u) << "members share the symbol's group";
        EXPECT_EQ(e.arrayLen, 1u);
    }
    EXPECT_TRUE(warn.contains("sVersion")) << "skipped members are reported";
}

// Array of structures — the case neither the type-name parser nor by-name
// resolution alone could cover.
TEST(TypeTableExpansion, ExpandsAnArrayOfStructuresElementByMember) {
    QString warn;
    const auto out = expandWithTypeTable(
        sym("TwinCAT_SystemInfoVarList._TaskInfo",
            "ARRAY [1..2] OF PlcTaskSystemInfo", 256, 0x5EB98),
        realTable(), 4096, &warn);

    // 2 elements x 11 numeric members (TaskName is a STRING).
    ASSERT_EQ(out.size(), 22u) << warn.toStdString();
    EXPECT_EQ(out[0].name.toStdString(),
              "TwinCAT_SystemInfoVarList._TaskInfo[1].ObjId")
        << "the declared lower bound shows in the name";
    EXPECT_EQ(out[0].indexOffset, 0x5EB98u);

    // Element 2 starts one element size (256/2 = 128) further on.
    EXPECT_EQ(out[11].name.toStdString(),
              "TwinCAT_SystemInfoVarList._TaskInfo[2].ObjId");
    EXPECT_EQ(out[11].indexOffset, 0x5EB98u + 128);

    // …and member offsets accumulate on top of the element base.
    EXPECT_EQ(out[16].name.toStdString(),
              "TwinCAT_SystemInfoVarList._TaskInfo[2].DcTaskTime");
    EXPECT_EQ(out[16].indexOffset, 0x5EB98u + 128 + 16);
    EXPECT_EQ(static_cast<int>(out[16].dataType), static_cast<int>(DataType::Int64));
}

TEST(TypeTableExpansion, LeavesScalarSymbolsAlone) {
    QString warn;
    AdsSymbol s;
    s.name = "MAIN.testreal";
    s.typeName = "REAL";
    s.size = 4;
    s.indexOffset = 0x60088;
    s.adsDataType = 4;
    EXPECT_TRUE(expandWithTypeTable(s, realTable(), 4096, &warn).empty())
        << "a scalar is already recordable; expanding it would duplicate it";
}

TEST(TypeTableExpansion, StopsAtTheLeafCapRatherThanFloodingTheList) {
    QString warn;
    const auto out = expandWithTypeTable(
        sym("TwinCAT_SystemInfoVarList._TaskInfo",
            "ARRAY [1..2] OF PlcTaskSystemInfo", 256, 0x5EB98),
        realTable(), /*maxLeaves=*/5, &warn);
    EXPECT_EQ(out.size(), 5u);
}

TEST(AdsTypeTable, MalformedInputStopsCleanlyInsteadOfWalkingOffTheEnd) {
    QByteArray blob = fixture("ads_datatypes.bin");
    ASSERT_FALSE(blob.isEmpty());
    blob.truncate(blob.size() / 2);          // cut mid-entry

    QString warn;
    const AdsTypeTable t = parseAdsTypeTable(bytesOf(blob), &warn);
    EXPECT_GT(t.size(), 0) << "the entries before the cut are still usable";
    EXPECT_LT(t.size(), 106);
    EXPECT_FALSE(warn.isEmpty()) << "a truncated table must be reported";

    QString warn2;
    const QByteArray junk(64, '\xff');       // entryLength would be huge
    EXPECT_TRUE(parseAdsTypeTable(bytesOf(junk), &warn2).isEmpty());
}

// ---------------------------------------------------------------------------
// BIT members. Found by running the expansion against a real AXIS_REF: a BIT
// member's "offset" is a BIT INDEX, not a byte offset, and TwinCAT addresses
// it at index group 0x4041 with the whole address in bits. Treating it as a
// byte offset produced addresses that were wrong by a factor of eight — it
// would have read a neighbouring byte and called it the bit's value.
// ---------------------------------------------------------------------------

TEST(AdsTypeTable, MarksBitMembersAsBitAddressed) {
    const auto it = realTable().constFind("MC.NCTOPLC_AXIS_REF_STATE");
    ASSERT_NE(it, realTable().constEnd()) << "AXIS_REF state word missing";
    ASSERT_GE(it->members.size(), 3u);

    // A 4-byte status word whose members are consecutive BITS: index 0, 1, 2 …
    // If these were read as byte offsets, member 2 would land 2 bytes in.
    EXPECT_EQ(it->size, 4u);
    EXPECT_EQ(it->members[0].name.toStdString(), "Operational");
    EXPECT_TRUE(it->members[0].bitValue);
    EXPECT_EQ(it->members[0].typeName.toStdString(), "BIT");
    EXPECT_EQ(it->members[0].offset, 0u);
    EXPECT_EQ(it->members[1].name.toStdString(), "Homed");
    EXPECT_EQ(it->members[1].offset, 1u);
    EXPECT_EQ(it->members[2].offset, 2u);
}

TEST(TypeTableExpansion, BitMembersGetTheBitAccessGroupAndABitOffset) {
    // MAIN.testAxis.PlcToNc.ControlDWord sits at byte 0x60A98 on the fixture
    // PLC; SYM_INFOBYNAMEEX reports its first bit as 0x4041:0x3054C0, and
    // 0x60A98 * 8 == 0x3054C0.
    QString warn;
    const auto out = expandWithTypeTable(
        sym("MAIN.testAxis.PlcToNc", "MC.PLCTONC_AXIS_REF", 128, 0x60A98),
        realTable(), 4096, &warn);
    ASSERT_FALSE(out.empty()) << warn.toStdString();

    const AdsSymbol* enable = nullptr;
    for (const auto& e : out)
        if (e.name.endsWith(".ControlDWord.Enable")) { enable = &e; break; }
    ASSERT_NE(enable, nullptr) << "ControlDWord.Enable not expanded";

    EXPECT_EQ(enable->indexGroup, 0x4041u)
        << "a bit is read at the bit-access group, not the symbol's own";
    EXPECT_EQ(enable->indexOffset, 0x60A98u * 8)
        << "and its address is in bits";
    EXPECT_EQ(enable->size, 1u);
    EXPECT_EQ(static_cast<int>(enable->dataType), static_cast<int>(DataType::Bool));

    // The next bit is one BIT further on, not one byte.
    const AdsSymbol* feed = nullptr;
    for (const auto& e : out)
        if (e.name.endsWith(".ControlDWord.FeedEnablePlus")) { feed = &e; break; }
    ASSERT_NE(feed, nullptr);
    EXPECT_EQ(feed->indexOffset, enable->indexOffset + 1);
}

// A plain byte-addressed member of the same structure must be unaffected.
TEST(TypeTableExpansion, NonBitMembersKeepTheSymbolsOwnGroupAndByteOffset) {
    QString warn;
    const auto out = expandWithTypeTable(
        sym("MAIN.testAxis.PlcToNc", "MC.PLCTONC_AXIS_REF", 128, 0x60A98),
        realTable(), 4096, &warn);

    const AdsSymbol* over = nullptr;
    for (const auto& e : out)
        if (e.name.endsWith(".Override")) { over = &e; break; }
    ASSERT_NE(over, nullptr);
    EXPECT_EQ(over->indexGroup, 0x4040u);
    EXPECT_EQ(over->indexOffset, 0x60A98u + 4) << "Override is at byte +4";
    EXPECT_EQ(static_cast<int>(over->dataType), static_cast<int>(DataType::Uint32));
}

TEST(TypeTableExpansion, ExpandsAUserStructureFromTheFixture) {
    QString warn;
    const auto out = expandWithTypeTable(
        sym("MAIN.testStruct", "teststruct", 8, 0x5FCF0), realTable(), 4096, &warn);
    ASSERT_EQ(out.size(), 3u) << warn.toStdString();
    EXPECT_EQ(out[0].name.toStdString(), "MAIN.testStruct.testvalueReal");
    EXPECT_EQ(out[0].indexOffset, 0x5FCF0u);
    EXPECT_EQ(static_cast<int>(out[0].dataType), static_cast<int>(DataType::Float32));
    EXPECT_EQ(out[1].name.toStdString(), "MAIN.testStruct.testValueInt");
    EXPECT_EQ(out[1].indexOffset, 0x5FCF0u + 4);
    EXPECT_EQ(out[2].name.toStdString(), "MAIN.testStruct.testValueBool");
    EXPECT_EQ(out[2].indexOffset, 0x5FCF0u + 6);
}
