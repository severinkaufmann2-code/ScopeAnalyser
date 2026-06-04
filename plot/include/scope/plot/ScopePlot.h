#pragma once

#include <QWidget>

#include <memory>

class QCustomPlot;

namespace scope::plot {

// A QCustomPlot wrapped with a toolbar (Fit / X± / Y± / PNG / Pause) and
// modifier-aware wheel zoom + region-zoom + crosshair. Used by both the
// Recorder's LivePreviewPlot and the Analyser's plot panel.
//
// Wheel zoom convention (matches TwinCAT Scope, Audacity, LabVIEW):
//   Scroll          → zoom both axes, centred on the mouse cursor
//   Ctrl+Scroll     → zoom X axis only
//   Shift+Scroll    → zoom Y axis only
//
// Ctrl+LeftDrag draws a rectangle and zooms to that region on release.
// Plain LeftDrag still pans (QCustomPlot's iRangeDrag behaviour).
//
// Keyboard:
//   Home  → Fit all
//   + / - → Zoom both in / out
//   Arrows → Pan
class ScopePlot : public QWidget {
    Q_OBJECT
public:
    explicit ScopePlot(QWidget* parent = nullptr);
    ~ScopePlot();

    // Underlying QCustomPlot. Callers add their graphs / set labels here.
    QCustomPlot* plot() const;

    // Show or hide the Pause/Resume button (Recorder uses it; Analyser
    // doesn't). Default: hidden.
    void setPauseSupported(bool enabled);
    bool isPaused() const;
    void setPaused(bool paused);

    // Zoom factor < 1 = zoom in, > 1 = zoom out (multiplies the axis range).
    void zoomXBy(double factor);
    void zoomYBy(double factor);
    void zoomBothBy(double factor);

public slots:
    void fitAll();         // QCP::rescaleAxes(); replot.
    void savePngDialog();  // prompt for filename, write PNG.
    void togglePause();

signals:
    void pausedChanged(bool paused);

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    void zoomAt(QPointF mouseInPlot, double factorX, double factorY);
    void panBy(double fracX, double fracY);
    void updateCrosshair(QPointF mousePx);
    void hideCrosshair();
    void beginRegionZoom(QPointF startPx);
    void updateRegionZoom(QPointF curPx);
    void endRegionZoom(QPointF endPx);
};

}  // namespace scope::plot
