#include "scope/plot/ScopePlot.h"
#include "scope/plot/PlotLayout.h"

#include <qcustomplot.h>

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QSignalSpy>

#include <filesystem>
#include <memory>

namespace {
struct GuiAppFixture {
    GuiAppFixture() {
        if (!QCoreApplication::instance()) {
            qputenv("QT_QPA_PLATFORM", "offscreen");
            static int argc = 1;
            static char a0[] = {'t', '\0'};
            static char* argv[] = {a0, nullptr};
            // Need QGuiApplication for QWidget; create a QApplication when we
            // have access. In tests run via QT_QPA_PLATFORM=offscreen, this
            // works without a display.
            app.reset(new QApplication(argc, argv));
        }
    }
    std::unique_ptr<QApplication> app;
};
}

#include <QApplication>

TEST(ScopePlot, FitAllMatchesGraphDataRange) {
    GuiAppFixture fixture;

    scope::plot::ScopePlot sp;
    auto* plot = sp.plot();
    auto* g = plot->addGraph();
    QVector<double> xs = {0.0, 1.0, 2.0, 3.0};
    QVector<double> ys = {-1.0, 0.0, 1.0, 2.0};
    g->setData(xs, ys, true);

    sp.fitAll();

    // X axis: exact rescale to data.
    EXPECT_NEAR(plot->xAxis->range().lower, 0.0, 1e-9);
    EXPECT_NEAR(plot->xAxis->range().upper, 3.0, 1e-9);
    // Y axis: rescaleAllYAxes() adds a 5% margin so edges aren't clipped.
    // For values [-1, 2] (range 3), margin is 0.15 → [-1.15, 2.15].
    EXPECT_NEAR(plot->yAxis->range().lower, -1.15, 1e-6);
    EXPECT_NEAR(plot->yAxis->range().upper,  2.15, 1e-6);
}

// rescaleYAxesToWindow: should fit Y to ONLY the samples that fall in
// the given X window, not the whole signal's value range.
TEST(ScopePlot, RescaleYAxesToWindowConsidersOnlySamplesInWindow) {
    GuiAppFixture fixture;
    scope::plot::ScopePlot sp;
    auto* plot = sp.plot();
    auto* g = plot->addGraph();
    // Values jump from a small range to a big one outside the window.
    QVector<double> xs = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    QVector<double> ys = {1, 2, 3, 4, 5, 6, 100, 200, 300, 400, 500};
    g->setData(xs, ys, true);

    // Window [2, 4] covers ys = {3, 4, 5}. Min=3, max=5, with 5% margin.
    sp.rescaleYAxesToWindow(2.0, 4.0);

    const double margin = 0.05 * (5.0 - 3.0);
    EXPECT_NEAR(plot->yAxis->range().lower, 3.0 - margin, 1e-6);
    EXPECT_NEAR(plot->yAxis->range().upper, 5.0 + margin, 1e-6);
}

// Toggling a graph's visibility off shouldn't move the X axis if the
// host calls rescaleAllYAxes (i.e. AnalyserPlot's new redraw path).
TEST(ScopePlot, RescaleAllYAxesDoesNotTouchXAxis) {
    GuiAppFixture fixture;
    scope::plot::ScopePlot sp;
    auto* plot = sp.plot();
    auto* g = plot->addGraph();
    g->setData(QVector<double>{0, 1, 2, 3},
               QVector<double>{0, 1, 2, 3}, true);

    plot->xAxis->setRange(0.4, 0.7);  // simulate user zoom
    const auto before = plot->xAxis->range();
    sp.rescaleAllYAxes();
    const auto after  = plot->xAxis->range();
    EXPECT_NEAR(after.lower, before.lower, 1e-12);
    EXPECT_NEAR(after.upper, before.upper, 1e-12);
}

TEST(ScopePlot, AddYAxisReturnsIncreasingIndex) {
    GuiAppFixture fixture;

    scope::plot::ScopePlot sp;
    EXPECT_EQ(sp.yAxisCount(), 1);
    const int i1 = sp.addYAxis();
    const int i2 = sp.addYAxis();
    const int i3 = sp.addYAxis();
    EXPECT_EQ(i1, 1);
    EXPECT_EQ(i2, 2);
    EXPECT_EQ(i3, 3);
    EXPECT_EQ(sp.yAxisCount(), 4);
}

