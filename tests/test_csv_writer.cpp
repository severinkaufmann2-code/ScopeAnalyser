#include "scope/converter/CsvWriter.h"
#include "scope/converter/CsvSource.h"
#include "scope/converter/ConverterProfile.h"
#include "scope/converter/SignalIO.h"
#include "scope/core/Signal.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

using namespace scope::converter;
using namespace scope::core;

namespace {
std::shared_ptr<Signal> makeRamp(QString name, std::size_t n, double dtSec,
                                 double startVal, double slope,
                                 QString unit = QString()) {
    Signal::Meta m;
    m.name = std::move(name);
    m.unit = std::move(unit);
    m.dataType = DataType::Float64;
    m.sampleRateHz = 1.0 / dtSec;
    auto sig = std::make_shared<Signal>(m);
    std::vector<TimestampNs> ts(n);
    std::vector<double> vs(n);
    for (std::size_t i = 0; i < n; ++i) {
        ts[i] = static_cast<TimestampNs>(i * dtSec * 1e9);
        vs[i] = startVal + i * slope;
    }
    sig->append(ts.data(), reinterpret_cast<const std::byte*>(vs.data()), n);
    return sig;
}
}

TEST(CsvWriter, SharedTimeColumnDefaults) {
    auto path = std::filesystem::temp_directory_path() / "scope_csv_writer_shared.csv";

    std::vector<std::shared_ptr<Signal>> chans;
    chans.push_back(makeRamp("speed",  4, 0.1, 0.0, 10.0, "rpm"));
    chans.push_back(makeRamp("torque", 4, 0.1, 1.0,  0.5, "Nm"));

    CsvExportOptions opts;  // all defaults
    QString err;
    ASSERT_TRUE(writeCsv(path, chans, opts, &err)) << err.toStdString();

    // Re-import via CsvSource → confirm structure and values.
    CsvSource src(path);
    EXPECT_GE(src.rowCount(), 5);   // header + 4 data rows
    EXPECT_EQ(src.columnCount(), 3); // t [s], speed, torque

    ConverterProfile p;
    p.headerRow = 1;
    p.columns = {
        {"A", ColumnMapping::Role::XTime,  "", "s"},
        {"B", ColumnMapping::Role::Signal, "speed",  "rpm"},
        {"C", ColumnMapping::Role::Signal, "torque", "Nm"},
    };
    auto loaded = src.apply(p, &err);
    ASSERT_EQ(loaded.size(), 2u) << err.toStdString();

    auto findByName = [&](const QString& name) -> std::shared_ptr<Signal> {
        for (const auto& s : loaded) if (s->meta().name == name) return s;
        return nullptr;
    };
    auto speedOut  = findByName("speed");
    auto torqueOut = findByName("torque");
    ASSERT_TRUE(speedOut);
    ASSERT_TRUE(torqueOut);
    EXPECT_EQ(speedOut->sampleCount(),  4u);
    EXPECT_EQ(torqueOut->sampleCount(), 4u);

    auto sv = speedOut->readAsDouble();
    auto tv = torqueOut->readAsDouble();
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_NEAR(sv[i], 0.0 + i * 10.0, 1e-6);
        EXPECT_NEAR(tv[i], 1.0 + i * 0.5,  1e-6);
    }

    std::filesystem::remove(path);
}

TEST(CsvWriter, SharedTimeColumnResamplesAcrossRates) {
    auto path = std::filesystem::temp_directory_path() / "scope_csv_writer_mixed_rates.csv";

    std::vector<std::shared_ptr<Signal>> chans;
    // Both ramps in [0, 1) s but at different rates.
    chans.push_back(makeRamp("fast", 10, 0.1,  0.0, 1.0));   // dt=0.1s, 10 samples
    chans.push_back(makeRamp("slow",  5, 0.2, 10.0, 1.0));   // dt=0.2s, 5 samples

    CsvExportOptions opts;
    QString err;
    ASSERT_TRUE(writeCsv(path, chans, opts, &err)) << err.toStdString();

    // Union timestamps should be {0, 0.1, 0.2, 0.3, ..., 0.9} → 10 entries
    // (fast's grid is a superset).
    CsvSource src(path);
    EXPECT_EQ(src.rowCount(), 11);   // header + 10 data rows

    std::filesystem::remove(path);
}

