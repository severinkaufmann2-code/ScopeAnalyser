#include "scope/converter/SignalIO.h"
#include "scope/core/Mdf4Session.h"
#include "scope/core/Signal.h"

#include <gtest/gtest.h>

#include <QHash>

#include <cmath>
#include <filesystem>
#include <unordered_map>

using namespace scope::core;

TEST(Mdf4Session, RoundTripFloat64) {
    auto path = std::filesystem::temp_directory_path() / "scope_mdf4_test.mf4";
    std::filesystem::remove(path);

    Signal::Meta meta;
    meta.name = "ch1";
    meta.unit = "V";
    meta.dataType = DataType::Float64;
    meta.mode = AcquisitionMode::AdsNotify;
    meta.sampleRateHz = 1000.0;
    meta.parentTaskCycleUs = 1000;
    meta.sourceSymbol = "MAIN.fVoltage";

    constexpr int N = 100;
    std::vector<TimestampNs> ts(N);
    std::vector<double> values(N);
    for (int i = 0; i < N; ++i) {
        ts[i] = static_cast<TimestampNs>(i) * 1'000'000;  // 1 ms grid
        values[i] = std::sin(i * 0.1);
    }

    {
        auto session = Mdf4Session::create(path);
        ASSERT_TRUE(session);
        ASSERT_TRUE(session->addChannel(meta));
        ASSERT_TRUE(session->appendSamples(
            "ch1", ts.data(),
            reinterpret_cast<const std::byte*>(values.data()),
            N));
        ASSERT_TRUE(session->finalize());
    }

    auto session = Mdf4Session::openForRead(path);
    ASSERT_TRUE(session);
    auto loaded = session->loadAllSignals();
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0]->meta().name, "ch1");
    EXPECT_EQ(loaded[0]->meta().unit, "V");
    EXPECT_EQ(loaded[0]->meta().sourceSymbol, "MAIN.fVoltage");
    EXPECT_EQ(loaded[0]->sampleCount(), static_cast<std::size_t>(N));

    auto view = loaded[0]->snapshotForRead();
    auto vals = loaded[0]->readAsDouble();
    for (int i = 0; i < N; ++i) {
        // Allow tiny FP error from the seconds-roundtrip in mdflib.
        EXPECT_NEAR(static_cast<double>(view.timestamps[i]) / 1e9,
                    static_cast<double>(ts[i]) / 1e9, 1e-6);
        EXPECT_NEAR(vals[i], values[i], 1e-9);
    }

    std::filesystem::remove(path);
}

TEST(Mdf4Session, DomainAttributeRoundTrip) {
    auto path = std::filesystem::temp_directory_path() / "scope_mdf4_domain.mf4";
    std::filesystem::remove(path);

    {
        auto session = Mdf4Session::create(path);
        ASSERT_TRUE(session);
        Signal::Meta meta;
        meta.name = "spec";
        meta.unit = "magnitude";
        meta.dataType = DataType::Float64;
        meta.domain = Signal::Domain::Frequency;
        ASSERT_TRUE(session->addChannel(meta));

        std::vector<TimestampNs> ts{0, 1'000'000'000LL, 2'000'000'000LL};
        std::vector<double> vs{10.0, 20.0, 30.0};
        ASSERT_TRUE(session->appendSamples("spec",
            ts.data(),
            reinterpret_cast<const std::byte*>(vs.data()),
            ts.size()));
        ASSERT_TRUE(session->finalize());
    }

    auto session = Mdf4Session::openForRead(path);
    ASSERT_TRUE(session);
    auto loaded = session->loadAllSignals();
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0]->meta().domain, Signal::Domain::Frequency);

    std::filesystem::remove(path);
}