TEST(ScopePlot, SetGraphYAxisMovesGraph) {
    GuiAppFixture fixture;

    scope::plot::ScopePlot sp;
    const int newIdx = sp.addYAxis("Custom");
    auto* g = sp.plot()->addGraph();
    QVector<double> xs = {0, 1, 2, 3};
    QVector<double> ys = {10, 20, 30, 40};
    g->setData(xs, ys, true);

    EXPECT_EQ(sp.graphYAxisIndex(g), 0);
    sp.setGraphYAxis(g, newIdx);
    EXPECT_EQ(sp.graphYAxisIndex(g), newIdx);
    EXPECT_EQ(g->valueAxis(), sp.yAxis(newIdx));
}

TEST(ScopePlot, RemoveYAxisRefusesWhenInUse) {
    GuiAppFixture fixture;

    scope::plot::ScopePlot sp;
    const int idx = sp.addYAxis();
    auto* g = sp.plot()->addGraph();
    QVector<double> xs = {0, 1}, ys = {0, 1};
    g->setData(xs, ys, true);
    sp.setGraphYAxis(g, idx);

    QString err;
    EXPECT_FALSE(sp.removeYAxis(idx, &err));
    EXPECT_TRUE(err.contains("in use"));
    EXPECT_EQ(sp.yAxisCount(), 2);

    // Move the graph back to Y1 and remove succeeds.
    sp.setGraphYAxis(g, 0);
    EXPECT_TRUE(sp.removeYAxis(idx));
    EXPECT_EQ(sp.yAxisCount(), 1);
}

TEST(ScopePlot, RemoveYAxisRefusesPrimary) {
    GuiAppFixture fixture;

    scope::plot::ScopePlot sp;
    QString err;
    EXPECT_FALSE(sp.removeYAxis(0, &err));
    EXPECT_TRUE(err.contains("primary"));
}

TEST(ScopePlot, DeriveChannelColorVariesPerIndex) {
    GuiAppFixture fixture;

    scope::plot::ScopePlot sp;
    const auto base = sp.axisBaseColor(0);
    const auto c0   = sp.deriveChannelColor(0, 0);
    const auto c1   = sp.deriveChannelColor(0, 1);
    const auto c2   = sp.deriveChannelColor(0, 2);
    // First channel matches the axis base colour exactly.
    EXPECT_EQ(c0, base);
    // Subsequent channels differ from the base AND from each other.
    EXPECT_NE(c1, base);
    EXPECT_NE(c2, base);
    EXPECT_NE(c1, c2);
}

TEST(ScopePlot, ShiftWheelInPlotCenterZoomsAllYAxes) {
    // Hovering anywhere INSIDE the plot rect with Shift held should zoom
    // every Y axis at once (don't force the user to pick one). Hovering
    // outside the plot rect — over an axis label area — still picks just
    // that axis.
    GuiAppFixture fixture;

    scope::plot::ScopePlot sp;
    sp.resize(800, 600);
    sp.show();
    QApplication::processEvents();
    const int rightIdx = sp.addYAxis("R", Qt::AlignRight);
    QApplication::processEvents();

    auto* plot = sp.plot();
    plot->yAxis->setRange(0, 100);
    sp.yAxis(rightIdx)->setRange(0, 1000);

    const QRect ar = plot->axisRect()->rect();
    const QPointF localPos(ar.center());
    QWheelEvent ev(localPos,
                   plot->mapToGlobal(localPos.toPoint()),
                   QPoint(),
                   QPoint(0, 120),
                   Qt::NoButton,
                   Qt::ShiftModifier,
                   Qt::NoScrollPhase,
                   false);
    QApplication::sendEvent(plot, &ev);
    QApplication::processEvents();

    // BOTH axes should have shrunk.
    EXPECT_LT(plot->yAxis->range().size(), 100.0);
    EXPECT_LT(sp.yAxis(rightIdx)->range().size(), 1000.0);
}

