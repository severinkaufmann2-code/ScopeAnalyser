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
#include "scope/plot/ScopePlot.h"

#include <qcustomplot.h>

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QListWidget>

#include <cmath>
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

    // ---- display-correctness helpers (also via friendship) ----
    QCPAbstractPlottable* plottableFor(AnalyserPlot& p, const QString& name) {
        return p.plotted_.value(name, nullptr);
    }
    int rowOf(AnalyserPlot& p, const QString& name) {
        for (int r = 0; r < p.table_->rowCount(); ++r)
            if (p.table_->item(r, 1)->text() == name) return r;
        return -1;
    }
    void setRowVisible(AnalyserPlot& p, const QString& name, bool on) {
        const int r = rowOf(p, name);
        ASSERT_GE(r, 0) << name.toStdString() << " not in table";
        auto* cb = qobject_cast<QCheckBox*>(p.table_->cellWidget(r, 0));
        ASSERT_NE(cb, nullptr);
        cb->setChecked(on);
    }
    void setViewIndex(AnalyserPlot& p, int idx) {            // 0=Time 1=Freq 2=XY
        p.viewCombo_->setCurrentIndex(idx);
    }
    void setXyXChannel(AnalyserPlot& p, const QString& name) {
        const int i = p.xyXCombo_->findData(name);
        ASSERT_GE(i, 0) << name.toStdString() << " not in XY combo";
        p.xyXCombo_->setCurrentIndex(i);
    }
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

    // Selecting each function fires onFunctionHovered → plotResponse (filters)
    // or plotExample (everything else, which runs the real impl on a synthetic
    // input). Walk the whole list to make sure none crash.
    auto* list = dlg.findChild<QListWidget*>();
    ASSERT_NE(list, nullptr);
    EXPECT_GT(list->count(), 0);
    for (int i = 0; i < list->count(); ++i) list->setCurrentRow(i);
    SUCCEED();
}

// ===========================================================================
// Display correctness — what lands on the chart must be exactly the data.
// ===========================================================================

namespace {
void addChannelAt(SignalStore& store, const char* name,
                  Signal::Domain domain, TimestampNs startNs, TimestampNs dtNs,
                  const std::vector<double>& vs) {
    Signal::Meta m;
    m.name = name;
    m.dataType = DataType::Float64;
    m.domain = domain;
    std::vector<TimestampNs> ts(vs.size());
    for (std::size_t i = 0; i < vs.size(); ++i)
        ts[i] = startNs + static_cast<TimestampNs>(i) * dtNs;
    auto s = std::make_shared<Signal>(m);
    s->append(ts.data(), reinterpret_cast<const std::byte*>(vs.data()), vs.size());
    store.add(s);
}
}  // namespace

