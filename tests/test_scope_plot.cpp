#include "scope/plot/ScopePlot.h"

#include <qcustomplot.h>

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QSignalSpy>

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