TEST(ScopePlot, AltWheelZoomsClosestYAxis) {
    GuiAppFixture fixture;

    scope::plot::ScopePlot sp;
    sp.resize(800, 600);
    sp.show();
    QApplication::processEvents();
    const int rightIdx = sp.addYAxis("R", Qt::AlignRight);
    QApplication::processEvents();

    auto* plot = sp.plot();
    plot->yAxis->setRange(0, 100);
    sp.yAxis(rightIdx)->setRange(0, 1000);

    const QRect ar = plot->axisRect()->rect();
    const QPointF localPos(ar.right() + 5, ar.center().y());
    QWheelEvent ev(localPos,
                   plot->mapToGlobal(localPos.toPoint()),
                   QPoint(),
                   QPoint(0, 120),
                   Qt::NoButton,
                   Qt::AltModifier,         // <-- Alt, not Shift
                   Qt::NoScrollPhase,
                   false);
    QApplication::sendEvent(plot, &ev);
    QApplication::processEvents();

    EXPECT_LT(sp.yAxis(rightIdx)->range().size(), 1000.0);
    EXPECT_DOUBLE_EQ(plot->yAxis->range().size(), 100.0);
}

TEST(ScopePlot, NoModifierWheelOverYAxisAreaZoomsThatAxis) {
    GuiAppFixture fixture;

    scope::plot::ScopePlot sp;
    sp.resize(800, 600);
    sp.show();
    QApplication::processEvents();
    const int rightIdx = sp.addYAxis("R", Qt::AlignRight);
    QApplication::processEvents();

    auto* plot = sp.plot();
    plot->xAxis->setRange(0, 10);
    plot->yAxis->setRange(0, 100);
    sp.yAxis(rightIdx)->setRange(0, 1000);

    const QRect ar = plot->axisRect()->rect();
    // Hover OVER the right-axis label area (outside the inner plot rect)
    // with NO modifier — should zoom only Y2, not X, not Y1.
    const QPointF localPos(ar.right() + 5, ar.center().y());
    QWheelEvent ev(localPos,
                   plot->mapToGlobal(localPos.toPoint()),
                   QPoint(),
                   QPoint(0, 120),
                   Qt::NoButton,
                   Qt::NoModifier,
                   Qt::NoScrollPhase,
                   false);
    QApplication::sendEvent(plot, &ev);
    QApplication::processEvents();

    EXPECT_LT(sp.yAxis(rightIdx)->range().size(), 1000.0);
    EXPECT_DOUBLE_EQ(plot->yAxis->range().size(), 100.0);
    EXPECT_DOUBLE_EQ(plot->xAxis->range().size(), 10.0);
}

TEST(ScopePlot, NoModifierWheelOverPlotCenterZoomsBoth) {
    GuiAppFixture fixture;

    scope::plot::ScopePlot sp;
    sp.resize(800, 600);
    sp.show();
    QApplication::processEvents();
    auto* plot = sp.plot();
    plot->xAxis->setRange(0, 10);
    plot->yAxis->setRange(0, 100);

    const QRect ar = plot->axisRect()->rect();
    const QPointF localPos(ar.center());
    QWheelEvent ev(localPos,
                   plot->mapToGlobal(localPos.toPoint()),
                   QPoint(),
                   QPoint(0, 120),
                   Qt::NoButton,
                   Qt::NoModifier,
                   Qt::NoScrollPhase,
                   false);
    QApplication::sendEvent(plot, &ev);
    QApplication::processEvents();

    EXPECT_LT(plot->xAxis->range().size(), 10.0);
    EXPECT_LT(plot->yAxis->range().size(), 100.0);
}