TEST(CsvWriter, PerSignalTimeMode) {
    auto path = std::filesystem::temp_directory_path() / "scope_csv_writer_per_signal.csv";

    std::vector<std::shared_ptr<Signal>> chans;
    chans.push_back(makeRamp("a", 3, 0.5, 0.0, 1.0));
    chans.push_back(makeRamp("b", 5, 0.2, 100.0, 1.0));

    CsvExportOptions opts;
    opts.timeMode = CsvExportOptions::TimeMode::PerSignal;
    QString err;
    ASSERT_TRUE(writeCsv(path, chans, opts, &err)) << err.toStdString();

    CsvSource src(path);
    EXPECT_EQ(src.columnCount(), 4);  // t_a, a, t_b, b
    EXPECT_EQ(src.rowCount(), 6);     // header + max(3,5)=5 data rows

    std::filesystem::remove(path);
}

TEST(CsvWriter, CustomSeparatorsRoundTrip) {
    auto path = std::filesystem::temp_directory_path() / "scope_csv_writer_eu.csv";

    std::vector<std::shared_ptr<Signal>> chans;
    chans.push_back(makeRamp("v", 3, 1.0, 0.5, 1.0));

    CsvExportOptions opts;
    opts.columnDelimiter = ";";
    opts.decimalSeparator = ",";
    QString err;
    ASSERT_TRUE(writeCsv(path, chans, opts, &err)) << err.toStdString();

    CsvSource src(path, ";");
    ConverterProfile p;
    p.headerRow = 1;
    p.decimalSeparator = ",";
    p.columns = {
        {"A", ColumnMapping::Role::XTime,  "", "s"},
        {"B", ColumnMapping::Role::Signal, "v", ""},
    };
    auto loaded = src.apply(p, &err);
    ASSERT_EQ(loaded.size(), 1u) << err.toStdString();
    auto vs = loaded[0]->readAsDouble();
    EXPECT_NEAR(vs[1], 1.5, 1e-6);

    std::filesystem::remove(path);
}

// Per-signal mode: a frequency-domain signal emits "f_<name> [Hz]" and
// stays absolute (no time-origin subtraction).
TEST(CsvWriter, PerSignalUsesFHzForFrequencyDomain) {
    Signal::Meta m;
    m.name = "spec";
    m.unit = "magnitude";
    m.dataType = DataType::Float64;
    m.domain = Signal::Domain::Frequency;
    auto sig = std::make_shared<Signal>(m);
    // Three frequency bins encoded as "timestamps" = Hz × 1e9.
    std::vector<TimestampNs> ts{0, 10LL * 1'000'000'000LL, 20LL * 1'000'000'000LL};
    std::vector<double> vs{100.0, 50.0, 10.0};
    sig->append(ts.data(), reinterpret_cast<const std::byte*>(vs.data()), ts.size());

    CsvExportOptions opts;
    opts.timeMode = CsvExportOptions::TimeMode::PerSignal;
    // Disable metadata so the column-header line is at line 0 — easier
    // to inspect without skipping the # comment.
    opts.includeMetadata = false;
    auto path = std::filesystem::temp_directory_path() / "scope_test_csv_fhz_persig.csv";
    QString err;
    ASSERT_TRUE(writeCsv(path, {sig}, opts, &err)) << err.toStdString();

    std::ifstream in(path);
    std::string header; std::getline(in, header);
    EXPECT_NE(header.find("f_spec [Hz]"),     std::string::npos) << header;
    EXPECT_NE(header.find("spec [magnitude]"), std::string::npos);
    // First data row's X should be 0 (not subtracted-from-anything).
    std::string firstRow; std::getline(in, firstRow);
    EXPECT_EQ(firstRow.substr(0, 1), "0") << firstRow;
    in.close();  // close before removing (Windows locks open files)
    std::filesystem::remove(path);
}

