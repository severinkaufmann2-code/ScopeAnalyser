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
#include <QHash>
#include <QListWidget>

#include <cmath>
#include <memory>
#include <vector>

using namespace scope::core;

namespace {
// One QApplication for the whole process (offscreen).
//
// Deliberately leaked. Held in a function-static it is destroyed during
// static destruction, by which point parts of Qt have already torn down —
// under the offscreen platform that crashes AFTER the test has passed
// ([ OK ] then SIGSEGV/SIGBUS). That is what linux.yml excludes
// AnalyserPlotLayoutTest and AddChannelDialogTest for. The process is exiting
// anyway, so never destroying it is the fix, not a workaround.
QApplication* ensureApp() {
    if (!QCoreApplication::instance()) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        static int argc = 1;
        static char a0[] = {'t', '\0'};
        static char* argv[] = {a0, nullptr};
        new QApplication(argc, argv);   // NOLINT — intentionally not deleted
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
    void apply(AnalyserPlot& p, const scope::plot::PlotLayout& l,
               AnalyserPlot::FormulaImport mode) {
        p.applyLayout(l, mode);
    }
    // What openChartDialog does for an additional file: merge the layout's
    // axes into whatever is already on screen.
    void applyMerge(AnalyserPlot& p, const scope::plot::PlotLayout& l) {
        p.applyLayout(l, AnalyserPlot::FormulaImport::Recalculate,
                      AnalyserPlot::LayoutApply::Merge);
    }
    void remapNames(scope::plot::PlotLayout& l,
                    const QHash<QString, QString>& renamed) {
        AnalyserPlot::remapLayoutChannelNames(l, renamed);
    }
    // Mimic a user renaming a Y axis via the plot's context menu.
    void renameAxis(AnalyserPlot& p, int idx, const QString& label) {
        p.scope_->yAxis(idx)->setLabel(label);
        emit p.scope_->yAxesChanged();
    }

    // ---- undo/redo snapshot round-trip (via friendship) ----
    AnalyserPlot::UndoSnap captureUndo(AnalyserPlot& p) { return p.captureState(); }
    void restoreUndo(AnalyserPlot& p, const AnalyserPlot::UndoSnap& s) {
        p.restoreState(s);
    }

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
    scope::converter::HtmlExportView htmlView(AnalyserPlot& p) {
        return p.htmlExportView();
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

// Opening a second chart must NOT clobber the first file's axis names.
// Merge keeps the existing axes and appends the new file's distinct ones,
// remapping the new channels onto them.
TEST_F(AnalyserPlotLayoutTest, MergeKeepsExistingAxisNamesAndAppendsNew) {
    SignalStore store;
    scope::analyser::FormulaEngine engine(store);
    AnalyserPlot plot(store, engine);

    // File 1 — two channels on two named axes (Replace, like the first open).
    addTimeChannel(store, "speed",  "rpm");
    addTimeChannel(store, "torque", "Nm");
    scope::plot::PlotLayout l1;
    l1.viewMode = "time";
    l1.axesByDomain.insert("time", {{"rpm", "left", true, 0.0, 6000.0},
                                    {"Nm",  "right", true, 0.0, 500.0}});
    QList<scope::plot::PlotLayoutChannel> c1;
    { scope::plot::PlotLayoutChannel c; c.name = "speed";  c.axisIndex = 0; c.domain = "time"; c1.append(c); }
    { scope::plot::PlotLayoutChannel c; c.name = "torque"; c.axisIndex = 1; c.domain = "time"; c1.append(c); }
    l1.channelsByDomain.insert("time", c1);
    apply(plot, l1);

    // File 2 — two more channels on two DIFFERENT named axes (Merge).
    addTimeChannel(store, "pressure", "bar");
    addTimeChannel(store, "temp",     "degC");
    scope::plot::PlotLayout l2;
    l2.viewMode = "time";
    l2.axesByDomain.insert("time", {{"bar",  "left",  true, 0.0, 10.0},
                                    {"degC", "right", true, 0.0, 100.0}});
    QList<scope::plot::PlotLayoutChannel> c2;
    { scope::plot::PlotLayoutChannel c; c.name = "pressure"; c.axisIndex = 0; c.domain = "time"; c2.append(c); }
    { scope::plot::PlotLayoutChannel c; c.name = "temp";     c.axisIndex = 1; c.domain = "time"; c2.append(c); }
    l2.channelsByDomain.insert("time", c2);
    applyMerge(plot, l2);

    const auto out = grab(plot);
    ASSERT_TRUE(out.axesByDomain.contains("time"));
    const auto& axes = out.axesByDomain["time"];
    ASSERT_EQ(axes.size(), 4) << "second file should append its axes, not replace";
    EXPECT_EQ(axes[0].label, "rpm");   // file 1's names preserved
    EXPECT_EQ(axes[1].label, "Nm");
    EXPECT_EQ(axes[2].label, "bar");   // file 2's appended
    EXPECT_EQ(axes[3].label, "degC");

    auto axisOf = [&](const QString& n) {
        for (const auto& c : out.channelsByDomain.value("time"))
            if (c.name == n) return c.axisIndex;
        return -1;
    };
    EXPECT_EQ(axisOf("speed"),    0);
    EXPECT_EQ(axisOf("torque"),   1);
    EXPECT_EQ(axisOf("pressure"), 2);  // remapped onto the appended axes
    EXPECT_EQ(axisOf("temp"),     3);
}

// A second file whose axis shares a label with an existing one reuses that
// axis (no duplicate) and lands its channel there.
TEST_F(AnalyserPlotLayoutTest, MergeReusesAxisWithSameLabel) {
    SignalStore store;
    scope::analyser::FormulaEngine engine(store);
    AnalyserPlot plot(store, engine);

    addTimeChannel(store, "speedA", "rpm");
    scope::plot::PlotLayout l1;
    l1.viewMode = "time";
    l1.axesByDomain.insert("time", {{"rpm", "left", true, 0.0, 6000.0}});
    QList<scope::plot::PlotLayoutChannel> c1;
    { scope::plot::PlotLayoutChannel c; c.name = "speedA"; c.axisIndex = 0; c.domain = "time"; c1.append(c); }
    l1.channelsByDomain.insert("time", c1);
    apply(plot, l1);

    addTimeChannel(store, "speedB", "rpm");
    scope::plot::PlotLayout l2;
    l2.viewMode = "time";
    l2.axesByDomain.insert("time", {{"rpm", "left", true, 0.0, 8000.0}});
    QList<scope::plot::PlotLayoutChannel> c2;
    { scope::plot::PlotLayoutChannel c; c.name = "speedB"; c.axisIndex = 0; c.domain = "time"; c2.append(c); }
    l2.channelsByDomain.insert("time", c2);
    applyMerge(plot, l2);

    const auto out = grab(plot);
    ASSERT_TRUE(out.axesByDomain.contains("time"));
    EXPECT_EQ(out.axesByDomain["time"].size(), 1) << "same-label axis should be reused";
    for (const auto& c : out.channelsByDomain.value("time"))
        EXPECT_EQ(c.axisIndex, 0) << c.name.toStdString() << " should share the rpm axis";
}

// A second file whose channel name collides with one already loaded is
// renamed for uniqueness on open; remapping the layout's references through
// that rename keeps the renamed channel's saved axis assignment.
TEST_F(AnalyserPlotLayoutTest, MergeHonoursRenamedCollidingChannel) {
    SignalStore store;
    scope::analyser::FormulaEngine engine(store);
    AnalyserPlot plot(store, engine);

    // File 1 — "speed" on the "rpm" axis.
    addTimeChannel(store, "speed", "rpm");
    scope::plot::PlotLayout l1;
    l1.viewMode = "time";
    l1.axesByDomain.insert("time", {{"rpm", "left", true, 0.0, 6000.0}});
    QList<scope::plot::PlotLayoutChannel> c1;
    { scope::plot::PlotLayoutChannel c; c.name = "speed"; c.axisIndex = 0; c.domain = "time"; c1.append(c); }
    l1.channelsByDomain.insert("time", c1);
    apply(plot, l1);

    // File 2 — also a "speed", which openChartDialog renames to "speed (2)";
    // its layout (still referencing "speed") puts it on a new "kph" axis.
    addTimeChannel(store, "speed (2)", "kph");
    scope::plot::PlotLayout l2;
    l2.viewMode = "time";
    l2.axesByDomain.insert("time", {{"kph", "left", true, 0.0, 200.0}});
    QList<scope::plot::PlotLayoutChannel> c2;
    { scope::plot::PlotLayoutChannel c; c.name = "speed"; c.axisIndex = 0; c.domain = "time"; c2.append(c); }
    l2.channelsByDomain.insert("time", c2);

    QHash<QString, QString> renamed;
    renamed.insert("speed", "speed (2)");
    remapNames(l2, renamed);          // what openChartDialog does post-rename
    applyMerge(plot, l2);

    const auto out = grab(plot);
    ASSERT_TRUE(out.axesByDomain.contains("time"));
    ASSERT_EQ(out.axesByDomain["time"].size(), 2);
    EXPECT_EQ(out.axesByDomain["time"][1].label, "kph");

    int axisOfRenamed = -1;
    for (const auto& c : out.channelsByDomain.value("time"))
        if (c.name == "speed (2)") axisOfRenamed = c.axisIndex;
    EXPECT_EQ(axisOfRenamed, 1) << "renamed channel lost its axis after merge";
}

// The reported bug: rename an axis, then open a chart whose embedded layout
// uses the axis's OLD name AND a channel whose name collides (dedup-renamed).
// The rename must survive and the opened channel must land on its own axis.
TEST_F(AnalyserPlotLayoutTest, RenameThenOpenChartWithOldAxisNameKeepsRename) {
    SignalStore store;
    scope::analyser::FormulaEngine engine(store);
    AnalyserPlot plot(store, engine);

    // File 1 (no layout): channel sits on the default "Y1" axis.
    addTimeChannel(store, "sig", "");

    // User renames that axis via the plot UI.
    renameAxis(plot, 0, "Engine Speed");

    // File 2: SAME channel name (dedup-renamed to "sig (2)"); its embedded
    // layout references the axis's OLD name "Y1".
    addTimeChannel(store, "sig (2)", "");
    scope::plot::PlotLayout l2;
    l2.viewMode = "time";
    l2.axesByDomain.insert("time", {{"Y1", "left", false, 0.0, 0.0}});
    QList<scope::plot::PlotLayoutChannel> c2;
    { scope::plot::PlotLayoutChannel c; c.name = "sig"; c.axisIndex = 0; c.domain = "time"; c2.append(c); }
    l2.channelsByDomain.insert("time", c2);
    QHash<QString, QString> renamed;
    renamed.insert("sig", "sig (2)");
    remapNames(l2, renamed);          // openChartDialog does this post-rename
    applyMerge(plot, l2);

    const auto out = grab(plot);
    ASSERT_EQ(out.axesByDomain["time"].size(), 2);
    EXPECT_EQ(out.axesByDomain["time"][0].label, "Engine Speed")
        << "the rename was clobbered by the opened chart";
    EXPECT_EQ(out.axesByDomain["time"][1].label, "Y1");
    int axisOfNew = -1;
    for (const auto& c : out.channelsByDomain.value("time"))
        if (c.name == "sig (2)") axisOfNew = c.axisIndex;
    EXPECT_EQ(axisOfNew, 1) << "opened channel didn't land on its own axis";
}

// The Open-chart formula-import modes, applied per channel. Mirrors Open:
// data already in the store, then applyLayout with the chosen mode.
//   "double"  — a CHOICE channel: its data is in the store. The mode decides.
//   "triple"  — a FORMULA-ONLY channel: in the layout but not the store (no
//               data was saved). Always recomputed, regardless of the mode.
TEST_F(AnalyserPlotLayoutTest, FormulaImportModes) {
    // "double" = 2*speed, loaded with deliberately stale values and the
    // formula text in sourceSymbol (as a round-tripped file would carry it).
    auto addStaleDouble = [](SignalStore& store) {
        Signal::Meta m;
        m.name = "double"; m.unit = "rpm";
        m.dataType = DataType::Float64; m.domain = Signal::Domain::Time;
        m.sourceSymbol = "double = 2 * speed";
        std::vector<TimestampNs> ts{0, 1'000'000LL, 2'000'000LL};
        std::vector<double> vs{99.0, 99.0, 99.0};
        auto s = std::make_shared<Signal>(m);
        s->append(ts.data(), reinterpret_cast<const std::byte*>(vs.data()), ts.size());
        store.add(s);
    };
    // Layout lists speed, the data-bearing "double", and the formula-only
    // "triple" (no matching channel in the store).
    auto layout = [] {
        scope::plot::PlotLayout in; in.viewMode = "time";
        QList<scope::plot::PlotLayoutAxis> axes;
        axes.append({"rpm", "left", false, 0.0, 0.0});
        in.axesByDomain.insert("time", axes);
        QList<scope::plot::PlotLayoutChannel> chans;
        { scope::plot::PlotLayoutChannel c; c.name = "speed";  c.axisIndex = 0; c.domain = "time"; chans.append(c); }
        { scope::plot::PlotLayoutChannel c; c.name = "double"; c.axisIndex = 0; c.domain = "time"; c.formula = "2 * speed"; chans.append(c); }
        { scope::plot::PlotLayoutChannel c; c.name = "triple"; c.axisIndex = 0; c.domain = "time"; c.formula = "3 * speed"; chans.append(c); }
        in.channelsByDomain.insert("time", chans);
        return in;
    };
    using FI = AnalyserPlot::FormulaImport;

    // ImportDataOnly — "double" keeps its values and loses its formula (plain
    // channel); "triple" has no data, so it is still imported + computed.
    {
        SignalStore store;
        scope::analyser::FormulaEngine engine(store);
        AnalyserPlot plot(store, engine);
        addTimeChannel(store, "speed", "rpm");   // {1, 2, 3}
        addStaleDouble(store);                   // {99, 99, 99}, NO "triple"
        apply(plot, layout(), FI::ImportDataOnly);

        auto dbl = store.get("double");
        auto dv = dbl->readAsDouble();
        ASSERT_EQ(dv.size(), 3u);
        EXPECT_DOUBLE_EQ(dv[0], 99.0);
        EXPECT_DOUBLE_EQ(dv[2], 99.0);
        EXPECT_TRUE(engine.formulaFor("double").isEmpty())
            << "data-bearing channel must not be registered";
        EXPECT_TRUE(dbl->meta().sourceSymbol.isEmpty())
            << "data-bearing channel must be stripped to a plain signal";

        ASSERT_TRUE(store.contains("triple"))
            << "formula-only channel must be imported even under data-only";
        auto tv = store.get("triple")->readAsDouble();
        ASSERT_EQ(tv.size(), 3u);
        EXPECT_DOUBLE_EQ(tv[0], 3.0);
        EXPECT_DOUBLE_EQ(tv[2], 9.0);
    }
    // Recalculate — both are re-derived from speed.
    {
        SignalStore store;
        scope::analyser::FormulaEngine engine(store);
        AnalyserPlot plot(store, engine);
        addTimeChannel(store, "speed", "rpm");   // {1, 2, 3}
        addStaleDouble(store);                   // {99, 99, 99}
        apply(plot, layout(), FI::Recalculate);

        auto dv = store.get("double")->readAsDouble();
        ASSERT_EQ(dv.size(), 3u);
        EXPECT_DOUBLE_EQ(dv[0], 2.0);
        EXPECT_DOUBLE_EQ(dv[2], 6.0);
        ASSERT_TRUE(store.contains("triple"));
        auto tv = store.get("triple")->readAsDouble();
        ASSERT_EQ(tv.size(), 3u);
        EXPECT_DOUBLE_EQ(tv[0], 3.0);
        EXPECT_DOUBLE_EQ(tv[2], 9.0);
    }
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

// A sliced channel stays at its true position on the time axis no matter
// what else is visible. (The reported bug: with the source hidden, the slice
// became the earliest visible channel, redefined the axis origin, and slid
// to 0. It now carries its source's origin and the axis anchors on it.)
TEST_F(AnalyserPlotLayoutTest, SlicedChannelKeepsTruePositionWhenShownAlone) {
    SignalStore store;
    scope::analyser::FormulaEngine engine(store);
    AnalyserPlot plot(store, engine);

    // 0 … 2.5 s at 4 Hz starting at 4 s — every x value exactly representable.
    std::vector<double> vs(11);
    for (int i = 0; i < 11; ++i) vs[i] = i;
    addChannelAt(store, "A", Signal::Domain::Time,
                 4'000'000'000LL, 250'000'000LL, vs);
    QString err;
    ASSERT_TRUE(engine.evaluate("S = Slice(A, 0.25, 0.75)", &err))
        << err.toStdString();

    auto* gs = qobject_cast<QCPGraph*>(plottableFor(plot, "S"));
    ASSERT_NE(gs, nullptr);
    ASSERT_EQ(gs->data()->size(), 3);
    EXPECT_DOUBLE_EQ(gs->data()->at(0)->key, 0.25);   // with the source visible

    setRowVisible(plot, "A", false);                   // slice shown alone
    gs = qobject_cast<QCPGraph*>(plottableFor(plot, "S"));
    ASSERT_NE(gs, nullptr);
    ASSERT_EQ(gs->data()->size(), 3);
    EXPECT_DOUBLE_EQ(gs->data()->at(0)->key, 0.25)
        << "slice must not slide to 0 when its source is hidden";
    EXPECT_DOUBLE_EQ(gs->data()->at(2)->key, 0.75);

    setRowVisible(plot, "A", true);                    // and back
    gs = qobject_cast<QCPGraph*>(plottableFor(plot, "S"));
    ASSERT_NE(gs, nullptr);
    EXPECT_DOUBLE_EQ(gs->data()->at(0)->key, 0.25);
}

// Toggling a channel's visibility must not re-colour the other traces —
// colours are reserved per table row (per axis), not per visible subset.
TEST_F(AnalyserPlotLayoutTest, ChannelColorsStableWhenTogglingVisibility) {
    SignalStore store;
    scope::analyser::FormulaEngine engine(store);
    AnalyserPlot plot(store, engine);

    addChannelAt(store, "A", Signal::Domain::Time, 0, 1'000'000LL, {1, 2, 3});
    addChannelAt(store, "B", Signal::Domain::Time, 0, 1'000'000LL, {4, 5, 6});
    addChannelAt(store, "C", Signal::Domain::Time, 0, 1'000'000LL, {7, 8, 9});

    auto colorOf = [&](const char* n) {
        auto* g = qobject_cast<QCPGraph*>(plottableFor(plot, n));
        return g ? g->pen().color() : QColor();
    };
    const QColor a0 = colorOf("A"), b0 = colorOf("B"), c0 = colorOf("C");
    EXPECT_NE(b0, c0);                        // distinct to begin with

    setRowVisible(plot, "A", false);          // hide the first channel
    EXPECT_EQ(colorOf("B"), b0) << "B re-coloured by hiding A";
    EXPECT_EQ(colorOf("C"), c0) << "C re-coloured by hiding A";

    setRowVisible(plot, "A", true);           // and back
    EXPECT_EQ(colorOf("A"), a0);
    EXPECT_EQ(colorOf("B"), b0);
    EXPECT_EQ(colorOf("C"), c0);
}

// The interactive-HTML exporter receives exactly what's on screen: axes
// (label + side), channel→axis assignments, and visibility.
TEST_F(AnalyserPlotLayoutTest, HtmlExportViewMirrorsTheScreen) {
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
    setRowVisible(plot, "speed", false);

    const auto v = htmlView(plot);
    ASSERT_EQ(v.time.axes.size(), 2);
    EXPECT_EQ(v.time.axes[0].label, "rpm");
    EXPECT_FALSE(v.time.axes[0].right);
    EXPECT_EQ(v.time.axes[1].label, "Nm");
    EXPECT_TRUE(v.time.axes[1].right);

    ASSERT_EQ(v.time.channels.size(), 2);
    int iSpeed = v.time.channels[0].name == "speed" ? 0 : 1;
    const auto& sp = v.time.channels[iSpeed];
    const auto& tq = v.time.channels[1 - iSpeed];
    EXPECT_EQ(sp.axisIndex, 0);
    EXPECT_FALSE(sp.visible) << "hidden channel must export unchecked";
    EXPECT_EQ(tq.axisIndex, 1);
    EXPECT_TRUE(tq.visible);
    // Colours follow the app's light-palette derivation per axis.
    EXPECT_EQ(sp.color,
              scope::plot::ScopePlot::deriveChannelColorFor(false, 0, 0).name());
    EXPECT_EQ(tq.color,
              scope::plot::ScopePlot::deriveChannelColorFor(false, 1, 0).name());
    EXPECT_EQ(v.initialView, "time");

    // Switch the app to XY — the export opens there too, with the X channel.
    setViewIndex(plot, 2);
    setXyXChannel(plot, "speed");
    const auto v2 = htmlView(plot);
    EXPECT_EQ(v2.initialView, "xy");
    EXPECT_EQ(v2.xyChannel, "speed");
}

// An undo snapshot taken before a delete restores the channel (and its data)
// when re-applied — the core of the Analyser's undo.
TEST_F(AnalyserPlotLayoutTest, UndoSnapshotRestoresDeletedChannels) {
    SignalStore store;
    scope::analyser::FormulaEngine engine(store);
    AnalyserPlot plot(store, engine);

    addTimeChannel(store, "A", "V");
    addTimeChannel(store, "B", "A");
    const auto snap = captureUndo(plot);

    store.remove("B");
    ASSERT_FALSE(store.contains("B"));

    restoreUndo(plot, snap);
    EXPECT_TRUE(store.contains("A"));
    ASSERT_TRUE(store.contains("B"));
    // Data came back intact (the snapshot held the shared_ptr, no re-read).
    EXPECT_EQ(store.get("B")->snapshotForRead().count, 3u);
}

// Redo direction: a snapshot taken after an add re-creates a channel that was
// removed since.
TEST_F(AnalyserPlotLayoutTest, UndoSnapshotReappliesAddedChannel) {
    SignalStore store;
    scope::analyser::FormulaEngine engine(store);
    AnalyserPlot plot(store, engine);

    addTimeChannel(store, "A", "V");
    addTimeChannel(store, "B", "A");
    const auto withBoth = captureUndo(plot);   // the "redo" target

    // Step back to a single-channel world, then re-apply the two-channel snap.
    store.remove("B");
    restoreUndo(plot, withBoth);
    EXPECT_TRUE(store.contains("A"));
    EXPECT_TRUE(store.contains("B"));
}

namespace scope::analyser::ui {

// ---------------------------------------------------------------------------
// Channel rename.
//
// Before this there was no rename anywhere in the stack — only a formula
// channel *redefine* (AddChannelDialog re-runs "<name> = <expr>" and drops the
// old key), which a plain recorded or imported channel cannot express. The
// pencil button dead-ended on "wasn't created with a formula".
//
// A rename has to move the store key, the engine's formula key, every formula
// that REFERENCES the channel, and this widget's own name-keyed state — all
// without looking like a remove to the listeners that tear rows down.
// ---------------------------------------------------------------------------

class AnalyserPlotRenameTest : public ::testing::Test {
protected:
    void SetUp() override { ensureApp(); }
    bool rename(AnalyserPlot& p, const QString& from, const QString& to,
                QString* err = nullptr) {
        QString sink;
        return p.renameChannel(from, to, err ? err : &sink);
    }
};

TEST_F(AnalyserPlotRenameTest, RenamesAPlainChannelKeepingItsSamples) {
    SignalStore store;
    addTimeChannel(store, "raw", "V");
    FormulaEngine engine(store);
    AnalyserPlot plot(store, engine);

    ASSERT_TRUE(rename(plot, "raw", "pressure"));
    EXPECT_FALSE(store.contains("raw"));
    ASSERT_TRUE(store.contains("pressure"));

    auto s = store.get("pressure");
    ASSERT_TRUE(s != nullptr);
    EXPECT_EQ(s->meta().name.toStdString(), "pressure");
    EXPECT_EQ(s->meta().unit.toStdString(), "V") << "unit must survive";
    ASSERT_EQ(s->sampleCount(), 3u) << "samples must survive";
    EXPECT_DOUBLE_EQ(s->readAsDouble()[1], 2.0);
}

TEST_F(AnalyserPlotRenameTest, AnyNameIsAllowedIncludingSpacesAndSymbols) {
    SignalStore store;
    addTimeChannel(store, "raw", "V");
    FormulaEngine engine(store);
    AnalyserPlot plot(store, engine);

    // Formulas reference awkward names in brackets, so nothing needs to be
    // restricted to plain identifiers.
    ASSERT_TRUE(rename(plot, "raw", "Axis position (mm)"));
    EXPECT_TRUE(store.contains("Axis position (mm)"));
}

TEST_F(AnalyserPlotRenameTest, RefusesToClobberAnExistingChannel) {
    SignalStore store;
    addTimeChannel(store, "a", "V");
    addTimeChannel(store, "b", "V");
    FormulaEngine engine(store);
    AnalyserPlot plot(store, engine);

    QString err;
    EXPECT_FALSE(rename(plot, "a", "b", &err));
    EXPECT_FALSE(err.isEmpty());
    EXPECT_TRUE(store.contains("a")) << "the source must be left alone";
    EXPECT_TRUE(store.contains("b"));
    EXPECT_DOUBLE_EQ(store.get("b")->readAsDouble()[1], 2.0);
}

TEST_F(AnalyserPlotRenameTest, RewritesFormulasThatReferenceTheChannel) {
    SignalStore store;
    addTimeChannel(store, "speed", "rpm");
    FormulaEngine engine(store);
    AnalyserPlot plot(store, engine);

    QString err;
    ASSERT_TRUE(engine.evaluate("doubled = speed * 2", &err)) << err.toStdString();
    ASSERT_TRUE(rename(plot, "speed", "velocity"));

    // The dependent formula now names the new channel and still recomputes.
    EXPECT_EQ(engine.formulaFor("doubled").toStdString(), "velocity * 2");
    QStringList errors;
    engine.recomputeAll(&errors);
    EXPECT_TRUE(errors.isEmpty()) << errors.join("; ").toStdString();
    ASSERT_TRUE(store.contains("doubled"));
    EXPECT_DOUBLE_EQ(store.get("doubled")->readAsDouble()[1], 4.0);
}

TEST_F(AnalyserPlotRenameTest, RewriteIsWholeTokenNotSubstring) {
    SignalStore store;
    addTimeChannel(store, "speed", "rpm");
    addTimeChannel(store, "speedy", "rpm");
    FormulaEngine engine(store);
    AnalyserPlot plot(store, engine);

    QString err;
    ASSERT_TRUE(engine.evaluate("sum = speed + speedy", &err)) << err.toStdString();
    ASSERT_TRUE(rename(plot, "speed", "v"));
    EXPECT_EQ(engine.formulaFor("sum").toStdString(), "v + speedy")
        << "renaming 'speed' must not touch 'speedy'";
}

TEST_F(AnalyserPlotRenameTest, ReferenceIsRebracketedWhenTheNewNameNeedsIt) {
    SignalStore store;
    addTimeChannel(store, "speed", "rpm");
    FormulaEngine engine(store);
    AnalyserPlot plot(store, engine);

    QString err;
    ASSERT_TRUE(engine.evaluate("doubled = speed * 2", &err)) << err.toStdString();
    ASSERT_TRUE(rename(plot, "speed", "Axis speed"));

    EXPECT_EQ(engine.formulaFor("doubled").toStdString(), "[Axis speed] * 2")
        << "a name with a space only parses bracketed";
    QStringList errors;
    engine.recomputeAll(&errors);
    EXPECT_TRUE(errors.isEmpty()) << errors.join("; ").toStdString();
}

TEST_F(AnalyserPlotRenameTest, RenamingAFormulaChannelMovesItsOwnFormula) {
    SignalStore store;
    addTimeChannel(store, "src", "V");
    FormulaEngine engine(store);
    AnalyserPlot plot(store, engine);

    QString err;
    ASSERT_TRUE(engine.evaluate("derived = src * 3", &err)) << err.toStdString();
    ASSERT_TRUE(rename(plot, "derived", "scaled"));

    EXPECT_TRUE(engine.formulaFor("derived").isEmpty());
    EXPECT_EQ(engine.formulaFor("scaled").toStdString(), "src * 3");
    QStringList errors;
    engine.recomputeAll(&errors);
    EXPECT_TRUE(errors.isEmpty()) << errors.join("; ").toStdString();
    ASSERT_TRUE(store.contains("scaled"));
    EXPECT_DOUBLE_EQ(store.get("scaled")->readAsDouble()[1], 6.0);
}

TEST_F(AnalyserPlotRenameTest, DependentsOfNamesTheFormulasThatWouldBreak) {
    SignalStore store;
    addTimeChannel(store, "a", "V");
    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("b = a * 2", &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("c = b + 1", &err)) << err.toStdString();

    EXPECT_EQ(engine.dependentsOf("a"), QStringList{"b"});
    EXPECT_EQ(engine.dependentsOf("b"), QStringList{"c"});
    EXPECT_TRUE(engine.dependentsOf("c").isEmpty());
}

}  // namespace scope::analyser::ui