TEST(Mdf4Session, TwoChannelsRoundTrip) {
    auto path = std::filesystem::temp_directory_path() / "scope_mdf4_two.mf4";
    std::filesystem::remove(path);

    Signal::Meta m1; m1.name = "a"; m1.unit = "rpm"; m1.dataType = DataType::Float64;
    Signal::Meta m2; m2.name = "b"; m2.unit = "Nm";  m2.dataType = DataType::Float64;

    {
        auto session = Mdf4Session::create(path);
        ASSERT_TRUE(session);
        ASSERT_TRUE(session->addChannel(m1));
        ASSERT_TRUE(session->addChannel(m2));

        std::vector<TimestampNs> ts{0, 1'000'000LL, 2'000'000LL};
        std::vector<double> v1{1.0, 2.0, 3.0};
        std::vector<double> v2{10.0, 20.0, 30.0};
        ASSERT_TRUE(session->appendSamples("a", ts.data(),
            reinterpret_cast<const std::byte*>(v1.data()), ts.size()));
        ASSERT_TRUE(session->appendSamples("b", ts.data(),
            reinterpret_cast<const std::byte*>(v2.data()), ts.size()));
        ASSERT_TRUE(session->finalize());
    }

    auto session = Mdf4Session::openForRead(path);
    ASSERT_TRUE(session);
    auto loaded = session->loadAllSignals();
    ASSERT_EQ(loaded.size(), 2u);
    // Loaded order can be either DG order; collect by name.
    std::shared_ptr<Signal> a, b;
    for (auto& s : loaded) {
        if (s->meta().name == "a") a = s;
        else if (s->meta().name == "b") b = s;
    }
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    EXPECT_EQ(a->meta().unit, "rpm");
    EXPECT_EQ(b->meta().unit, "Nm");
    EXPECT_EQ(a->sampleCount(), 3u);
    EXPECT_EQ(b->sampleCount(), 3u);

    std::filesystem::remove(path);
}

// Mimic the Analyser's saveChartDialog post-trim path: build a NEW Signal
// with meta.dataType forced to Float64, append values from a std::vector,
// then pass to SignalIO::saveFile. This is what the Analyser actually feeds
// into writeMdf4.
TEST(Mdf4Session, AnalyserTrimmedFirstChannelKeepsAllSamples) {
    auto path = std::filesystem::temp_directory_path() / "scope_mdf4_trim.mf4";
    std::filesystem::remove(path);

    constexpr int N = 1000;
    constexpr TimestampNs dtNs = 250'000;  // 4 kHz

    auto makeTrimmedLikeAnalyser = [&](const char* name,
                                       DataType origType,
                                       double base) {
        // Pretend src signal had origType. saveChartDialog rebuilds with
        // dataType=Float64 and double samples — that's what we feed in.
        Signal::Meta meta;
        meta.name = name;
        meta.unit = "V";
        meta.dataType = origType;
        meta.sourceSymbol = "MAIN.fSomething";
        meta.sampleRateHz = 4000.0;
        meta.parentTaskCycleUs = 250;

        std::vector<TimestampNs> ts(N);
        std::vector<double>      vs(N);
        for (int i = 0; i < N; ++i) {
            ts[i] = static_cast<TimestampNs>(i) * dtNs;
            vs[i] = base + std::sin(i * 0.001);
        }

        Signal::Meta trimMeta = meta;
        trimMeta.dataType = DataType::Float64;  // <-- the trim step does this
        auto trimmed = std::make_shared<Signal>(trimMeta);
        trimmed->append(ts.data(),
                        reinterpret_cast<const std::byte*>(vs.data()),
                        ts.size());
        return std::make_pair(trimmed, vs);
    };

    auto [s1, v1] = makeTrimmedLikeAnalyser("AAA_first",  DataType::Float32, 0.0);
    auto [s2, v2] = makeTrimmedLikeAnalyser("BBB_second", DataType::Float64, 100.0);
    auto [s3, v3] = makeTrimmedLikeAnalyser("CCC_third",  DataType::Int16,   200.0);

    std::vector<std::shared_ptr<Signal>> chans = {s1, s2, s3};
    QString err;
    ASSERT_TRUE(scope::converter::saveFile(
        path, chans, scope::converter::FileFormat::Mdf4, {}, &err))
        << err.toStdString();

    auto r = scope::converter::loadFile(path, scope::converter::FileFormat::Mdf4);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_EQ(r.channels.size(), 3u);

    std::unordered_map<QString, std::shared_ptr<Signal>> byName;
    for (auto& s : r.channels) byName[s->meta().name] = s;
    for (const auto& s : chans) {
        auto it = byName.find(s->meta().name);
        ASSERT_NE(it, byName.end());
        EXPECT_EQ(it->second->sampleCount(), static_cast<std::size_t>(N))
            << "channel " << s->meta().name.toStdString()
            << " has " << it->second->sampleCount() << " samples";
    }

    std::filesystem::remove(path);
}

