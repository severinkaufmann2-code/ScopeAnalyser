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

TEST(CsvConverter, SampleRateXAxisDefaultMs) {
    auto path = std::filesystem::temp_directory_path() / "scope_test_csv_rate.csv";
    {
        std::ofstream f(path);
        f << "v\n0\n1\n2\n3\n4\n";
    }
    CsvSource src(path);

    ConverterProfile p;
    p.headerRow = 1;
    p.useSampleRate = true;
    p.sampleRateDisplayUnit = "ms";
    p.sampleRateHz = 1000.0;       // 1 ms → 1 kHz
    p.columns = { {"A", ColumnMapping::Role::Signal, "V", "", -1, -1} };
    QString err;
    auto sigs = src.apply(p, &err);
    ASSERT_EQ(sigs.size(), 1u) << err.toStdString();
    auto view = sigs[0]->snapshotForRead();
    ASSERT_EQ(view.count, 5u);
    EXPECT_EQ(view.timestamps[0], 0);
    EXPECT_EQ(view.timestamps[1], 1'000'000);  // 1 ms in ns
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
    // Take rows 4..8 (0-based), which correspond to data rows where t=3..7.
    p.columns = {
        {"A", ColumnMapping::Role::XTime,  "",  "s",  4, 8},
        {"B", ColumnMapping::Role::Signal, "V", "V",  4, 8},
    };
    QString err;
    auto sigs = src.apply(p, &err);
    ASSERT_EQ(sigs.size(), 1u) << err.toStdString();
    EXPECT_EQ(sigs[0]->sampleCount(), 5u);
    auto vs = sigs[0]->readAsDouble();
    EXPECT_DOUBLE_EQ(vs[0], 30.0);   // row 4 → t=3, v=30
    EXPECT_DOUBLE_EQ(vs[4], 70.0);   // row 8 → t=7, v=70
    std::filesystem::remove(path);
}

TEST(CsvConverter, ResetTimestampsToZero) {
    auto path = std::filesystem::temp_directory_path() / "scope_test_csv_reset_zero.csv";
    {
        // X column starts at 5.0 s, not 0.
        std::ofstream f(path);
        f << "t,v\n";
        for (int i = 0; i < 5; ++i) f << (5.0 + i * 0.1) << "," << i << "\n";
    }
    CsvSource src(path);

    ConverterProfile p;
    p.headerRow = 1;
    ColumnMapping x;
    x.columnId = "A"; x.role = ColumnMapping::Role::XTime; x.unit = "s";
    ColumnMapping y;
    y.columnId = "B"; y.role = ColumnMapping::Role::Signal;
    y.signalName = "V"; y.xSourceColumn = "A";
    y.resetTimeToZero = true;            // <-- the new option
    p.columns = {x, y};

    QString err;
    auto sigs = src.apply(p, &err);
    ASSERT_EQ(sigs.size(), 1u) << err.toStdString();
    auto view = sigs[0]->snapshotForRead();
    ASSERT_EQ(view.count, 5u);
    // First sample at 0, subsequent +0.1 s.
    EXPECT_EQ(view.timestamps[0], 0);
    EXPECT_EQ(view.timestamps[1], 100'000'000);
    EXPECT_EQ(view.timestamps[4], 400'000'000);
    std::filesystem::remove(path);
}

TEST(CsvConverter, PerChannelSampleRate) {
    auto path = std::filesystem::temp_directory_path() / "scope_test_csv_per_ch_rate.csv";
    {
        std::ofstream f(path);
        f << "fast,slow\n";
        for (int i = 0; i < 5; ++i) f << i << "," << (i * 10) << "\n";
    }
    CsvSource src(path);

    ConverterProfile p;
    p.headerRow = 1;
    // Two Y channels with DIFFERENT sample rates — no X column at all.
    ColumnMapping fast;
    fast.columnId = "A"; fast.role = ColumnMapping::Role::Signal;
    fast.signalName = "Fast"; fast.useSampleRate = true;
    fast.sampleRateHz = 1000.0;             // 1 ms tick
    ColumnMapping slow;
    slow.columnId = "B"; slow.role = ColumnMapping::Role::Signal;
    slow.signalName = "Slow"; slow.useSampleRate = true;
    slow.sampleRateHz = 100.0;              // 10 ms tick
    p.columns = {fast, slow};

    QString err;
    auto sigs = src.apply(p, &err);
    ASSERT_EQ(sigs.size(), 2u) << err.toStdString();

    auto findByName = [&](const QString& name) -> std::shared_ptr<Signal> {
        for (const auto& s : sigs) if (s->meta().name == name) return s;
        return nullptr;
    };
    auto fastSig = findByName("Fast");
    auto slowSig = findByName("Slow");
    ASSERT_TRUE(fastSig);
    ASSERT_TRUE(slowSig);
    // Both 5 samples, same data rows, different timestamps.
    EXPECT_EQ(fastSig->sampleCount(), 5u);
    EXPECT_EQ(slowSig->sampleCount(), 5u);
    auto fv = fastSig->snapshotForRead();
    auto sv = slowSig->snapshotForRead();
    EXPECT_EQ(fv.timestamps[1], 1'000'000);    // 1 ms
    EXPECT_EQ(sv.timestamps[1], 10'000'000);   // 10 ms
    EXPECT_EQ(fastSig->meta().sampleRateHz, 1000.0);
    EXPECT_EQ(slowSig->meta().sampleRateHz, 100.0);
    std::filesystem::remove(path);
}

TEST(CsvConverter, MixedXSourceAndRate) {
    auto path = std::filesystem::temp_directory_path() / "scope_test_csv_mixed.csv";
    {
        std::ofstream f(path);
        f << "t,a,b\n";
        for (int i = 0; i < 4; ++i) f << (i * 0.1) << "," << i << "," << (i * 100) << "\n";
    }
    CsvSource src(path);

    ConverterProfile p;
    p.headerRow = 1;
    ColumnMapping x;
    x.columnId = "A"; x.role = ColumnMapping::Role::XTime; x.unit = "s";
    ColumnMapping yA;
    yA.columnId = "B"; yA.role = ColumnMapping::Role::Signal;
    yA.signalName = "FromX"; yA.xSourceColumn = "A";
    ColumnMapping yB;
    yB.columnId = "C"; yB.role = ColumnMapping::Role::Signal;
    yB.signalName = "FromRate"; yB.useSampleRate = true;
    yB.sampleRateHz = 500.0;   // 2 ms tick
    p.columns = {x, yA, yB};

    QString err;
    auto sigs = src.apply(p, &err);
    ASSERT_EQ(sigs.size(), 2u) << err.toStdString();

    auto findByName = [&](const QString& name) -> std::shared_ptr<Signal> {
        for (const auto& s : sigs) if (s->meta().name == name) return s;
        return nullptr;
    };
    auto fromX = findByName("FromX");
    auto fromR = findByName("FromRate");
    ASSERT_TRUE(fromX);
    ASSERT_TRUE(fromR);

    // FromX: timestamps come from column A (0.0, 0.1, 0.2, 0.3 seconds)
    auto xView = fromX->snapshotForRead();
    EXPECT_EQ(xView.timestamps[1], 100'000'000);  // 0.1 s = 1e8 ns

    // FromRate: timestamps come from sample rate (0, 2 ms, 4 ms, 6 ms)
    auto rView = fromR->snapshotForRead();
    EXPECT_EQ(rView.timestamps[1], 2'000'000);    // 2 ms = 2e6 ns

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
