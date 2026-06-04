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

    EXPECT_NEAR(plot->xAxis->range().lower, 0.0, 1e-9);
    EXPECT_NEAR(plot->xAxis->range().upper, 3.0, 1e-9);
    EXPECT_NEAR(plot->yAxis->range().lower, -1.0, 1e-9);
    EXPECT_NEAR(plot->yAxis->range().upper,  2.0, 1e-9);
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