// 4 kHz sampling, lots of samples — reproducing the user-reported case of
// "first channel only has 2 data points after round-trip via SignalIO".
TEST(Mdf4Session, SignalIOFirstChannelDoesNotGetTrimmedAt4kHz) {
    auto path = std::filesystem::temp_directory_path() / "scope_mdf4_4khz.mf4";
    std::filesystem::remove(path);

    constexpr int N = 4000;  // 1 second of 4 kHz
    constexpr TimestampNs dtNs = 250'000;  // 250 us = 4 kHz

    std::vector<TimestampNs> ts(N);
    for (int i = 0; i < N; ++i) ts[i] = static_cast<TimestampNs>(i) * dtNs;

    auto makeSig = [&](const char* name, double base) {
        Signal::Meta m; m.name = name; m.unit = "V";
        m.dataType = DataType::Float64;
        std::vector<double> v(N);
        for (int i = 0; i < N; ++i) v[i] = base + i * 0.001;
        auto s = std::make_shared<Signal>(m);
        s->append(ts.data(),
                  reinterpret_cast<const std::byte*>(v.data()), N);
        return s;
    };
    std::vector<std::shared_ptr<Signal>> chans = {
        makeSig("AAA_first",  0.0),
        makeSig("BBB_second", 100.0),
    };

    QString err;
    ASSERT_TRUE(scope::converter::saveFile(
        path, chans, scope::converter::FileFormat::Mdf4, {}, &err))
        << err.toStdString();

    auto r = scope::converter::loadFile(path, scope::converter::FileFormat::Mdf4);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_EQ(r.channels.size(), 2u);

    for (auto& s : r.channels) {
        EXPECT_EQ(s->sampleCount(), static_cast<std::size_t>(N))
            << s->meta().name.toStdString() << " sample count wrong";
    }

    std::filesystem::remove(path);
}

