#include "scope/converter/CsvWriter.h"
#include "scope/converter/CsvSource.h"
#include "scope/converter/ConverterProfile.h"
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
    std::filesystem::remove(path);
}