TEST(ScopePlot, ShiftWheelDisablesAutoFitOnTargetAxis) {
    GuiAppFixture fixture;

    scope::plot::ScopePlot sp;
    sp.resize(800, 600);
    sp.show();
    QApplication::processEvents();
    const int rightIdx = sp.addYAxis("R", Qt::AlignRight);

    // Add data so rescaleAllYAxes has something to scale to.
    auto* plot = sp.plot();
    auto* g = plot->addGraph(plot->xAxis, sp.yAxis(rightIdx));
    QVector<double> xs = {0, 1, 2}, ys = {0, 5, 10};
    g->setData(xs, ys, true);

    // First fit puts Y2 at ~[0, 10] (with margin).
    sp.fitAll();
    EXPECT_NEAR(sp.yAxis(rightIdx)->range().lower, -0.5, 1e-6);
    EXPECT_NEAR(sp.yAxis(rightIdx)->range().upper, 10.5, 1e-6);

    // User Shift+Scrolls Y2 — should mark it manual.
    sp.zoomYBy(0.5, rightIdx);
    const auto rangeAfterUser = sp.yAxis(rightIdx)->range();

    // Now simulate a "tick" calling rescaleAllYAxes: it should NOT rescale
    // Y2 anymore because the user touched it.
    sp.rescaleAllYAxes();
    EXPECT_EQ(sp.yAxis(rightIdx)->range().lower, rangeAfterUser.lower);
    EXPECT_EQ(sp.yAxis(rightIdx)->range().upper, rangeAfterUser.upper);

    // fitAll re-arms auto-fit and rescales.
    sp.fitAll();
    EXPECT_NEAR(sp.yAxis(rightIdx)->range().lower, -0.5, 1e-6);
    EXPECT_NEAR(sp.yAxis(rightIdx)->range().upper, 10.5, 1e-6);
}

TEST(ScopePlot, ShiftWheelWithX11HorizontalDeltaStillZoomsY) {
    // On X11, Shift+Wheel swaps the delta from y() to x() — the
    // horizontal-scroll convention. ScopePlot must treat that the same as
    // a y-delta otherwise Shift+Scroll silently does nothing on Linux.
    GuiAppFixture fixture;
    scope::plot::ScopePlot sp;
    sp.resize(800, 600);
    sp.show();
    QApplication::processEvents();
    const int rightIdx = sp.addYAxis("R", Qt::AlignRight);
    QApplication::processEvents();

    auto* plot = sp.plot();
    plot->yAxis->setRange(0, 100);
    sp.yAxis(rightIdx)->setRange(0, 1000);

    const QRect ar = plot->axisRect()->rect();
    const QPointF localPos(ar.right() + 5, ar.center().y());
    // X-axis delta (NOT y), Shift held. This is what X11 delivers.
    QWheelEvent ev(localPos,
                   plot->mapToGlobal(localPos.toPoint()),
                   QPoint(),
                   QPoint(120, 0),
                   Qt::NoButton,
                   Qt::ShiftModifier,
                   Qt::NoScrollPhase,
                   false);
    QApplication::sendEvent(plot, &ev);
    QApplication::processEvents();

    // Y2 should have shrunk.
    EXPECT_LT(sp.yAxis(rightIdx)->range().size(), 1000.0);
    EXPECT_DOUBLE_EQ(plot->yAxis->range().size(), 100.0);
}

TEST(ScopePlot, ShiftWheelOverRightAxisZoomsOnlyRightAxis) {
    GuiAppFixture fixture;

    scope::plot::ScopePlot sp;
    sp.resize(800, 600);
    sp.show();
    QApplication::processEvents();

    const int rightIdx = sp.addYAxis("R", Qt::AlignRight);
    QApplication::processEvents();

    auto* plot = sp.plot();
    plot->yAxis->setRange(0, 100);
    sp.yAxis(rightIdx)->setRange(0, 1000);

    const QRect ar = plot->axisRect()->rect();

    // Synthesise a Shift+Scroll over the right axis label area.
    const QPointF localPos(ar.right() + 5, ar.center().y());
    QWheelEvent ev(localPos,
                   plot->mapToGlobal(localPos.toPoint()),
                   QPoint(),
                   QPoint(0, 120),               // one notch up
                   Qt::NoButton,
                   Qt::ShiftModifier,
                   Qt::NoScrollPhase,
                   false);
    QApplication::sendEvent(plot, &ev);
    QApplication::processEvents();

    // Y2 (rightIdx) should have shrunk; Y1 should be unchanged.
    EXPECT_LT(sp.yAxis(rightIdx)->range().size(), 1000.0);
    EXPECT_DOUBLE_EQ(plot->yAxis->range().size(), 100.0);
}

