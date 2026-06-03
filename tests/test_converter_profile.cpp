#include "scope/converter/ConverterProfile.h"

#include <gtest/gtest.h>

#include <filesystem>

using namespace scope::converter;

TEST(ConverterProfile, RoundTrip) {
    auto path = std::filesystem::temp_directory_path() / "scope_converter_profile.scaconv";

    ConverterProfile p;
    p.sourceType = "excel";
    p.sheet = "Sheet1";
    p.range = "A2:C100";
    p.headerRow = 1;
    p.columns.push_back({"A", ColumnMapping::Role::XTime, "", "s"});
    p.columns.push_back({"B", ColumnMapping::Role::Signal, "Speed", "rpm"});
    p.columns.push_back({"C", ColumnMapping::Role::Ignore, "", ""});

    ASSERT_TRUE(p.saveToFile(path));
    auto p2 = ConverterProfile::loadFromFile(path);
    EXPECT_EQ(p2.sourceType, "excel");
    EXPECT_EQ(p2.sheet, "Sheet1");
    EXPECT_EQ(p2.range, "A2:C100");
    ASSERT_EQ(p2.columns.size(), 3u);
    EXPECT_EQ(p2.columns[1].role, ColumnMapping::Role::Signal);
    EXPECT_EQ(p2.columns[1].signalName, "Speed");
    EXPECT_EQ(p2.columns[1].unit, "rpm");

    std::filesystem::remove(path);
}