// Mimics the Analyser Save chart path: SignalIO::saveFile with the same
// "iterate store, addChannel + appendSamples" loop the production code uses.
TEST(Mdf4Session, SignalIOSaveFileFirstChannelHasValues) {
    auto path = std::filesystem::temp_directory_path() / "scope_mdf4_sigio.mf4";
    std::filesystem::remove(path);

    constexpr int N = 500;
    std::vector<TimestampNs> ts(N);
    for (int i = 0; i < N; ++i) ts[i] = static_cast<TimestampNs>(i) * 1'000'000;

    auto makeSig = [&](const char* name, const char* unit, int seed) {
        Signal::Meta m;
        m.name = name;
        m.unit = unit;
        m.dataType = DataType::Float64;
        std::vector<double> v(N);
        for (int i = 0; i < N; ++i) v[i] = std::sin((i + seed) * 0.01) + seed;
        auto s = std::make_shared<Signal>(m);
        s->append(ts.data(),
                  reinterpret_cast<const std::byte*>(v.data()), N);
        return std::make_pair(s, v);
    };

    auto [s1, v1] = makeSig("AAA_first",  "V",   0);
    auto [s2, v2] = makeSig("BBB_second", "rpm", 1);
    auto [s3, v3] = makeSig("CCC_third",  "Nm",  2);

    {
        auto session = Mdf4Session::create(path);
        ASSERT_TRUE(session);
        // Production order: addChannel for all, then appendSamples for all.
        ASSERT_TRUE(session->addChannel(s1->meta()));
        ASSERT_TRUE(session->addChannel(s2->meta()));
        ASSERT_TRUE(session->addChannel(s3->meta()));

        auto append = [&](std::shared_ptr<Signal> s) {
            auto vv = s->snapshotForRead();
            return session->appendSamples(s->meta().name,
                                          vv.timestamps, vv.values, vv.count);
        };
        ASSERT_TRUE(append(s1));
        ASSERT_TRUE(append(s2));
        ASSERT_TRUE(append(s3));
        ASSERT_TRUE(session->finalize());
    }

    auto session = Mdf4Session::openForRead(path);
    ASSERT_TRUE(session);
    auto loaded = session->loadAllSignals();
    ASSERT_EQ(loaded.size(), 3u);

    std::unordered_map<QString, std::shared_ptr<Signal>> byName;
    for (auto& s : loaded) byName[s->meta().name] = s;

    auto check = [&](const QString& name, const std::vector<double>& expected) {
        auto it = byName.find(name);
        ASSERT_NE(it, byName.end()) << "missing: " << name.toStdString();
        auto got = it->second->readAsDouble();
        ASSERT_EQ(got.size(), expected.size())
            << name.toStdString() << " size mismatch";
        for (std::size_t i = 0; i < got.size(); ++i) {
            EXPECT_NEAR(got[i], expected[i], 1e-9)
                << name.toStdString() << "[" << i << "] wrong";
            if (::testing::Test::HasFailure()) break;
        }
    };
    check("AAA_first",  v1);
    check("BBB_second", v2);
    check("CCC_third",  v3);

    std::filesystem::remove(path);
}

// Go through SignalIO::saveFile + loadFile — the exact path the Analyser
// uses. This is functionally identical to going through Mdf4Session directly
// but worth a separate test because that's where the user-facing bug would
// land if it's anywhere in SignalIO's plumbing.
TEST(Mdf4Session, SignalIORoundTripMatchesAnalyserPath) {
    auto path = std::filesystem::temp_directory_path() / "scope_mdf4_sigio_full.mf4";
    std::filesystem::remove(path);

    constexpr int N = 200;
    std::vector<TimestampNs> ts(N);
    for (int i = 0; i < N; ++i) ts[i] = static_cast<TimestampNs>(i) * 1'000'000;

    std::vector<std::shared_ptr<Signal>> chans;
    std::vector<std::vector<double>>     expected;
    auto add = [&](const char* name, const char* unit, double base, double freq) {
        Signal::Meta m; m.name = name; m.unit = unit;
        m.dataType = DataType::Float64;
        std::vector<double> v(N);
        for (int i = 0; i < N; ++i) v[i] = base + std::sin(i * freq);
        auto s = std::make_shared<Signal>(m);
        s->append(ts.data(),
                  reinterpret_cast<const std::byte*>(v.data()), N);
        chans.push_back(s);
        expected.push_back(std::move(v));
    };
    add("AAA", "V",   0.0, 0.05);
    add("BBB", "rpm", 5.0, 0.03);
    add("CCC", "Nm",  10.0, 0.07);

    QString err;
    ASSERT_TRUE(scope::converter::saveFile(
        path, chans,
        scope::converter::FileFormat::Mdf4, {}, &err))
        << err.toStdString();

    auto r = scope::converter::loadFile(path, scope::converter::FileFormat::Mdf4);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_EQ(r.channels.size(), 3u);

    std::unordered_map<QString, std::shared_ptr<Signal>> byName;
    for (auto& s : r.channels) byName[s->meta().name] = s;

    for (std::size_t k = 0; k < chans.size(); ++k) {
        const QString name = chans[k]->meta().name;
        auto it = byName.find(name);
        ASSERT_NE(it, byName.end());
        auto got = it->second->readAsDouble();
        ASSERT_EQ(got.size(), expected[k].size());
        for (std::size_t i = 0; i < got.size(); ++i) {
            EXPECT_NEAR(got[i], expected[k][i], 1e-9)
                << name.toStdString() << "[" << i << "] wrong";
            if (::testing::Test::HasFailure()) break;
        }
    }

    std::filesystem::remove(path);
}

