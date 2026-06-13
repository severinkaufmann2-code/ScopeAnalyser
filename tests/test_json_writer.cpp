#include "scope/converter/SignalIO.h"
#include "scope/core/Signal.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

using namespace scope::converter;
using namespace scope::core;

namespace {
std::shared_ptr<Signal> makeSig(QString name, QString unit, Signal::Domain dom,
                                std::vector<TimestampNs> ts,
                                std::vector<double> vs) {
    Signal::Meta m;
    m.name = std::move(name);
    m.unit = std::move(unit);
    m.dataType = DataType::Float64;
    m.domain = dom;
    auto s = std::make_shared<Signal>(m);
    s->append(ts.data(), reinterpret_cast<const std::byte*>(vs.data()), ts.size());
    return s;
}
std::shared_ptr<Signal> byName(const std::vector<std::shared_ptr<Signal>>& v,
                               const QString& n) {
    for (const auto& s : v) if (s->meta().name == n) return s;
    return nullptr;
}
}  // namespace

// The whole point of the raw-int64 encoding: epoch-scale ns that a double
// can't hold exactly must survive a save → re-open untouched.
TEST(JsonWriter, NativeRoundTripIsBitExact) {
    auto path = std::filesystem::temp_directory_path() / "scope_json_rt.json";

    std::vector<std::shared_ptr<Signal>> chans;
    chans.push_back(makeSig("Speed", "rpm", Signal::Domain::Time,
        {1700000000000000001LL, 1700000000001000002LL, 1700000000002000003LL},
        {12.3, 12.4, 12.5}));
    chans.push_back(makeSig("Spectrum", "dB", Signal::Domain::Frequency,
        {0, 1000000000LL, 2000000000LL}, {-3.0, -6.0, -9.0}));

    SaveOptions opts;
    QString err;
    ASSERT_TRUE(saveFile(path, chans, FileFormat::Json, opts, &err))
        << err.toStdString();

    auto r = loadFile(path, FileFormat::Json);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_EQ(r.channels.size(), 2u);

    auto speed = byName(r.channels, "Speed");
    ASSERT_TRUE(speed);
    EXPECT_EQ(speed->meta().unit, "rpm");
    EXPECT_EQ(speed->meta().domain, Signal::Domain::Time);
    auto sv = speed->snapshotForRead();
    ASSERT_EQ(sv.count, 3u);
    EXPECT_EQ(sv.timestamps[0], 1700000000000000001LL);   // bit-exact int64
    EXPECT_EQ(sv.timestamps[1], 1700000000001000002LL);
    EXPECT_EQ(sv.timestamps[2], 1700000000002000003LL);
    auto svals = speed->readAsDouble();
    ASSERT_EQ(svals.size(), 3u);
    EXPECT_DOUBLE_EQ(svals[0], 12.3);
    EXPECT_DOUBLE_EQ(svals[2], 12.5);

    auto spec = byName(r.channels, "Spectrum");
    ASSERT_TRUE(spec);
    EXPECT_EQ(spec->meta().domain, Signal::Domain::Frequency);
    auto fv = spec->snapshotForRead();
    ASSERT_EQ(fv.count, 3u);
    EXPECT_EQ(fv.timestamps[1], 1000000000LL);            // Hz×1e9 verbatim
}

// Auto-detect from extension must route .json through the JSON loader.
TEST(JsonWriter, AutoFormatFromExtension) {
    auto path = std::filesystem::temp_directory_path() / "scope_json_auto.json";
    std::vector<std::shared_ptr<Signal>> chans;
    chans.push_back(makeSig("a", "", Signal::Domain::Time,
        {0, 1000000000LL}, {1.0, 2.0}));
    QString err;
    ASSERT_TRUE(saveFile(path, chans, FileFormat::Auto, {}, &err))
        << err.toStdString();
    auto r = loadFile(path, FileFormat::Auto);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_EQ(r.channels.size(), 1u);
    EXPECT_EQ(r.channels[0]->meta().name, "a");
}

// A saved layout blob must come back via LoadResult::layoutJson.
TEST(JsonWriter, EmbedsLayoutJson) {
    auto path = std::filesystem::temp_directory_path() / "scope_json_layout.json";
    std::vector<std::shared_ptr<Signal>> chans;
    chans.push_back(makeSig("a", "", Signal::Domain::Time,
        {0, 1000000000LL}, {1.0, 2.0}));

    SaveOptions opts;
    opts.layoutJson = R"({"axes":[{"id":"y1"}],"view":"time"})";
    QString err;
    ASSERT_TRUE(saveFile(path, chans, FileFormat::Json, opts, &err))
        << err.toStdString();

    auto r = loadFile(path, FileFormat::Json);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_FALSE(r.layoutJson.isEmpty());
    EXPECT_NE(r.layoutJson.indexOf("y1"), -1);
}

// Foreign columnar JSON opens (best-effort) and is flagged as guessed.
TEST(JsonWriter, ForeignColumnarBestEffort) {
    auto path = std::filesystem::temp_directory_path() / "scope_json_foreign.json";
    {
        std::ofstream f(path);
        f << R"({"t":[0.0,0.1,0.2],"speed":[0.0,10.0,20.0]})";
    }
    auto r = loadFile(path, FileFormat::Json);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_EQ(r.channels.size(), 1u);
    EXPECT_EQ(r.channels[0]->meta().name, "speed");
    EXPECT_FALSE(r.autoDetectedFormat.isEmpty());   // "JSON (guessed …)"
    auto v = r.channels[0]->snapshotForRead();
    ASSERT_EQ(v.count, 3u);
    EXPECT_EQ(v.timestamps[1], 100000000LL);         // 0.1 s → ns
}
