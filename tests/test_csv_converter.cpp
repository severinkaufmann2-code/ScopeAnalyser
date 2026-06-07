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

TEST(CsvConverter, FrequencyXAxisStampsDomainAndScalesHz) {
    auto path = std::filesystem::temp_directory_path() / "scope_test_csv_xfreq.csv";
    {
        std::ofstream f(path);
        f << "f_Hz,magnitude\n";
        f << "0,1.0\n";
        f << "50,2.0\n";
        f << "100,3.0\n";
        f << "200,4.0\n";
    }
    CsvSource src(path);

    ConverterProfile p;
    p.sourceType = "csv";
    p.headerRow  = 1;
    p.decimalSeparator = ".";
    p.columns = {
        {"A", ColumnMapping::Role::XFrequency, "",   "Hz"},
        {"B", ColumnMapping::Role::Signal,     "FFT","magnitude"},
    };
    QString err;
    auto sigs = src.apply(p, &err);
    ASSERT_EQ(sigs.size(), 1u) << err.toStdString();

    auto fft = sigs[0];
    EXPECT_EQ(fft->meta().domain, Signal::Domain::Frequency);
    auto view = fft->snapshotForRead();
    ASSERT_EQ(view.count, 4u);
    // Stored as Hz × 1e9 in the int64 master field — same convention
    // the in-app FFT output uses.
    EXPECT_EQ(view.timestamps[0],         0);
    EXPECT_EQ(view.timestamps[1],   50'000'000'000LL);
    EXPECT_EQ(view.timestamps[2],  100'000'000'000LL);
    EXPECT_EQ(view.timestamps[3],  200'000'000'000LL);
    auto vs = fft->readAsDouble();
    EXPECT_DOUBLE_EQ(vs[2], 3.0);
    std::filesystem::remove(path);
}

TEST(CsvConverter, FrequencyXAxisScalesKHz) {
    auto path = std::filesystem::temp_directory_path() / "scope_test_csv_xfreq_khz.csv";
    {
        std::ofstream f(path);
        f << "f,mag\n";
        f << "1.5,10.0\n";   // 1.5 kHz
        f << "2.5,20.0\n";   // 2.5 kHz
    }
    CsvSource src(path);

    ConverterProfile p;
    p.sourceType = "csv";
    p.headerRow  = 1;
    p.decimalSeparator = ".";
    p.columns = {
        {"A", ColumnMapping::Role::XFrequency, "",   "kHz"},
        {"B", ColumnMapping::Role::Signal,     "FFT","magnitude"},
    };
    QString err;
    auto sigs = src.apply(p, &err);
    ASSERT_EQ(sigs.size(), 1u) << err.toStdString();
    auto view = sigs[0]->snapshotForRead();
    ASSERT_EQ(view.count, 2u);
    EXPECT_EQ(view.timestamps[0], 1'500'000'000'000LL);  // 1.5 kHz × 1e9
    EXPECT_EQ(view.timestamps[1], 2'500'000'000'000LL);  // 2.5 kHz × 1e9
    std::filesystem::remove(path);
}

// profileFromScopeMetadata: the "Open chart…" CSV path uses this to
// pre-fill the mapping panel from a "# scope-csv:" header so the user
// just clicks Apply.
TEST(CsvConverter, ProfileFromScopeMetadataPrefillsMapping) {
    auto path = std::filesystem::temp_directory_path() / "scope_test_csv_prefill.csv";
    {
        std::ofstream f(path);
        f << "# scope-csv: {\"v\":1,\"columns\":["
             "{\"role\":\"x_time\",\"unit\":\"s\"},"
             "{\"role\":\"signal\",\"unit\":\"rpm\",\"name\":\"speed\",\"refX\":0},"
             "{\"role\":\"x_frequency\",\"unit\":\"Hz\"},"
             "{\"role\":\"signal\",\"unit\":\"mag\",\"name\":\"FFT_speed\",\"refX\":2}"
             "]}\n";
        f << "t [s],speed [rpm],f [Hz],FFT_speed [mag]\n";
        f << "0,0,0,1\n";
    }
    ConverterProfile prof;
    QString err;
    ASSERT_TRUE(profileFromScopeMetadata(path, &prof, &err)) << err.toStdString();
    ASSERT_EQ(prof.columns.size(), 4u);
    EXPECT_EQ(prof.columns[0].columnId,   "A");
    EXPECT_EQ(prof.columns[0].role,       ColumnMapping::Role::XTime);
    EXPECT_EQ(prof.columns[0].unit,       "s");
    EXPECT_EQ(prof.columns[1].columnId,   "B");
    EXPECT_EQ(prof.columns[1].role,       ColumnMapping::Role::Signal);
    EXPECT_EQ(prof.columns[1].unit,       "rpm");
    EXPECT_EQ(prof.columns[1].signalName, "speed");
    EXPECT_EQ(prof.columns[1].xSourceColumn, "A");
    EXPECT_EQ(prof.columns[2].columnId,   "C");
    EXPECT_EQ(prof.columns[2].role,       ColumnMapping::Role::XFrequency);
    EXPECT_EQ(prof.columns[2].unit,       "Hz");
    EXPECT_EQ(prof.columns[3].columnId,   "D");
    EXPECT_EQ(prof.columns[3].role,       ColumnMapping::Role::Signal);
    EXPECT_EQ(prof.columns[3].signalName, "FFT_speed");
    EXPECT_EQ(prof.columns[3].xSourceColumn, "C");
    std::filesystem::remove(path);
}

TEST(CsvConverter, ProfileFromScopeMetadataReturnsFalseWithoutHeader) {
    auto path = std::filesystem::temp_directory_path() / "scope_test_csv_no_meta.csv";
    {
        std::ofstream f(path);
        f << "t,v\n0,1\n0.1,2\n";
    }
    ConverterProfile prof;
    QString err;
    EXPECT_FALSE(profileFromScopeMetadata(path, &prof, &err));
    EXPECT_TRUE(err.isEmpty());
    EXPECT_TRUE(prof.columns.empty());
    std::filesystem::remove(path);
}

// Row range typed as "1:*" pulls the header line into the parsed range.
// toDouble can't parse text → 0 silently. We expect one warning per
// affected column (one for Y, one for X) telling the user what was
// turned into zero and where.
TEST(CsvConverter, RangeOverHeaderRowProducesNonNumericWarning) {
    auto path = std::filesystem::temp_directory_path() / "scope_test_csv_text_in_range.csv";
    {
        std::ofstream f(path);
        f << "t_s,speed\n";   // preview row 1
        f << "0,1.0\n";       // preview row 2
        f << "0.1,2.0\n";     // preview row 3
    }
    CsvSource src(path);

    ConverterProfile p;
    p.sourceType = "csv";
    p.headerRow  = 1;
    p.columns = {
        {"A", ColumnMapping::Role::XTime,  "",      "s",   /*rowStart=*/0, /*rowEnd=*/-1},
        {"B", ColumnMapping::Role::Signal, "Speed", "rpm", /*rowStart=*/0, /*rowEnd=*/-1},
    };
    QString err;
    QStringList warnings;
    auto sigs = src.apply(p, &err, &warnings);
    ASSERT_EQ(sigs.size(), 1u) << err.toStdString();

    // Speed signal got the header row parsed as (t=0, v=0) plus the two
    // real data rows.
    EXPECT_EQ(sigs[0]->sampleCount(), 3u);

    // Two warnings: one for the Y column "B" and one for the X column "A".
    ASSERT_EQ(warnings.size(), 2);
    bool sawY = false, sawX = false;
    for (const auto& w : warnings) {
        if (w.contains("Column B") && w.contains("Y signal 'Speed'")
            && w.contains("\"speed\"")     // the header cell
            && w.contains("preview row 1"))
            sawY = true;
        if (w.contains("Column A") && w.contains("X-axis")
            && w.contains("\"t_s\"")
            && w.contains("preview row 1"))
            sawX = true;
    }
    EXPECT_TRUE(sawY) << warnings.join("\n").toStdString();
    EXPECT_TRUE(sawX) << warnings.join("\n").toStdString();
    std::filesystem::remove(path);
}

TEST(CsvConverter, RangeAllSkipsHeaderAndProducesNoNonNumericWarning) {
    auto path = std::filesystem::temp_directory_path() / "scope_test_csv_text_safe.csv";
    {
        std::ofstream f(path);
        f << "t_s,speed\n";
        f << "0,1.0\n";
        f << "0.1,2.0\n";
    }
    CsvSource src(path);

    ConverterProfile p;
    p.sourceType = "csv";
    p.headerRow  = 1;
    // rowStart = -1 / rowEnd = -1 → "all", which uses the headerRow
    // setting to skip preview row 1.
    p.columns = {
        {"A", ColumnMapping::Role::XTime,  "",      "s",   -1, -1},
        {"B", ColumnMapping::Role::Signal, "Speed", "rpm", -1, -1},
    };
    QString err;
    QStringList warnings;
    auto sigs = src.apply(p, &err, &warnings);
    ASSERT_EQ(sigs.size(), 1u) << err.toStdString();
    EXPECT_EQ(sigs[0]->sampleCount(), 2u);   // header skipped, two data rows
    // The mismatched-unit checks could still fire (they don't here —
    // "s" is on the ladder), so the count we actually care about is
    // "no non-numeric warnings".
    for (const auto& w : warnings) {
        EXPECT_FALSE(w.contains("non-numeric"))
            << "unexpected: " << w.toStdString();
    }
    std::filesystem::remove(path);
}

TEST(CsvConverter, EmptyCellsAreNotFlaggedAsNonNumeric) {
    // Sparse CSVs are common — empty cells should silently become 0 and
    // NOT produce a warning. Only non-empty unparseable text triggers
    // the warning.
    auto path = std::filesystem::temp_directory_path() / "scope_test_csv_empty_cells.csv";
    {
        std::ofstream f(path);
        f << "t,v\n";
        f << "0,1.0\n";
        f << "0.1,\n";    // missing v
        f << "0.2,3.0\n";
    }
    CsvSource src(path);

    ConverterProfile p;
    p.sourceType = "csv";
    p.headerRow  = 1;
    p.columns = {
        {"A", ColumnMapping::Role::XTime,  "",  "s",   -1, -1},
        {"B", ColumnMapping::Role::Signal, "V", "V",   -1, -1},
    };
    QString err;
    QStringList warnings;
    auto sigs = src.apply(p, &err, &warnings);
    ASSERT_EQ(sigs.size(), 1u) << err.toStdString();
    EXPECT_EQ(sigs[0]->sampleCount(), 3u);
    for (const auto& w : warnings) {
        EXPECT_FALSE(w.contains("non-numeric"))
            << "empty cell shouldn't warn: " << w.toStdString();
    }
    std::filesystem::remove(path);
}

TEST(CsvConverter, BogusUnitOnXTimeProducesWarning) {
    auto path = std::filesystem::temp_directory_path() / "scope_test_csv_bogus_unit.csv";
    {
        std::ofstream f(path);
        f << "t_qws,v\n0.1,1.0\n0.2,2.0\n";
    }
    CsvSource src(path);

    ConverterProfile p;
    p.sourceType = "csv";
    p.headerRow  = 1;
    p.columns = {
        {"A", ColumnMapping::Role::XTime,  "",  "qws"},
        {"B", ColumnMapping::Role::Signal, "V", "V"},
    };
    QString err;
    QStringList warnings;
    auto sigs = src.apply(p, &err, &warnings);
    ASSERT_EQ(sigs.size(), 1u) << err.toStdString();
    ASSERT_EQ(warnings.size(), 1);
    EXPECT_TRUE(warnings[0].contains("Column A"))    << warnings[0].toStdString();
    EXPECT_TRUE(warnings[0].contains("\"qws\""))     << warnings[0].toStdString();
    EXPECT_TRUE(warnings[0].contains("seconds"))     << warnings[0].toStdString();
    // Parser still ran; values landed as seconds (0.1 s = 1e8 ns).
    auto view = sigs[0]->snapshotForRead();
    EXPECT_EQ(view.timestamps[0], 100'000'000);
    std::filesystem::remove(path);
}

TEST(CsvConverter, BogusUnitOnXFrequencyProducesWarning) {
    auto path = std::filesystem::temp_directory_path() / "scope_test_csv_bogus_freq.csv";
    {
        std::ofstream f(path);
        f << "f_bogus,m\n10,1.0\n20,2.0\n";
    }
    CsvSource src(path);

    ConverterProfile p;
    p.sourceType = "csv";
    p.headerRow  = 1;
    p.columns = {
        {"A", ColumnMapping::Role::XFrequency, "",  "bogus"},
        {"B", ColumnMapping::Role::Signal,     "M", "m"},
    };
    QString err;
    QStringList warnings;
    auto sigs = src.apply(p, &err, &warnings);
    ASSERT_EQ(sigs.size(), 1u) << err.toStdString();
    ASSERT_EQ(warnings.size(), 1);
    EXPECT_TRUE(warnings[0].contains("Column A"))   << warnings[0].toStdString();
    EXPECT_TRUE(warnings[0].contains("\"bogus\"")) << warnings[0].toStdString();
    EXPECT_TRUE(warnings[0].contains("Hz"))         << warnings[0].toStdString();
    std::filesystem::remove(path);
}

TEST(CsvConverter, KnownUnitsProduceNoWarning) {
    auto path = std::filesystem::temp_directory_path() / "scope_test_csv_clean_units.csv";
    {
        std::ofstream f(path);
        f << "t_ms,v\n0,1.0\n100,2.0\n";
    }
    CsvSource src(path);
    ConverterProfile p;
    p.sourceType = "csv";
    p.headerRow  = 1;
    p.columns = {
        {"A", ColumnMapping::Role::XTime,  "",  "ms"},
        {"B", ColumnMapping::Role::Signal, "V", "V"},
    };
    QString err;
    QStringList warnings;
    auto sigs = src.apply(p, &err, &warnings);
    ASSERT_EQ(sigs.size(), 1u) << err.toStdString();
    EXPECT_TRUE(warnings.isEmpty()) << warnings.join(", ").toStdString();
    std::filesystem::remove(path);
}

TEST(CsvConverter, MixedTimeAndFrequencyXRequiresExplicitXSource) {
    auto path = std::filesystem::temp_directory_path() / "scope_test_csv_mixed_x.csv";
    {
        std::ofstream f(path);
        f << "t_s,f_Hz,v,m\n";
        f << "0.0,0,1.0,10\n";
        f << "0.1,50,2.0,20\n";
    }
    CsvSource src(path);

    ConverterProfile p;
    p.sourceType = "csv";
    p.headerRow  = 1;
    p.decimalSeparator = ".";
    // Y signals pin no xSourceColumn — ambiguous given two X kinds.
    p.columns = {
        {"A", ColumnMapping::Role::XTime,      "",  "s"},
        {"B", ColumnMapping::Role::XFrequency, "",  "Hz"},
        {"C", ColumnMapping::Role::Signal,     "V", "V"},
        {"D", ColumnMapping::Role::Signal,     "M", "magnitude"},
    };
    QString err;
    auto sigs = src.apply(p, &err);
    EXPECT_TRUE(sigs.empty());
    EXPECT_TRUE(err.contains("pin its X column"))
        << err.toStdString();
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

// Reset-to-zero subtracts the first sample's timestamp.
TEST(CsvConverter, ResetTimeToZeroShiftsFirstSampleToZero) {
    auto path = std::filesystem::temp_directory_path() / "scope_test_csv_reset.csv";
    {
        std::ofstream f(path);
        f << "t,v\n";
        f << "5.0,1\n";
        f << "5.1,2\n";
        f << "5.2,3\n";
    }
    CsvSource src(path);

    ConverterProfile p;
    p.headerRow = 1;
    p.decimalSeparator = ".";
    ColumnMapping x{"A", ColumnMapping::Role::XTime,  "",  "s"};
    ColumnMapping y{"B", ColumnMapping::Role::Signal, "V", "V"};
    y.xSourceColumn   = "A";
    y.resetTimeToZero = true;
    p.columns = { x, y };

    QString err;
    auto sigs = src.apply(p, &err);
    ASSERT_EQ(sigs.size(), 1u) << err.toStdString();
    auto view = sigs[0]->snapshotForRead();
    ASSERT_EQ(view.count, 3u);
    EXPECT_EQ(view.timestamps[0], 0);
    EXPECT_NEAR(view.timestamps[1] / 1e9, 0.1, 1e-9);
    EXPECT_NEAR(view.timestamps[2] / 1e9, 0.2, 1e-9);
    std::filesystem::remove(path);
}

// Offset shifts every timestamp by a constant amount of seconds.
TEST(CsvConverter, TimeOffsetSecAddsConstantShift) {
    auto path = std::filesystem::temp_directory_path() / "scope_test_csv_offset.csv";
    {
        std::ofstream f(path);
        f << "t,v\n";
        f << "0.0,1\n";
        f << "0.1,2\n";
    }
    CsvSource src(path);

    ConverterProfile p;
    p.headerRow = 1;
    p.decimalSeparator = ".";
    ColumnMapping x{"A", ColumnMapping::Role::XTime,  "",  "s"};
    ColumnMapping y{"B", ColumnMapping::Role::Signal, "V", "V"};
    y.xSourceColumn = "A";
    y.timeOffsetSec = 10.0;
    p.columns = { x, y };

    QString err;
    auto sigs = src.apply(p, &err);
    ASSERT_EQ(sigs.size(), 1u) << err.toStdString();
    auto view = sigs[0]->snapshotForRead();
    ASSERT_EQ(view.count, 2u);
    EXPECT_NEAR(view.timestamps[0] / 1e9, 10.0, 1e-9);
    EXPECT_NEAR(view.timestamps[1] / 1e9, 10.1, 1e-9);
    std::filesystem::remove(path);
}

// Reset + offset combine: reset first, then offset, so reset + offset = K
// puts the first sample at exactly t = K seconds.
TEST(CsvConverter, ResetThenOffsetFirstSampleAtOffset) {
    auto path = std::filesystem::temp_directory_path() / "scope_test_csv_both.csv";
    {
        std::ofstream f(path);
        f << "t,v\n";
        f << "5.0,1\n";
        f << "5.5,2\n";
    }
    CsvSource src(path);

    ConverterProfile p;
    p.headerRow = 1;
    p.decimalSeparator = ".";
    ColumnMapping x{"A", ColumnMapping::Role::XTime,  "",  "s"};
    ColumnMapping y{"B", ColumnMapping::Role::Signal, "V", "V"};
    y.xSourceColumn   = "A";
    y.resetTimeToZero = true;
    y.timeOffsetSec   = 3.0;
    p.columns = { x, y };

    QString err;
    auto sigs = src.apply(p, &err);
    ASSERT_EQ(sigs.size(), 1u) << err.toStdString();
    auto view = sigs[0]->snapshotForRead();
    ASSERT_EQ(view.count, 2u);
    // 5.0 → reset to 0 → +3 = 3
    EXPECT_NEAR(view.timestamps[0] / 1e9, 3.0, 1e-9);
    // 5.5 → reset to 0.5 → +3 = 3.5
    EXPECT_NEAR(view.timestamps[1] / 1e9, 3.5, 1e-9);
    std::filesystem::remove(path);
}

// Duplicate-X collapse: a CSV that repeats the same X for several rows
// should produce one Signal sample per unique X when collapse is set.
namespace {
auto writeDupCsv = []{
    auto path = std::filesystem::temp_directory_path() / "scope_test_csv_dup.csv";
    std::ofstream f(path);
    f << "t,v\n";
    f << "0.25,1\n";
    f << "0.25,1\n";
    f << "0.25,1\n";
    f << "0.25,1\n";
    f << "0.5,2\n";
    f << "0.5,3\n";   // different Y at duplicate t: matters for First vs Last vs Mean
    f << "0.5,4\n";
    f << "0.5,5\n";
    return path;
};

ConverterProfile makeDupProfile(ColumnMapping::CollapseMode mode) {
    ConverterProfile p;
    p.headerRow = 1;
    p.decimalSeparator = ".";
    ColumnMapping x{"A", ColumnMapping::Role::XTime,  "",  "s"};
    ColumnMapping y{"B", ColumnMapping::Role::Signal, "V", "V"};
    y.xSourceColumn       = "A";
    y.collapseDuplicates  = mode;
    p.columns = { x, y };
    return p;
}
}  // namespace

TEST(CsvConverter, CollapseDuplicates_None_KeepsAll) {
    auto path = writeDupCsv();
    CsvSource src(path);
    QString err;
    auto sigs = src.apply(makeDupProfile(ColumnMapping::CollapseMode::None), &err);
    ASSERT_EQ(sigs.size(), 1u) << err.toStdString();
    EXPECT_EQ(sigs[0]->sampleCount(), 8u);  // all 8 rows preserved
    std::filesystem::remove(path);
}

TEST(CsvConverter, CollapseDuplicates_First) {
    auto path = writeDupCsv();
    CsvSource src(path);
    QString err;
    auto sigs = src.apply(makeDupProfile(ColumnMapping::CollapseMode::First), &err);
    ASSERT_EQ(sigs.size(), 1u) << err.toStdString();
    auto view = sigs[0]->snapshotForRead();
    auto vs   = sigs[0]->readAsDouble();
    ASSERT_EQ(view.count, 2u);
    EXPECT_NEAR(view.timestamps[0] / 1e9, 0.25, 1e-9);
    EXPECT_NEAR(view.timestamps[1] / 1e9, 0.50, 1e-9);
    EXPECT_DOUBLE_EQ(vs[0], 1.0);
    EXPECT_DOUBLE_EQ(vs[1], 2.0);   // first of {2,3,4,5}
    std::filesystem::remove(path);
}

TEST(CsvConverter, CollapseDuplicates_Last) {
    auto path = writeDupCsv();
    CsvSource src(path);
    QString err;
    auto sigs = src.apply(makeDupProfile(ColumnMapping::CollapseMode::Last), &err);
    ASSERT_EQ(sigs.size(), 1u) << err.toStdString();
    auto vs = sigs[0]->readAsDouble();
    ASSERT_EQ(sigs[0]->sampleCount(), 2u);
    EXPECT_DOUBLE_EQ(vs[0], 1.0);
    EXPECT_DOUBLE_EQ(vs[1], 5.0);   // last of {2,3,4,5}
    std::filesystem::remove(path);
}

TEST(CsvConverter, CollapseDuplicates_Mean) {
    auto path = writeDupCsv();
    CsvSource src(path);
    QString err;
    auto sigs = src.apply(makeDupProfile(ColumnMapping::CollapseMode::Mean), &err);
    ASSERT_EQ(sigs.size(), 1u) << err.toStdString();
    auto vs = sigs[0]->readAsDouble();
    ASSERT_EQ(sigs[0]->sampleCount(), 2u);
    EXPECT_DOUBLE_EQ(vs[0], 1.0);             // mean of {1,1,1,1}
    EXPECT_DOUBLE_EQ(vs[1], (2 + 3 + 4 + 5) / 4.0);  // mean of {2,3,4,5}
    std::filesystem::remove(path);
}

// Value-plateau collapse: a CSV logged faster than the underlying
// value updates (integer encoder, distinct timestamps but value
// repeats across rows). With KeepFirst / KeepLast we get one sample
// per value run.
namespace {
auto writePlateauCsv = []{
    auto path = std::filesystem::temp_directory_path() / "scope_test_csv_plat.csv";
    std::ofstream f(path);
    f << "t,v\n";
    f << "0.00,10\n";
    f << "0.01,10\n";
    f << "0.02,10\n";
    f << "0.03,11\n";
    f << "0.04,11\n";
    f << "0.05,11\n";
    f << "0.06,12\n";
    f << "0.07,12\n";
    f << "0.08,12\n";
    return path;
};

ConverterProfile makePlateauProfile(ColumnMapping::ValuePlateauMode mode) {
    ConverterProfile p;
    p.headerRow = 1;
    p.decimalSeparator = ".";
    ColumnMapping x{"A", ColumnMapping::Role::XTime,  "",  "s"};
    ColumnMapping y{"B", ColumnMapping::Role::Signal, "V", "V"};
    y.xSourceColumn          = "A";
    y.collapseValuePlateaus  = mode;
    p.columns = { x, y };
    return p;
}
}  // namespace

TEST(CsvConverter, ValuePlateaus_None_KeepsAll) {
    auto path = writePlateauCsv();
    CsvSource src(path);
    QString err;
    auto sigs = src.apply(makePlateauProfile(ColumnMapping::ValuePlateauMode::None), &err);
    ASSERT_EQ(sigs.size(), 1u) << err.toStdString();
    EXPECT_EQ(sigs[0]->sampleCount(), 9u);  // 9 rows preserved
    std::filesystem::remove(path);
}

TEST(CsvConverter, ValuePlateaus_KeepFirst) {
    auto path = writePlateauCsv();
    CsvSource src(path);
    QString err;
    auto sigs = src.apply(makePlateauProfile(ColumnMapping::ValuePlateauMode::KeepFirst), &err);
    ASSERT_EQ(sigs.size(), 1u) << err.toStdString();
    auto view = sigs[0]->snapshotForRead();
    auto vs   = sigs[0]->readAsDouble();
    ASSERT_EQ(view.count, 3u);
    EXPECT_NEAR(view.timestamps[0] / 1e9, 0.00, 1e-9);
    EXPECT_NEAR(view.timestamps[1] / 1e9, 0.03, 1e-9);
    EXPECT_NEAR(view.timestamps[2] / 1e9, 0.06, 1e-9);
    EXPECT_DOUBLE_EQ(vs[0], 10.0);
    EXPECT_DOUBLE_EQ(vs[1], 11.0);
    EXPECT_DOUBLE_EQ(vs[2], 12.0);
    std::filesystem::remove(path);
}

TEST(CsvConverter, ValuePlateaus_KeepLast) {
    auto path = writePlateauCsv();
    CsvSource src(path);
    QString err;
    auto sigs = src.apply(makePlateauProfile(ColumnMapping::ValuePlateauMode::KeepLast), &err);
    ASSERT_EQ(sigs.size(), 1u) << err.toStdString();
    auto view = sigs[0]->snapshotForRead();
    auto vs   = sigs[0]->readAsDouble();
    ASSERT_EQ(view.count, 3u);
    EXPECT_NEAR(view.timestamps[0] / 1e9, 0.02, 1e-9);
    EXPECT_NEAR(view.timestamps[1] / 1e9, 0.05, 1e-9);
    EXPECT_NEAR(view.timestamps[2] / 1e9, 0.08, 1e-9);
    EXPECT_DOUBLE_EQ(vs[0], 10.0);
    EXPECT_DOUBLE_EQ(vs[1], 11.0);
    EXPECT_DOUBLE_EQ(vs[2], 12.0);
    std::filesystem::remove(path);
}

TEST(CsvConverter, ValuePlateaus_AllSameCollapsesToOne) {
    auto path = std::filesystem::temp_directory_path() / "scope_test_csv_flat.csv";
    {
        std::ofstream f(path);
        f << "t,v\n";
        for (int i = 0; i < 5; ++i) f << (i * 0.01) << ",7\n";
    }
    CsvSource src(path);
    QString err;
    auto sigs = src.apply(makePlateauProfile(ColumnMapping::ValuePlateauMode::KeepFirst), &err);
    ASSERT_EQ(sigs.size(), 1u) << err.toStdString();
    EXPECT_EQ(sigs[0]->sampleCount(), 1u);
    std::filesystem::remove(path);
}
