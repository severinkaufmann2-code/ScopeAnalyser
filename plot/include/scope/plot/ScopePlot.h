#pragma once

#include <QList>
#include <QWidget>

#include <memory>

class QCustomPlot;
class QCPAxis;
class QCPGraph;

namespace scope::plot {

// A QCustomPlot wrapped with a toolbar (Fit / X± / Y± / Y+ / PNG / Pause)
// and modifier-aware wheel zoom + region-zoom + crosshair, supporting an
// arbitrary number of Y axes that channels can be individually assigned to.
//
// Wheel convention:
//   Scroll          → zoom X and ALL Y axes (centred on cursor)
//   Ctrl+Scroll     → zoom X only
//   Shift+Scroll    → zoom the Y axis nearest the cursor
//
// Ctrl+LeftDrag → region zoom (rectangle).
// Plain LeftDrag → pan (QCustomPlot iRangeDrag).
//
// Right-click a Y-axis label → rename / set range / auto-scale / remove.
//
// Keyboard: Home (fit), +/- (zoom both), arrow keys (pan).
class ScopePlot : public QWidget {
    Q_OBJECT
public:
    explicit ScopePlot(QWidget* parent = nullptr);
    ~ScopePlot();

    QCustomPlot* plot() const;

    // Pause support (Recorder uses it; Analyser doesn't). Default: hidden.
    void setPauseSupported(bool enabled);
    bool isPaused() const;
    void setPaused(bool paused);

    // ---- Multi-Y-axis API --------------------------------------------
    // Index 0 is always the original Y axis (plot()->yAxis). Additional
    // axes are appended via addYAxis() and live on the chosen side. New
    // axes alternate left/right unless an explicit side is given.
    int  yAxisCount() const;
    QCPAxis* yAxis(int index) const;
    int  addYAxis(const QString& label = QString(),
                  Qt::Alignment side = Qt::Alignment());  // empty = alternate
    bool removeYAxis(int index, QString* errOut = nullptr);
    void setGraphYAxis(QCPGraph* graph, int axisIndex);

    // 0 if not bound to any of our tracked axes.
    int  graphYAxisIndex(QCPGraph* graph) const;

    // Pick the Y axis whose label area is closest to a pixel position.
    int  closestYAxisToPos(QPointF pixelPos) const;

    // Re-scale every Y axis independently to the data of its assigned graphs.
    void rescaleAllYAxes();

    // Zoom helpers (multiply axis range; <1 = zoom in)
    void zoomXBy(double factor);
    void zoomYBy(double factor, int yAxisIndex = -1);   // -1 = all Y axes
    void zoomBothBy(double factor);

public slots:
    void fitAll();
    void savePngDialog();
    void togglePause();

signals:
    void pausedChanged(bool paused);
    void yAxesChanged();     // emitted when axes are added/removed/renamed

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    void zoomAt(QPointF mousePx, double factorX, double factorY, int yAxisIndex);
    void panBy(double fracX, double fracY);
    void updateCrosshair(QPointF mousePx);
    void hideCrosshair();
    void beginRegionZoom(QPointF startPx);
    void updateRegionZoom(QPointF curPx);
    void endRegionZoom(QPointF endPx);
    void showAxisContextMenu(int axisIndex, QPoint globalPos);
};

}  // namespace scope::plot
