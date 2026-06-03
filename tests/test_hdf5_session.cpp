#include "scope/core/Hdf5Session.h"
#include "scope/core/Signal.h"

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
    auto signals = session->loadAllSignals();
    ASSERT_EQ(signals.size(), 1u);
    EXPECT_EQ(signals[0]->meta().name, "ch1");
    EXPECT_EQ(signals[0]->meta().unit, "V");
    EXPECT_EQ(signals[0]->meta().dataType, DataType::Float64);
    EXPECT_EQ(signals[0]->sampleCount(), 100u);

    std::filesystem::remove(path);
}
