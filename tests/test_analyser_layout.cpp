// Widget-level layout round-trip tests. The Save chart / Open chart dialogs
// are modal, so we can't drive them headlessly — but the meat of the work
// lives in AnalyserPlot::currentLayout() (build a PlotLayout from the live
// widget state, what Save embeds) and AnalyserPlot::applyLayout() (restore
// the widget from a PlotLayout, what Open does). The fixture befriends
// AnalyserPlot so it can call those two directly.

#include "AnalyserPlot.h"
#include "AddChannelDialog.h"

#include "scope/analyser/FormulaEngine.h"
#include "scope/core/Signal.h"
#include "scope/core/SignalStore.h"
#include "scope/plot/PlotLayout.h"

#include <gtest/gtest.h>

#include <QApplication>

#include <memory>
#include <vector>

using namespace scope::core;

namespace {
// One QApplication for the whole process (offscreen).
QApplication* ensureApp() {
    if (!QCoreApplication::instance()) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        static int argc = 1;
        static char a0[] = {'t', '\0'};
        static char* argv[] = {a0, nullptr};
        static QApplication app(argc, argv);
    }
    return static_cast<QApplication*>(QCoreApplication::instance());
}

void addTimeChannel(SignalStore& store, const char* name, const char* unit) {
    Signal::Meta m;
    m.name = name; m.unit = unit;
    m.dataType = DataType::Float64;
    m.domain = Signal::Domain::Time;
    std::vector<TimestampNs> ts{0, 1'000'000LL, 2'000'000LL};
    std::vector<double> vs{1.0, 2.0, 3.0};
    auto s = std::make_shared<Signal>(m);
    s->append(ts.data(), reinterpret_cast<const std::byte*>(vs.data()), ts.size());
    store.add(s);
}
}  // namespace

namespace scope::analyser::ui {

class AnalyserPlotLayoutTest : public ::testing::Test {
protected:
    void SetUp() override { ensureApp(); }
    // These run as members of the friend class, so they may touch privates.
    scope::plot::PlotLayout grab(AnalyserPlot& p) { return p.currentLayout(); }
    void apply(AnalyserPlot& p, const scope::plot::PlotLayout& l) { p.applyLayout(l); }
};

}  // namespace scope::analyser::ui

using scope::analyser::ui::AnalyserPlot;
using scope::analyser::ui::AnalyserPlotLayoutTest;

// Reopening a saved XY chart must restore the XY view. Mimics Open: channels
// already in the store, then applyLayout(); then read it back the way Save
// (currentLayout) and the file writer (toJsonString) would.
TEST_F(AnalyserPlotLayoutTest, XyViewRoundTripsThroughWidget) {
    SignalStore store;
    scope::analyser::FormulaEngine engine(store);
    AnalyserPlot plot(store, engine);

    addTimeChannel(store, "speed",  "rpm");
    addTimeChannel(store, "torque", "Nm");

    scope::plot::PlotLayout in;
    in.viewMode  = "xy";
    in.xyChannel = "speed";
    QList<scope::plot::PlotLayoutAxis> axes;
    axes.append({"rpm", "left", true, 0.0, 6000.0});
    in.axesByDomain.insert("time", axes);
    QList<scope::plot::PlotLayoutChannel> chans;
    { scope::plot::PlotLayoutChannel c; c.name = "speed";  c.axisIndex = 0; c.domain = "time"; chans.append(c); }
    { scope::plot::PlotLayoutChannel c; c.name = "torque"; c.axisIndex = 0; c.domain = "time"; chans.append(c); }
    in.channelsByDomain.insert("time", chans);

    apply(plot, in);

    // What Save chart would capture from the live widget:
    const auto out = grab(plot);
    EXPECT_EQ(out.viewMode, "xy") << "XY view not retained by currentLayout()";
    EXPECT_EQ(out.xyChannel, "speed");

    // And the JSON the file actually carries:
    const auto rt = scope::plot::PlotLayout::fromJsonString(out.toJsonString());
    EXPECT_EQ(rt.viewMode, "xy");
    EXPECT_EQ(rt.xyChannel, "speed");
}

// Time-view per-axis assignments must survive apply → currentLayout.
TEST_F(AnalyserPlotLayoutTest, TimeAxisAssignmentsRoundTripThroughWidget) {
    SignalStore store;
    scope::analyser::FormulaEngine engine(store);
    AnalyserPlot plot(store, engine);

    addTimeChannel(store, "speed",  "rpm");
    addTimeChannel(store, "torque", "Nm");

    scope::plot::PlotLayout in;
    in.viewMode = "time";
    QList<scope::plot::PlotLayoutAxis> axes;
    axes.append({"rpm", "left",  true, 0.0, 6000.0});
    axes.append({"Nm",  "right", true, 0.0, 500.0});
    in.axesByDomain.insert("time", axes);
    QList<scope::plot::PlotLayoutChannel> chans;
    { scope::plot::PlotLayoutChannel c; c.name = "speed";  c.axisIndex = 0; c.domain = "time"; chans.append(c); }
    { scope::plot::PlotLayoutChannel c; c.name = "torque"; c.axisIndex = 1; c.domain = "time"; chans.append(c); }
    in.channelsByDomain.insert("time", chans);

    apply(plot, in);
    const auto out = grab(plot);

    EXPECT_EQ(out.viewMode, "time");
    ASSERT_TRUE(out.axesByDomain.contains("time"));
    EXPECT_EQ(out.axesByDomain["time"].size(), 2);

    // torque should still be assigned to axis index 1.
    int torqueAxis = -1;
    for (const auto& c : out.channelsByDomain.value("time"))
        if (c.name == "torque") torqueAxis = c.axisIndex;
    EXPECT_EQ(torqueAxis, 1) << "axis assignment lost through the widget";
}

// Smoke test: the Add-Channel dialog (filter builder + response charts) must
// construct without crashing — exercises the QCustomPlot setup and the initial
// plotResponse() path. Separate suite so the Linux CI's AnalyserPlotLayoutTest
// exclusion (offscreen QCustomPlot teardown SIGBUS) also covers it.
TEST(AddChannelDialogTest, ConstructsAndPreviews) {
    ensureApp();
    SignalStore store;
    addTimeChannel(store, "speed", "m/s");
    scope::analyser::FormulaEngine engine(store);
    scope::analyser::ui::AddChannelDialog dlg(store, engine);
    SUCCEED();
}