// Shared mode + mixed domain: two shared X columns side-by-side, each
// labelled correctly. Padded with empty cells for the shorter block.
TEST(CsvWriter, SharedMixedDomainGetsTwoXColumns) {
    Signal::Meta tm; tm.name = "speed"; tm.unit = "m/s";
    tm.dataType = DataType::Float64; tm.domain = Signal::Domain::Time;
    auto tsig = std::make_shared<Signal>(tm);
    std::vector<TimestampNs> tts{0, 1'000'000'000LL, 2'000'000'000LL,
                                 3'000'000'000LL, 4'000'000'000LL};
    std::vector<double> tvs{10, 11, 12, 13, 14};
    tsig->append(tts.data(),
                 reinterpret_cast<const std::byte*>(tvs.data()), tts.size());

    Signal::Meta fm; fm.name = "spec"; fm.unit = "magnitude";
    fm.dataType = DataType::Float64; fm.domain = Signal::Domain::Frequency;
    auto fsig = std::make_shared<Signal>(fm);
    std::vector<TimestampNs> fts{0, 5LL * 1'000'000'000LL, 10LL * 1'000'000'000LL};
    std::vector<double> fvs{100, 50, 10};
    fsig->append(fts.data(),
                 reinterpret_cast<const std::byte*>(fvs.data()), fts.size());

    CsvExportOptions opts;
    opts.timeMode = CsvExportOptions::TimeMode::Shared;
    // Keep this test focused on the data-column layout — disable the
    // separate metadata-header line.
    opts.includeMetadata = false;
    auto path = std::filesystem::temp_directory_path() / "scope_test_csv_mixed.csv";
    QString err;
    ASSERT_TRUE(writeCsv(path, {tsig, fsig}, opts, &err)) << err.toStdString();

    std::ifstream in(path);
    std::string header; std::getline(in, header);
    EXPECT_NE(header.find("t [s]"),  std::string::npos) << header;
    EXPECT_NE(header.find("f [Hz]"), std::string::npos);
    EXPECT_NE(header.find("speed"), std::string::npos);
    EXPECT_NE(header.find("spec"),  std::string::npos);

    // Should have max(5, 3) = 5 data rows.
    int dataRows = 0;
    std::string r;
    while (std::getline(in, r)) { if (!r.empty()) ++dataRows; }
    EXPECT_EQ(dataRows, 5);
    in.close();  // close before removing (Windows locks open files)
    std::filesystem::remove(path);
}

