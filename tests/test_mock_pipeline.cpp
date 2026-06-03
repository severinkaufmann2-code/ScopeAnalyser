#include "scope/ads/MockAdsClient.h"
#include "scope/analyser/FormulaEngine.h"
#include "scope/core/Hdf5Session.h"
#include "scope/core/SignalStore.h"
#include "scope/recorder/NotifyChannel.h"
#include "scope/recorder/RecordingSession.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>

#include <filesystem>

using namespace scope::core;
using namespace scope::recorder;

namespace {
// Ensures a QCoreApplication exists so QTimer events fire.
struct QtAppFixture {
    QtAppFixture() {
        if (!QCoreApplication::instance()) {
            static int argc = 1;
            static char argv0[] = {'t', '\0'};
            static char* argv[] = {argv0, nullptr};
            app.reset(new QCoreApplication(argc, argv));
        }
    }
    std::unique_ptr<QCoreApplication> app;
};

void pump(int ms) {
    QElapsedTimer t; t.start();
    while (t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
}
}

TEST(MockPipeline, RecordsBothChannelsToHdf5) {
    QtAppFixture fixture;

    auto client = scope::ads::makeMockAdsClient();
    ASSERT_TRUE(client->connect({}, nullptr));

    auto symbols = client->listSymbols(nullptr);
    ASSERT_GE(symbols.size(), 2u);

    // Pick the sine (1 ms) and toggle (10 ms).
    AdsSymbol sine, toggle;
    for (const auto& s : symbols) {
        if (s.name == "Mock.sine_1hz") sine = s;
        if (s.name == "Mock.toggle")   toggle = s;
    }
    ASSERT_FALSE(sine.name.isEmpty());
    ASSERT_FALSE(toggle.name.isEmpty());

    SignalStore store;
    RecordingSession session(store, *client);

    NotifyChannel::Config cfgSine;
    cfgSine.meta.name = sine.name;
    cfgSine.meta.dataType = sine.dataType;
    cfgSine.meta.parentTaskCycleUs = client->taskCycleForSymbol(sine);
    cfgSine.meta.sampleRateHz = 1e6 / cfgSine.meta.parentTaskCycleUs;
    cfgSine.indexGroup  = sine.indexGroup;
    cfgSine.indexOffset = sine.indexOffset;
    cfgSine.cycleTimeUs = cfgSine.meta.parentTaskCycleUs;

    NotifyChannel::Config cfgToggle;
    cfgToggle.meta.name = toggle.name;
    cfgToggle.meta.dataType = toggle.dataType;
    cfgToggle.meta.parentTaskCycleUs = client->taskCycleForSymbol(toggle);
    cfgToggle.meta.sampleRateHz = 1e6 / cfgToggle.meta.parentTaskCycleUs;
    cfgToggle.indexGroup  = toggle.indexGroup;
    cfgToggle.indexOffset = toggle.indexOffset;
    cfgToggle.cycleTimeUs = cfgToggle.meta.parentTaskCycleUs;

    ASSERT_TRUE(session.addNotifyChannels({cfgSine, cfgToggle}));

    const auto path = std::filesystem::temp_directory_path() / "scope_mock_pipeline.h5";
    std::filesystem::remove(path);
    ASSERT_TRUE(session.start(path));

    pump(2000);

    session.stop();

    auto sineSig   = store.get(sine.name);
    auto toggleSig = store.get(toggle.name);
    ASSERT_TRUE(sineSig);
    ASSERT_TRUE(toggleSig);

    // The mock's nominal 1 ms tick can only be hit reliably under RT scheduling.
    // On a default Linux scheduler we typically see ~10 ms per sleep_until, so
    // we just assert "the pipeline produced a substantial number of samples".
    EXPECT_GT(sineSig->sampleCount(),   50u);
    EXPECT_LT(sineSig->sampleCount(), 5000u);
    EXPECT_GT(toggleSig->sampleCount(),   50u);
    EXPECT_LT(toggleSig->sampleCount(),  500u);

    // Verify HDF5 round-trip from disk.
    auto reader = Hdf5Session::openForRead(path);
    ASSERT_TRUE(reader);
    auto loaded = reader->loadAllSignals();
    EXPECT_EQ(loaded.size(), 2u);
    std::size_t found = 0;
    for (const auto& s : loaded) {
        if (s->meta().name == sine.name) {
            EXPECT_EQ(s->meta().dataType, DataType::Float64);
            ++found;
        }
        if (s->meta().name == toggle.name) {
            EXPECT_EQ(s->meta().dataType, DataType::Bool);
            ++found;
        }
    }
    EXPECT_EQ(found, 2u);
    std::filesystem::remove(path);
}

// Recorder → Analyser flow: record from Mock, then run a formula over the
// captured channel, verify the derived signal appears in the store.
TEST(MockPipeline, RecordThenAnalyseFlow) {
    QtAppFixture fixture;

    auto client = scope::ads::makeMockAdsClient();
    ASSERT_TRUE(client->connect({}, nullptr));

    AdsSymbol sine;
    for (const auto& s : client->listSymbols(nullptr))
        if (s.name == "Mock.sine_1hz") sine = s;
    ASSERT_FALSE(sine.name.isEmpty());

    SignalStore store;
    RecordingSession session(store, *client);

    NotifyChannel::Config cfg;
    cfg.meta.name = sine.name;
    cfg.meta.dataType = sine.dataType;
    cfg.meta.parentTaskCycleUs = client->taskCycleForSymbol(sine);
    cfg.meta.sampleRateHz = 1e6 / cfg.meta.parentTaskCycleUs;
    cfg.indexGroup  = sine.indexGroup;
    cfg.indexOffset = sine.indexOffset;
    cfg.cycleTimeUs = cfg.meta.parentTaskCycleUs;

    ASSERT_TRUE(session.addNotifyChannels({cfg}));

    const auto path = std::filesystem::temp_directory_path() / "scope_record_then_analyse.h5";
    std::filesystem::remove(path);
    ASSERT_TRUE(session.start(path));
    pump(1500);
    session.stop();

    auto sig = store.get(sine.name);
    ASSERT_TRUE(sig);
    ASSERT_GT(sig->sampleCount(), 20u);

    scope::analyser::FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("Filtered = Filter(Mock.sine_1hz, 0.05)", &err))
        << err.toStdString();

    auto filtered = store.get("Filtered");
    ASSERT_TRUE(filtered);
    EXPECT_EQ(filtered->sampleCount(), sig->sampleCount());

    std::filesystem::remove(path);
}