// Many-channel stress test mimicking a real Analyser save: 10 channels of
// 200 samples each, distinct value patterns, mixed time/frequency domains
// and a derived channel (sourceSymbol contains '=' and ',').
TEST(Mdf4Session, ManyChannelsRoundTripValuesMatch) {
    auto path = std::filesystem::temp_directory_path() / "scope_mdf4_many.mf4";
    std::filesystem::remove(path);

    constexpr int N = 200;
    constexpr int kChans = 10;

    std::vector<TimestampNs> ts(N);
    for (int i = 0; i < N; ++i) ts[i] = static_cast<TimestampNs>(i) * 1'000'000;

    // Per-channel value pattern: each channel gets a distinct slope so we can
    // detect "wrong channel's data" easily (slopes don't accidentally match).
    auto pattern = [](int chanIdx, int i) {
        return static_cast<double>(chanIdx) + 0.01 * static_cast<double>(i);
    };

    {
        auto session = Mdf4Session::create(path);
        ASSERT_TRUE(session);

        for (int c = 0; c < kChans; ++c) {
            Signal::Meta m;
            m.name = QString("ch_%1").arg(c);
            m.unit = (c == 3) ? QString("m/s\xc2\xb2")  // unicode unit
                              : (c == 7) ? QString("[weird;unit]")  // ';' in unit
                                         : QString("V");
            m.dataType = DataType::Float64;
            m.domain = (c >= 8) ? Signal::Domain::Frequency
                                : Signal::Domain::Time;
            if (c == 5) {
                // Derived-channel marker: source_symbol contains '=' and ','
                m.sourceSymbol = "ch_5 = Filter(ch_0, 0.01)";
            }
            ASSERT_TRUE(session->addChannel(m));
        }

        std::vector<double> vals(N);
        for (int c = 0; c < kChans; ++c) {
            for (int i = 0; i < N; ++i) vals[i] = pattern(c, i);
            const QString name = QString("ch_%1").arg(c);
            ASSERT_TRUE(session->appendSamples(
                name, ts.data(),
                reinterpret_cast<const std::byte*>(vals.data()), N))
                << "appendSamples failed for " << name.toStdString();
        }
        ASSERT_TRUE(session->finalize());
    }

    auto session = Mdf4Session::openForRead(path);
    ASSERT_TRUE(session);
    auto loaded = session->loadAllSignals();
    ASSERT_EQ(loaded.size(), static_cast<std::size_t>(kChans));

    std::unordered_map<QString, std::shared_ptr<Signal>> byName;
    for (auto& s : loaded) byName[s->meta().name] = s;

    for (int c = 0; c < kChans; ++c) {
        const QString name = QString("ch_%1").arg(c);
        auto it = byName.find(name);
        ASSERT_NE(it, byName.end()) << "missing channel: " << name.toStdString();
        auto sig = it->second;
        ASSERT_EQ(sig->sampleCount(), static_cast<std::size_t>(N))
            << name.toStdString() << " has wrong sample count";
        auto vs = sig->readAsDouble();
        for (int i = 0; i < N; ++i) {
            EXPECT_NEAR(vs[i], pattern(c, i), 1e-9)
                << name.toStdString() << " sample[" << i << "] wrong";
            if (::testing::Test::HasFailure()) break;
        }
    }

    std::filesystem::remove(path);
}