TEST(ScopePlot, ShiftWheelOverLeftAxisZoomsOnlyLeftAxis) {
    GuiAppFixture fixture;

    scope::plot::ScopePlot sp;
    sp.resize(800, 600);
    sp.show();
    QApplication::processEvents();

    const int rightIdx = sp.addYAxis("R", Qt::AlignRight);
    QApplication::processEvents();

    auto* plot = sp.plot();
    plot->yAxis->setRange(0, 100);
    sp.yAxis(rightIdx)->setRange(0, 1000);

    const QRect ar = plot->axisRect()->rect();

    // Shift+Scroll over the LEFT axis area.
    const QPointF localPos(ar.left() - 5, ar.center().y());
    QWheelEvent ev(localPos,
                   plot->mapToGlobal(localPos.toPoint()),
                   QPoint(),
                   QPoint(0, 120),
                   Qt::NoButton,
                   Qt::ShiftModifier,
                   Qt::NoScrollPhase,
                   false);
    QApplication::sendEvent(plot, &ev);
    QApplication::processEvents();

    EXPECT_LT(plot->yAxis->range().size(), 100.0);
    EXPECT_DOUBLE_EQ(sp.yAxis(rightIdx)->range().size(), 1000.0);
}

TEST(ScopePlot, ClosestYAxisToPosByPixelDistance) {
    GuiAppFixture fixture;

    scope::plot::ScopePlot sp;
    sp.resize(800, 600);
    sp.show();
    QApplication::processEvents();

    // Add a right-side axis (index 1).
    const int rightIdx = sp.addYAxis(QString(), Qt::AlignRight);
    QApplication::processEvents();

    auto* plot = sp.plot();
    const QRect ar = plot->axisRect()->rect();

    // Mouse far left → Y1 (index 0).
    EXPECT_EQ(sp.closestYAxisToPos(QPointF(ar.left() - 5, ar.center().y())), 0);
    // Mouse far right → the right-side axis we just added.
    EXPECT_EQ(sp.closestYAxisToPos(QPointF(ar.right() + 5, ar.center().y())),
              rightIdx);
}

TEST(ScopePlot, LayoutFileRoundTrip) {
    auto path = std::filesystem::temp_directory_path() / "scope_layout.scolayout";

    scope::plot::PlotLayout layout;
    layout.axes.append({"Volts", "left",  true, -10.0, 10.0});
    layout.axes.append({"rpm",   "right", true,   0.0, 5000.0});
    layout.channels.append({"speed",  0});
    layout.channels.append({"torque", 1});
    layout.channels.append({"temp",   0});

    QString err;
    ASSERT_TRUE(layout.saveToFile(path, &err)) << err.toStdString();

    auto loaded = scope::plot::PlotLayout::loadFromFile(path, &err);
    ASSERT_TRUE(err.isEmpty()) << err.toStdString();
    ASSERT_EQ(loaded.axes.size(), 2);
    EXPECT_EQ(loaded.axes[0].label, "Volts");
    EXPECT_EQ(loaded.axes[1].side,  "right");
    EXPECT_DOUBLE_EQ(loaded.axes[1].max, 5000.0);
    ASSERT_EQ(loaded.channels.size(), 3);
    EXPECT_EQ(loaded.channels[1].name, "torque");
    EXPECT_EQ(loaded.channels[1].axisIndex, 1);

    std::filesystem::remove(path);
}

TEST(ScopePlot, LayoutFileRoundTripPreservesMixedDomain) {
    // Regression: Save layout used to only persist channels currently
    // in the table (i.e. the active View domain). After the fix it
    // persists every channel in the store, tagging each with its
    // domain so a reload restores it under the right View.
    auto path = std::filesystem::temp_directory_path() / "scope_mixed_layout.scolayout";

    scope::plot::PlotLayout layout;
    layout.axes.append({"v",   "left",  true, 0.0, 10.0});

    scope::plot::PlotLayoutChannel timeCh;
    timeCh.name = "speed";
    timeCh.axisIndex = 0;
    timeCh.formula = "";
    timeCh.domain = "time";
    layout.channels.append(timeCh);

    scope::plot::PlotLayoutChannel freqCh;
    freqCh.name = "SpeedSpectrum";
    freqCh.axisIndex = 0;
    freqCh.formula = "FFT(speed)";
    freqCh.domain = "frequency";
    layout.channels.append(freqCh);

    QString err;
    ASSERT_TRUE(layout.saveToFile(path, &err)) << err.toStdString();
    auto loaded = scope::plot::PlotLayout::loadFromFile(path, &err);
    ASSERT_TRUE(err.isEmpty()) << err.toStdString();
    ASSERT_EQ(loaded.channels.size(), 2);
    EXPECT_EQ(loaded.channels[0].name,    "speed");
    EXPECT_EQ(loaded.channels[0].domain,  "time");
    EXPECT_EQ(loaded.channels[1].name,    "SpeedSpectrum");
    EXPECT_EQ(loaded.channels[1].domain,  "frequency");
    EXPECT_EQ(loaded.channels[1].formula, "FFT(speed)");
    std::filesystem::remove(path);
}

