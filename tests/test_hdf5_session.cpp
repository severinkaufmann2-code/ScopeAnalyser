#include "scope/core/Hdf5Session.h"
#include "scope/core/Signal.h"
#include "scope/converter/SignalIO.h"
#include "scope/plot/PlotLayout.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <random>

using namespace scope::core;

TEST(Hdf5Session, RoundTripFloat64) {
    auto path = std::filesystem::temp_directory_path() / "scope_hdf5_test.h5";
    std::filesystem::remove(path);

    Signal::Meta meta;
    meta.name = "ch1";
    meta.unit = "V";
    meta.dataType = DataType::Float64;
    meta.mode = AcquisitionMode::AdsNotify;
    meta.sampleRateHz = 1000.0;
    meta.parentTaskCycleUs = 1000;
    meta.sourceSymbol = "MAIN.fVoltage";

    {
        auto session = Hdf5Session::create(path);
        ASSERT_TRUE(session);
        ASSERT_TRUE(session->addChannel(meta));

        constexpr int N = 100;
        std::vector<TimestampNs> ts(N);
        std::vector<double> values(N);
        for (int i = 0; i < N; ++i) {
            ts[i] = static_cast<TimestampNs>(i) * 1'000'000;
            values[i] = std::sin(i * 0.1);
        }
        ASSERT_TRUE(session->appendSamples(
            "ch1", ts.data(),
            reinterpret_cast<const std::byte*>(values.data()),
            N));
        session->flush();
    }

    auto session = Hdf5Session::openForRead(path);
    ASSERT_TRUE(session);
    auto loaded = session->loadAllSignals();
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0]->meta().name, "ch1");
    EXPECT_EQ(loaded[0]->meta().unit, "V");
    EXPECT_EQ(loaded[0]->meta().dataType, DataType::Float64);
    EXPECT_EQ(loaded[0]->sampleCount(), 100u);

    session.reset();  // close the file before removing it (Windows locks open files)
    std::filesystem::remove(path);
}

TEST(Hdf5Session, DomainAttributeRoundTrip) {
    auto path = std::filesystem::temp_directory_path() / "scope_test_h5_domain.h5";
    {
        auto session = Hdf5Session::create(path);
        ASSERT_TRUE(session);
        Signal::Meta meta;
        meta.name = "spec";
        meta.unit = "magnitude";
        meta.dataType = DataType::Float64;
        meta.domain = Signal::Domain::Frequency;   // <— the bit we're testing
        ASSERT_TRUE(session->addChannel(meta));

        std::vector<TimestampNs> ts{0, 1'000'000'000LL, 2'000'000'000LL};
        std::vector<double> vs{10.0, 20.0, 30.0};
        ASSERT_TRUE(session->appendSamples("spec",
            ts.data(),
            reinterpret_cast<const std::byte*>(vs.data()),
            ts.size()));
        session->flush();
    }

    auto session = Hdf5Session::openForRead(path);
    ASSERT_TRUE(session);
    auto loaded = session->loadAllSignals();
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0]->meta().domain, Signal::Domain::Frequency);
    EXPECT_EQ(loaded[0]->sampleCount(), 3u);

    session.reset();  // close the file before removing it (Windows locks open files)
    std::filesystem::remove(path);
}

// End-to-end via SignalIO (the path the Analyser uses): a saved .h5 carries
// the embedded PlotLayout in its /metadata attribute; a plain save without a
// layout reads back empty.
TEST(Hdf5Session, SignalIOEmbedsLayout) {
    auto path = std::filesystem::temp_directory_path() / "scope_h5_layout.h5";

    Signal::Meta m; m.name = "speed"; m.unit = "rpm"; m.dataType = DataType::Float64;
    std::vector<TimestampNs> ts{0, 1'000'000LL, 2'000'000LL};
    std::vector<double> vs{1.0, 2.0, 3.0};
    auto sig = std::make_shared<Signal>(m);
    sig->append(ts.data(), reinterpret_cast<const std::byte*>(vs.data()), ts.size());
    std::vector<std::shared_ptr<Signal>> chans{sig};

    using namespace scope::converter;

    // No layout → loads back empty (foreign / non-Analyser files behave so).
    std::filesystem::remove(path);
    QString err;
    ASSERT_TRUE(saveFile(path, chans, FileFormat::Hdf5, {}, &err)) << err.toStdString();
    EXPECT_TRUE(loadFile(path, FileFormat::Hdf5).layoutJson.isEmpty());

    // With layout → round-trips and restores the meaningful fields.
    scope::plot::PlotLayout lay;
    lay.viewMode = "frequency";
    QList<scope::plot::PlotLayoutAxis> a; a.append({"rpm", "left", true, 0.0, 6000.0});
    lay.axesByDomain.insert("time", a);
    QList<scope::plot::PlotLayoutChannel> c;
    { scope::plot::PlotLayoutChannel ch; ch.name = "speed"; ch.axisIndex = 0; ch.domain = "time"; c.append(ch); }
    lay.channelsByDomain.insert("time", c);

    SaveOptions opts; opts.layoutJson = lay.toJsonString();
    std::filesystem::remove(path);
    ASSERT_TRUE(saveFile(path, chans, FileFormat::Hdf5, opts, &err)) << err.toStdString();

    auto r = loadFile(path, FileFormat::Hdf5);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_FALSE(r.layoutJson.isEmpty());
    const auto back = scope::plot::PlotLayout::fromJsonString(r.layoutJson);
    EXPECT_EQ(back.viewMode, "frequency");
    ASSERT_TRUE(back.axesByDomain.contains("time"));
    EXPECT_EQ(back.axesByDomain["time"][0].label, "rpm");
    ASSERT_TRUE(back.channelsByDomain.contains("time"));
    EXPECT_EQ(back.channelsByDomain["time"][0].name, "speed");

    std::filesystem::remove(path);
}
