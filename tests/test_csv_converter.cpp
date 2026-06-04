#include "scope/converter/CsvSource.h"
#include "scope/converter/ConverterProfile.h"
#include "scope/core/Signal.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

using namespace scope::converter;
using namespace scope::core;

TEST(CsvConverter, ImportsHeaderedFile) {
    auto path = std::filesystem::temp_directory_path() / "scope_test_csv.csv";
    {
        std::ofstream f(path);
        f << "t_s,speed,torque\n";
        f << "0.0,0.0,0.0\n";
        f << "0.1,10.0,1.5\n";
        f << "0.2,20.0,3.0\n";
        f << "0.3,30.0,4.5\n";
    }

    CsvSource src(path);
    EXPECT_EQ(src.rowCount(), 5);
    EXPECT_EQ(src.columnCount(), 3);

    ConverterProfile p;
    p.sourceType = "csv";
    p.headerRow  = 1;
    p.decimalSeparator = ".";
    p.columns = {
        {"A", ColumnMapping::Role::XTime,  "",       "s"},
        {"B", ColumnMapping::Role::Signal, "Speed",  "rpm"},
        {"C", ColumnMapping::Role::Signal, "Torque", "Nm"},
    };

    QString err;
    auto sigs = src.apply(p, &err);
    ASSERT_EQ(sigs.size(), 2u) << err.toStdString();

    auto findByName = [&](const QString& name) -> std::shared_ptr<Signal> {
        for (const auto& s : sigs) if (s->meta().name == name) return s;
        return nullptr;
    };

    auto speed = findByName("Speed");
    auto torque = findByName("Torque");
    ASSERT_TRUE(speed);
    ASSERT_TRUE(torque);
    EXPECT_EQ(speed->sampleCount(), 4u);
    EXPECT_EQ(torque->sampleCount(), 4u);
    EXPECT_EQ(speed->meta().unit, "rpm");
    EXPECT_EQ(torque->meta().unit, "Nm");

    auto vs = speed->readAsDouble();
    EXPECT_DOUBLE_EQ(vs[3], 30.0);

    std::filesystem::remove(path);
}

TEST(CsvConverter, CustomTabDelimiterAndPipeRowDelimiter) {
    auto path = std::filesystem::temp_directory_path() / "scope_test_csv_custom.csv";
    {
        // Tab-separated, pipe-separated rows (all on one line).
        std::ofstream f(path);
        f << "t\tv|0.0\t10.0|0.1\t20.0|0.2\t30.0";
    }
    CsvSource src(path, "\t", "|");
    EXPECT_EQ(src.rowCount(), 4);     // header + 3 data rows
    EXPECT_EQ(src.columnCount(), 2);

    ConverterProfile p;
    p.headerRow = 1;
    p.decimalSeparator = ".";
    p.columnDelimiter = "\t";
    p.rowDelimiter    = "|";
    p.columns = {
        {"A", ColumnMapping::Role::XTime,  "",     "s"},
        {"B", ColumnMapping::Role::Signal, "V",    "V"},
    };
    QString err;
    auto sigs = src.apply(p, &err);
    ASSERT_EQ(sigs.size(), 1u) << err.toStdString();
    EXPECT_EQ(sigs[0]->sampleCount(), 3u);
    auto vs = sigs[0]->readAsDouble();
    EXPECT_DOUBLE_EQ(vs[2], 30.0);

    std::filesystem::remove(path);
}

TEST(CsvConverter, SampleRateXAxis) {
    auto path = std::filesystem::temp_directory_path() / "scope_test_csv_rate.csv";
    {
        std::ofstream f(path);
        f << "v\n0\n1\n2\n3\n4\n";
    }
    CsvSource src(path);

    ConverterProfile p;
    p.headerRow = 1;
    p.useSampleRate = true;
    p.sampleRateHz  = 1000.0;   // 1 kHz → dt = 1 ms = 1e6 ns
    p.columns = { {"A", ColumnMapping::Role::Signal, "V", "", -1, -1} };
    QString err;
    auto sigs = src.apply(p, &err);
    ASSERT_EQ(sigs.size(), 1u) << err.toStdString();
    auto view = sigs[0]->snapshotForRead();
    ASSERT_EQ(view.count, 5u);
    EXPECT_EQ(view.timestamps[0], 0);
    EXPECT_EQ(view.timestamps[1], 1'000'000);
    EXPECT_EQ(view.timestamps[4], 4'000'000);
    std::filesystem::remove(path);
}

TEST(CsvConverter, PerColumnRowRange) {
    auto path = std::filesystem::temp_directory_path() / "scope_test_csv_range.csv";
    {
        std::ofstream f(path);
        f << "t,v\n";
        for (int i = 0; i < 10; ++i) f << i << "," << (i * 10) << "\n";
    }
    CsvSource src(path);

    ConverterProfile p;
    p.headerRow = 1;
    // Subselect rows 3..7 (0-based indexes), which correspond to data rows 3-7.
    p.columns = {
        {"A", ColumnMapping::Role::XTime,  "",  "s",  3, 7},
        {"B", ColumnMapping::Role::Signal, "V", "V",  3, 7},
    };
    QString err;
    auto sigs = src.apply(p, &err);
    ASSERT_EQ(sigs.size(), 1u) << err.toStdString();
    EXPECT_EQ(sigs[0]->sampleCount(), 5u);
    auto vs = sigs[0]->readAsDouble();
    EXPECT_DOUBLE_EQ(vs[0], 20.0);   // row index 3 has t=2, v=20
    EXPECT_DOUBLE_EQ(vs[4], 60.0);   // row index 7 has t=6, v=60
    std::filesystem::remove(path);
}

TEST(CsvConverter, HandlesEuropeanDecimals) {
    auto path = std::filesystem::temp_directory_path() / "scope_test_csv_eu.csv";
    {
        std::ofstream f(path);
        f << "tijd;snelheid\n";
        f << "0,0;0,0\n";
        f << "0,1;1,5\n";
    }
    CsvSource src(path, ";");
    EXPECT_EQ(src.columnCount(), 2);

    ConverterProfile p;
    p.headerRow = 1;
    p.decimalSeparator = ",";
    p.columns = {
        {"A", ColumnMapping::Role::XTime,  "",         "s"},
        {"B", ColumnMapping::Role::Signal, "Snelheid", "kph"},
    };
    QString err;
    auto sigs = src.apply(p, &err);
    ASSERT_EQ(sigs.size(), 1u) << err.toStdString();
    auto vs = sigs[0]->readAsDouble();
    EXPECT_DOUBLE_EQ(vs[1], 1.5);

    std::filesystem::remove(path);
}