// Time view plots x = t/1e9 − base, with base = the earliest first sample
// among the active channels — so absolute recorder timestamps become
// readable seconds AND the relative offset between channels is preserved.
TEST_F(AnalyserPlotLayoutTest, TimeViewMapsTimestampsToSecondsRelativeToEarliestActive) {
    SignalStore store;
    scope::analyser::FormulaEngine engine(store);
    AnalyserPlot plot(store, engine);

    addChannelAt(store, "A", Signal::Domain::Time,
                 5'000'000'000LL, 1'000'000LL, {1.0, 2.0, 3.0});
    addChannelAt(store, "B", Signal::Domain::Time,
                 7'000'000'000LL, 1'000'000LL, {10.0, 20.0, 30.0});

    auto* ga = qobject_cast<QCPGraph*>(plottableFor(plot, "A"));
    auto* gb = qobject_cast<QCPGraph*>(plottableFor(plot, "B"));
    ASSERT_NE(ga, nullptr);
    ASSERT_NE(gb, nullptr);
    ASSERT_EQ(ga->data()->size(), 3);
    ASSERT_EQ(gb->data()->size(), 3);

    const double base = 5'000'000'000LL / 1e9;   // earliest active start
    for (int i = 0; i < 3; ++i) {
        const double expectedXa = (5'000'000'000LL + i * 1'000'000LL) / 1e9 - base;
        const double expectedXb = (7'000'000'000LL + i * 1'000'000LL) / 1e9 - base;
        EXPECT_DOUBLE_EQ(ga->data()->at(i)->key, expectedXa) << "A x[" << i << "]";
        EXPECT_DOUBLE_EQ(gb->data()->at(i)->key, expectedXb) << "B x[" << i << "]";
        EXPECT_DOUBLE_EQ(ga->data()->at(i)->value, (i + 1) * 1.0) << "A y[" << i << "]";
        EXPECT_DOUBLE_EQ(gb->data()->at(i)->value, (i + 1) * 10.0) << "B y[" << i << "]";
    }
    // B starts exactly 2 s after A on the shared axis.
    EXPECT_DOUBLE_EQ(gb->data()->at(0)->key - ga->data()->at(0)->key, 2.0);
}

// Frequency view: a spectrum's bin-k timestamp of k×1e9 ns must land at
// exactly k Hz on the X axis (bin 0 anchors the base at 0).
TEST_F(AnalyserPlotLayoutTest, FrequencyViewXAxisIsExactlyHz) {
    SignalStore store;
    scope::analyser::FormulaEngine engine(store);
    AnalyserPlot plot(store, engine);

    addChannelAt(store, "spec", Signal::Domain::Frequency,
                 0, 1'000'000'000LL, {5.0, 4.0, 3.0, 2.0, 1.0});

    setViewIndex(plot, 1);   // Frequency
    auto* g = qobject_cast<QCPGraph*>(plottableFor(plot, "spec"));
    ASSERT_NE(g, nullptr);
    ASSERT_EQ(g->data()->size(), 5);
    for (int k = 0; k < 5; ++k) {
        EXPECT_DOUBLE_EQ(g->data()->at(k)->key, static_cast<double>(k))
            << "bin " << k << " must plot at " << k << " Hz";
        EXPECT_DOUBLE_EQ(g->data()->at(k)->value, 5.0 - k);
    }
}

// XY with identical grids pairs strictly by index — no interpolation, no
// reordering: point i is exactly (x[i], y[i]).
TEST_F(AnalyserPlotLayoutTest, XySameGridPairsValuesExactly) {
    SignalStore store;
    scope::analyser::FormulaEngine engine(store);
    AnalyserPlot plot(store, engine);

    addChannelAt(store, "X", Signal::Domain::Time,
                 0, 100'000'000LL, {10.0, 20.0, 30.0});
    addChannelAt(store, "Y", Signal::Domain::Time,
                 0, 100'000'000LL, {1.0, 2.0, 3.0});

    setViewIndex(plot, 2);     // XY
    setXyXChannel(plot, "X");
    auto* curve = qobject_cast<QCPCurve*>(plottableFor(plot, "Y"));
    ASSERT_NE(curve, nullptr);
    ASSERT_EQ(curve->data()->size(), 3);
    for (int i = 0; i < 3; ++i) {
        EXPECT_DOUBLE_EQ(curve->data()->at(i)->key, (i + 1) * 10.0);
        EXPECT_DOUBLE_EQ(curve->data()->at(i)->value, (i + 1) * 1.0);
    }
}

// XY with different grids linearly interpolates Y onto X's timestamps.
// Y = 20·t sampled at 5 Hz against X sampled at 10 Hz: the in-between X
// samples must read the exact midpoint values.
TEST_F(AnalyserPlotLayoutTest, XyDifferentGridInterpolatesLinearly) {
    SignalStore store;
    scope::analyser::FormulaEngine engine(store);
    AnalyserPlot plot(store, engine);

    std::vector<double> xs(11), ys(6);
    for (int i = 0; i < 11; ++i) xs[i] = i;            // X value = sample idx
    for (int i = 0; i < 6;  ++i) ys[i] = 20.0 * (0.2 * i);   // 20·t at 5 Hz
    addChannelAt(store, "X", Signal::Domain::Time, 0, 100'000'000LL, xs);
    addChannelAt(store, "Y", Signal::Domain::Time, 0, 200'000'000LL, ys);

    setViewIndex(plot, 2);
    setXyXChannel(plot, "X");
    auto* curve = qobject_cast<QCPCurve*>(plottableFor(plot, "Y"));
    ASSERT_NE(curve, nullptr);
    ASSERT_EQ(curve->data()->size(), 11);
    for (int i = 0; i < 11; ++i) {
        const double t = 0.1 * i;
        EXPECT_DOUBLE_EQ(curve->data()->at(i)->key, xs[i]);
        EXPECT_DOUBLE_EQ(curve->data()->at(i)->value, 20.0 * t)
            << "Y not linearly interpolated at t=" << t;
    }
}

// Unchecking a channel must remove its plottable — the chart shows exactly
// the selected set, nothing more.
TEST_F(AnalyserPlotLayoutTest, UncheckedChannelLeavesThePlot) {
    SignalStore store;
    scope::analyser::FormulaEngine engine(store);
    AnalyserPlot plot(store, engine);

    addChannelAt(store, "A", Signal::Domain::Time, 0, 1'000'000LL, {1, 2, 3});
    addChannelAt(store, "B", Signal::Domain::Time, 0, 1'000'000LL, {4, 5, 6});
    ASSERT_NE(plottableFor(plot, "A"), nullptr);
    ASSERT_NE(plottableFor(plot, "B"), nullptr);

    setRowVisible(plot, "A", false);
    EXPECT_EQ(plottableFor(plot, "A"), nullptr) << "hidden channel still drawn";
    ASSERT_NE(plottableFor(plot, "B"), nullptr);

    setRowVisible(plot, "A", true);
    EXPECT_NE(plottableFor(plot, "A"), nullptr);
}

// Finding B4 regression guard: outside Y's recorded range the XY curve gets
// NaN (a gap), never clamped first/last values — an engineer must be able
// to trust that every drawn XY point is real data.
TEST_F(AnalyserPlotLayoutTest, XyOutsideYRangeLeavesGapNotFabricatedPoints) {
    SignalStore store;
    scope::analyser::FormulaEngine engine(store);
    AnalyserPlot plot(store, engine);

    std::vector<double> xs(11);
    for (int i = 0; i < 11; ++i) xs[i] = i;
    addChannelAt(store, "X", Signal::Domain::Time, 0, 100'000'000LL, xs);
    // Y exists only from 0.3 s to 0.5 s.
    addChannelAt(store, "Y", Signal::Domain::Time,
                 300'000'000LL, 100'000'000LL, {30.0, 40.0, 50.0});

    setViewIndex(plot, 2);
    setXyXChannel(plot, "X");
    auto* curve = qobject_cast<QCPCurve*>(plottableFor(plot, "Y"));
    ASSERT_NE(curve, nullptr);
    ASSERT_EQ(curve->data()->size(), 11);
    for (int i = 0; i < 11; ++i) {
        const double t = 0.1 * i;
        const double v = curve->data()->at(i)->value;
        if (t < 0.3 || t > 0.5) {
            EXPECT_TRUE(std::isnan(v))
                << "fabricated Y=" << v << " at t=" << t
                << " where Y has no data";
        } else {
            EXPECT_DOUBLE_EQ(v, 100.0 * t);
        }
    }
}