// Verifies the # scope-csv metadata header is emitted by default and
// describes each column. Independent of the data-row format.
TEST(CsvWriter, MetadataHeaderEmittedByDefault) {
    Signal::Meta tm; tm.name = "speed"; tm.unit = "rpm";
    tm.dataType = DataType::Float64; tm.domain = Signal::Domain::Time;
    tm.sourceSymbol = "MAIN.fSpeed";
    auto t = std::make_shared<Signal>(tm);
    std::vector<TimestampNs> tts{0, 1'000'000'000LL};
    std::vector<double>      tvs{0, 10};
    t->append(tts.data(), reinterpret_cast<const std::byte*>(tvs.data()), 2);

    Signal::Meta fm; fm.name = "FFT_speed"; fm.unit = "mag";
    fm.dataType = DataType::Float64; fm.domain = Signal::Domain::Frequency;
    auto f = std::make_shared<Signal>(fm);
    std::vector<TimestampNs> fts{0, 50LL * 1'000'000'000LL};
    std::vector<double>      fvs{1, 2};
    f->append(fts.data(), reinterpret_cast<const std::byte*>(fvs.data()), 2);

    auto path = std::filesystem::temp_directory_path() / "scope_csv_meta_default.csv";
    QString err;
    ASSERT_TRUE(writeCsv(path, {t, f}, CsvExportOptions{}, &err))
        << err.toStdString();

    std::ifstream in(path);
    std::string firstLine; std::getline(in, firstLine);
    EXPECT_EQ(firstLine.substr(0, 13), "# scope-csv: ") << firstLine;
    EXPECT_NE(firstLine.find("\"x_time\""), std::string::npos);
    EXPECT_NE(firstLine.find("\"x_frequency\""), std::string::npos);
    EXPECT_NE(firstLine.find("\"FFT_speed\""), std::string::npos);
    EXPECT_NE(firstLine.find("\"MAIN.fSpeed\""), std::string::npos);
    in.close();  // close before removing (Windows locks open files)
    std::filesystem::remove(path);
}

// Reproduces the user-reported bug: a mixed time + frequency shared-X CSV
// previously loaded with EVERY signal tagged time-domain, dropping the
// FFT into the wrong view. With the metadata header on, each signal
// round-trips its domain.
TEST(CsvWriter, MixedDomainRoundTripsDomainsViaMetadata) {
    Signal::Meta tm; tm.name = "speed"; tm.unit = "rpm";
    tm.dataType = DataType::Float64; tm.domain = Signal::Domain::Time;
    auto tsig = std::make_shared<Signal>(tm);
    std::vector<TimestampNs> tts{0, 1'000'000'000LL, 2'000'000'000LL};
    std::vector<double>      tvs{0, 10, 20};
    tsig->append(tts.data(), reinterpret_cast<const std::byte*>(tvs.data()), 3);

    Signal::Meta fm; fm.name = "FFT_speed"; fm.unit = "mag";
    fm.dataType = DataType::Float64; fm.domain = Signal::Domain::Frequency;
    auto fsig = std::make_shared<Signal>(fm);
    std::vector<TimestampNs> fts{0, 50LL * 1'000'000'000LL, 100LL * 1'000'000'000LL};
    std::vector<double>      fvs{1, 2, 3};
    fsig->append(fts.data(), reinterpret_cast<const std::byte*>(fvs.data()), 3);

    auto path = std::filesystem::temp_directory_path() / "scope_csv_mixed_round_trip.csv";
    CsvExportOptions opts; opts.timeMode = CsvExportOptions::TimeMode::Shared;
    QString err;
    ASSERT_TRUE(writeCsv(path, {tsig, fsig}, opts, &err)) << err.toStdString();

    auto r = loadFile(path, FileFormat::Csv);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_EQ(r.channels.size(), 2u);

    std::shared_ptr<Signal> speed, fft;
    for (auto& s : r.channels) {
        if (s->meta().name == "speed")     speed = s;
        if (s->meta().name == "FFT_speed") fft   = s;
    }
    ASSERT_TRUE(speed);
    ASSERT_TRUE(fft);
    EXPECT_EQ(speed->meta().domain, Signal::Domain::Time);
    EXPECT_EQ(fft  ->meta().domain, Signal::Domain::Frequency);
    EXPECT_EQ(speed->meta().unit,   "rpm");
    EXPECT_EQ(fft  ->meta().unit,   "mag");
    std::filesystem::remove(path);
}

// Legacy file written before the metadata feature (or with the
// checkbox unticked): the heuristic fallback in loadCsvChart must still
// correctly route mixed-X data — the prior bug was assigning every Y
// to column 0.
TEST(CsvWriter, MixedDomainHeuristicFallbackTagsCorrectly) {
    Signal::Meta tm; tm.name = "speed"; tm.unit = "rpm";
    tm.dataType = DataType::Float64; tm.domain = Signal::Domain::Time;
    auto tsig = std::make_shared<Signal>(tm);
    std::vector<TimestampNs> tts{0, 1'000'000'000LL};
    std::vector<double>      tvs{0, 10};
    tsig->append(tts.data(), reinterpret_cast<const std::byte*>(tvs.data()), 2);

    Signal::Meta fm; fm.name = "FFT_speed"; fm.unit = "mag";
    fm.dataType = DataType::Float64; fm.domain = Signal::Domain::Frequency;
    auto fsig = std::make_shared<Signal>(fm);
    std::vector<TimestampNs> fts{0, 50LL * 1'000'000'000LL};
    std::vector<double>      fvs{1, 2};
    fsig->append(fts.data(), reinterpret_cast<const std::byte*>(fvs.data()), 2);

    auto path = std::filesystem::temp_directory_path() / "scope_csv_mixed_legacy.csv";
    CsvExportOptions opts;
    opts.timeMode = CsvExportOptions::TimeMode::Shared;
    opts.includeMetadata = false;          // simulate legacy file
    QString err;
    ASSERT_TRUE(writeCsv(path, {tsig, fsig}, opts, &err)) << err.toStdString();

    auto r = loadFile(path, FileFormat::Csv);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_EQ(r.channels.size(), 2u);
    std::shared_ptr<Signal> speed, fft;
    for (auto& s : r.channels) {
        if (s->meta().name == "speed")     speed = s;
        if (s->meta().name == "FFT_speed") fft   = s;
    }
    ASSERT_TRUE(speed);
    ASSERT_TRUE(fft);
    EXPECT_EQ(speed->meta().domain, Signal::Domain::Time);
    EXPECT_EQ(fft  ->meta().domain, Signal::Domain::Frequency);
    std::filesystem::remove(path);
}

// saveChartFromStore is the shared helper both the Analyser and the
// Converter call from their Save chart… flows. This test exercises the
// per-domain split path through a real SignalStore.
TEST(CsvWriter, SaveChartFromStoreSplitsByDomain) {
    scope::core::SignalStore store;
    {
        Signal::Meta m; m.name = "speed"; m.unit = "rpm";
        m.dataType = DataType::Float64; m.domain = Signal::Domain::Time;
        auto s = std::make_shared<Signal>(m);
        std::vector<TimestampNs> ts{0, 1'000'000'000LL};
        std::vector<double>      vs{0, 10};
        s->append(ts.data(), reinterpret_cast<const std::byte*>(vs.data()), 2);
        store.add(s);
    }
    {
        Signal::Meta m; m.name = "FFT_speed"; m.unit = "mag";
        m.dataType = DataType::Float64; m.domain = Signal::Domain::Frequency;
        auto s = std::make_shared<Signal>(m);
        std::vector<TimestampNs> ts{0, 50LL * 1'000'000'000LL};
        std::vector<double>      vs{1, 2};
        s->append(ts.data(), reinterpret_cast<const std::byte*>(vs.data()), 2);
        store.add(s);
    }

    const auto base = std::filesystem::temp_directory_path() / "scope_chart_split.csv";
    const QString basePath = QString::fromStdString(base.string());
    ChartSaveFilters filters;
    filters.splitDomainsIntoTwoFiles = true;
    SaveOptions opts;
    QStringList messages; QString err;
    auto result = saveChartFromStore(basePath, store, FileFormat::Csv,
                                     filters, opts, &messages, &err);
    EXPECT_EQ(result, ChartSaveResult::Ok) << err.toStdString();
    EXPECT_EQ(messages.size(), 2);
    const auto tPath = std::filesystem::temp_directory_path() / "scope_chart_split_time.csv";
    const auto fPath = std::filesystem::temp_directory_path() / "scope_chart_split_frequency.csv";
    EXPECT_TRUE(std::filesystem::exists(tPath));
    EXPECT_TRUE(std::filesystem::exists(fPath));
    std::filesystem::remove(tPath);
    std::filesystem::remove(fPath);
}

TEST(CsvWriter, SaveChartFromStoreReportsNothingMatched) {
    scope::core::SignalStore store;
    {
        Signal::Meta m; m.name = "speed"; m.unit = "rpm";
        m.dataType = DataType::Float64; m.domain = Signal::Domain::Time;
        auto s = std::make_shared<Signal>(m);
        std::vector<TimestampNs> ts{0, 1'000'000'000LL};
        std::vector<double>      vs{0, 10};
        s->append(ts.data(), reinterpret_cast<const std::byte*>(vs.data()), 2);
        store.add(s);
    }
    const auto base = std::filesystem::temp_directory_path() / "scope_chart_empty.csv";
    ChartSaveFilters filters;
    filters.includeTime      = false;  // strip the only signal we have
    filters.includeFrequency = false;
    QStringList messages; QString err;
    auto result = saveChartFromStore(
        QString::fromStdString(base.string()),
        store, FileFormat::Csv, filters, SaveOptions{},
        &messages, &err);
    EXPECT_EQ(result, ChartSaveResult::NothingMatchedFilters);
    EXPECT_TRUE(messages.isEmpty());
    EXPECT_FALSE(std::filesystem::exists(base));
}

TEST(CsvWriter, MetadataHeaderCanBeDisabled) {
    auto sig = makeRamp("speed", 3, 0.1, 0.0, 1.0, "rpm");
    CsvExportOptions opts;
    opts.includeMetadata = false;
    auto path = std::filesystem::temp_directory_path() / "scope_csv_meta_off.csv";
    QString err;
    ASSERT_TRUE(writeCsv(path, {sig}, opts, &err)) << err.toStdString();
    std::ifstream in(path);
    std::string firstLine; std::getline(in, firstLine);
    // No metadata line — first line is the column header.
    EXPECT_NE(firstLine.find("t [s]"), std::string::npos) << firstLine;
    in.close();  // close before removing (Windows locks open files)
    std::filesystem::remove(path);
}

// ---- Custom export time range -------------------------------------------

// For signals whose timestamps start at 0 the custom range behaves as the
// dialog suggests: [0.25 s, 0.75 s] keeps the middle half.
TEST(CsvWriter, SaveChartCustomRangeOnZeroBasedSignals) {
    scope::core::SignalStore store;
    store.add(makeRamp("speed", 11, 0.1, 0.0, 1.0, "rpm"));   // t = 0.0 … 1.0 s

    const auto base = std::filesystem::temp_directory_path()
                    / "scope_chart_range0.csv";
    ChartSaveFilters filters;
    filters.useCustomRange = true;
    filters.fromSec = 0.25;
    filters.toSec   = 0.75;
    QStringList messages; QString err;
    auto result = saveChartFromStore(
        QString::fromStdString(base.string()), store, FileFormat::Csv,
        filters, SaveOptions{}, &messages, &err);
    ASSERT_EQ(result, ChartSaveResult::Ok) << err.toStdString();

    auto loaded = loadFile(base);
    ASSERT_TRUE(loaded.ok) << loaded.error.toStdString();
    ASSERT_EQ(loaded.channels.size(), 1u);
    // Samples at 0.3 / 0.4 / 0.5 / 0.6 / 0.7 s survive the [0.25, 0.75] cut.
    EXPECT_EQ(loaded.channels[0]->sampleCount(), 5u);
    std::filesystem::remove(base);
}

// Finding B1 regression guard: the Save-chart dialog is pre-filled with the
// chart's X range, which is in seconds RELATIVE to the recording start.
// saveChartFromStore resolves the range against the earliest first sample
// of the exported time channels (the same origin the CSV writer and the
// chart use), so recorder data with absolute epoch timestamps exports the
// samples the user actually selected.
TEST(CsvWriter, SaveChartCustomRangeMatchesChartRelativeSeconds) {
    scope::core::SignalStore store;
    {
        Signal::Meta m; m.name = "speed"; m.unit = "rpm";
        m.dataType = DataType::Float64; m.domain = Signal::Domain::Time;
        auto s = std::make_shared<Signal>(m);
        const TimestampNs t0 = 1'750'000'000'000'000'000LL;   // epoch start
        std::vector<TimestampNs> ts(11);
        std::vector<double>      vs(11);
        for (int i = 0; i < 11; ++i) {
            ts[i] = t0 + static_cast<TimestampNs>(i) * 100'000'000LL;  // 0.1 s
            vs[i] = i;
        }
        s->append(ts.data(), reinterpret_cast<const std::byte*>(vs.data()), 11);
        store.add(s);
    }

    const auto base = std::filesystem::temp_directory_path()
                    / "scope_chart_range_epoch.csv";
    ChartSaveFilters filters;
    filters.useCustomRange = true;
    filters.fromSec = 0.25;        // what the user reads off the chart
    filters.toSec   = 0.75;
    QStringList messages; QString err;
    auto result = saveChartFromStore(
        QString::fromStdString(base.string()), store, FileFormat::Csv,
        filters, SaveOptions{}, &messages, &err);
    ASSERT_EQ(result, ChartSaveResult::Ok)
        << "custom range must select chart-relative seconds, got: "
        << err.toStdString();
    auto loaded = loadFile(base);
    ASSERT_TRUE(loaded.ok) << loaded.error.toStdString();
    ASSERT_EQ(loaded.channels.size(), 1u);
    EXPECT_EQ(loaded.channels[0]->sampleCount(), 5u);
    std::filesystem::remove(base);
}