TEST(ScopePlot, FitAllRescalesEachYAxisIndependently) {
    GuiAppFixture fixture;

    scope::plot::ScopePlot sp;
    const int idx = sp.addYAxis();

    auto* g1 = sp.plot()->addGraph();
    QVector<double> xs1 = {0, 1, 2}, ys1 = {0, 1, 2};
    g1->setData(xs1, ys1, true);
    // g1 stays on Y1 (default)

    auto* g2 = sp.plot()->addGraph();
    QVector<double> xs2 = {0, 1, 2}, ys2 = {100, 200, 300};
    g2->setData(xs2, ys2, true);
    sp.setGraphYAxis(g2, idx);

    sp.fitAll();

    // Y1 covers 0..2 with 5% margin = -0.1 .. 2.1
    EXPECT_NEAR(sp.yAxis(0)->range().lower, -0.1, 1e-6);
    EXPECT_NEAR(sp.yAxis(0)->range().upper,  2.1, 1e-6);
    // Y2 covers 100..300 with 5% margin = 90 .. 310
    EXPECT_NEAR(sp.yAxis(idx)->range().lower,  90.0, 1e-6);
    EXPECT_NEAR(sp.yAxis(idx)->range().upper, 310.0, 1e-6);
}

TEST(ScopePlot, ZoomXByShrinksOnlyX) {
    GuiAppFixture fixture;

    scope::plot::ScopePlot sp;
    auto* plot = sp.plot();
    plot->xAxis->setRange(0.0, 10.0);
    plot->yAxis->setRange(0.0, 20.0);

    sp.zoomXBy(0.5);

    EXPECT_NEAR(plot->xAxis->range().size(), 5.0, 1e-9);  // shrunk
    EXPECT_NEAR(plot->yAxis->range().size(), 20.0, 1e-9); // unchanged
}

TEST(ScopePlot, ZoomYByShrinksOnlyY) {
    GuiAppFixture fixture;

    scope::plot::ScopePlot sp;
    auto* plot = sp.plot();
    plot->xAxis->setRange(0.0, 10.0);
    plot->yAxis->setRange(0.0, 20.0);

    sp.zoomYBy(0.5);

    EXPECT_NEAR(plot->xAxis->range().size(), 10.0, 1e-9);
    EXPECT_NEAR(plot->yAxis->range().size(), 10.0, 1e-9);
}

TEST(ScopePlot, ZoomBothByShrinksBoth) {
    GuiAppFixture fixture;

    scope::plot::ScopePlot sp;
    auto* plot = sp.plot();
    plot->xAxis->setRange(0.0, 10.0);
    plot->yAxis->setRange(0.0, 20.0);

    sp.zoomBothBy(0.5);

    EXPECT_NEAR(plot->xAxis->range().size(), 5.0, 1e-9);
    EXPECT_NEAR(plot->yAxis->range().size(), 10.0, 1e-9);
}

TEST(ScopePlot, PauseToggleEmitsSignal) {
    GuiAppFixture fixture;

    scope::plot::ScopePlot sp;
    sp.setPauseSupported(true);
    QSignalSpy spy(&sp, &scope::plot::ScopePlot::pausedChanged);
    EXPECT_FALSE(sp.isPaused());
    sp.setPaused(true);
    EXPECT_TRUE(sp.isPaused());
    EXPECT_EQ(spy.count(), 1);
    EXPECT_TRUE(spy.first().at(0).toBool());
    sp.setPaused(true);  // no change → no signal
    EXPECT_EQ(spy.count(), 1);
    sp.togglePause();
    EXPECT_FALSE(sp.isPaused());
    EXPECT_EQ(spy.count(), 2);
}
