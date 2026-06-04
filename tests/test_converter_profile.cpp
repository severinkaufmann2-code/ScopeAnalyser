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

TEST(ConverterProfile, RoundTripPerChannelXSource) {
    auto path = std::filesystem::temp_directory_path() / "scope_per_channel_x.scaconv";

    ConverterProfile p;
    p.sourceType        = "csv";
    p.headerRow         = 1;
    p.columnDelimiter   = ";";
    p.rowDelimiter      = "\n";
    p.decimalSeparator  = ",";
    // X column with a row-range subselection.
    ColumnMapping x;
    x.columnId = "A"; x.role = ColumnMapping::Role::XTime; x.unit = "s";
    x.rowStart = 4; x.rowEnd = 99;
    // Y signal that uses column A as its X source.
    ColumnMapping yByCol;
    yByCol.columnId = "B"; yByCol.role = ColumnMapping::Role::Signal;
    yByCol.signalName = "Speed"; yByCol.unit = "rpm";
    yByCol.xSourceColumn = "A";
    // Y signal that uses its own sample rate.
    ColumnMapping yByRate;
    yByRate.columnId = "C"; yByRate.role = ColumnMapping::Role::Signal;
    yByRate.signalName = "Pressure"; yByRate.unit = "bar";
    yByRate.useSampleRate = true;
    yByRate.sampleRateHz = 1000.0;
    yByRate.sampleRateDisplayUnit = "ms";
    p.columns = {x, yByCol, yByRate};

    QString err;
    ASSERT_TRUE(p.saveToFile(path, &err)) << err.toStdString();
    auto p2 = ConverterProfile::loadFromFile(path, &err);
    ASSERT_TRUE(err.isEmpty()) << err.toStdString();

    EXPECT_EQ(p2.columnDelimiter,  ";");
    EXPECT_EQ(p2.decimalSeparator, ",");
    ASSERT_EQ(p2.columns.size(), 3u);

    EXPECT_EQ(p2.columns[0].rowStart, 4);
    EXPECT_EQ(p2.columns[0].rowEnd,  99);

    EXPECT_EQ(p2.columns[1].xSourceColumn, "A");
    EXPECT_FALSE(p2.columns[1].useSampleRate);

    EXPECT_TRUE(p2.columns[2].useSampleRate);
    EXPECT_DOUBLE_EQ(p2.columns[2].sampleRateHz, 1000.0);
    EXPECT_EQ(p2.columns[2].sampleRateDisplayUnit, "ms");
    EXPECT_TRUE(p2.columns[2].xSourceColumn.isEmpty());

    std::filesystem::remove(path);
}
