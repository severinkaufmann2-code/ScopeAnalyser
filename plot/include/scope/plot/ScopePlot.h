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

    // Re-scale every Y axis to the data of its assigned visible graphs,
    // but considering only samples whose X falls within [xMin, xMax].
    // X axis is left untouched. Axes with no visible graphs (or no
    // samples in the window) keep their current range.
    void rescaleYAxesToWindow(double xMin, double xMax);

    // Whether the X axis is still in "auto-fit" mode (no manual zoom /
    // pan / region-zoom yet). Hosts can use this to decide whether to
    // refit X on a redraw.
    bool xAxisIsAutoFit() const;

    // ---- Line / scatter display mode -------------------------------
    // Single toolbar combo: Line (connected, no scatter), Points
    // (scatter dots only, no line — visually shows the sample rate),
    // or Line + points (both). Stored on ScopePlot so hosts can let
    // QCustomPlot's selection picker work on plain graphs.
    enum class DisplayMode { Line = 0, Points = 1, Both = 2 };
    DisplayMode lineDisplayMode() const;
    void setLineDisplayMode(DisplayMode m);
    // Apply the current mode's line style + scatter style to a single
    // graph. Host calls this after creating a new graph so it picks
    // up the current setting.
    void applyLineDisplayModeTo(QCPGraph* graph) const;

    // Base palette colour for axis `axisIndex`.
    QColor axisBaseColor(int axisIndex) const;
    // A distinct shade of the axis colour for the Nth channel on that axis.
    // Repeats after a handful of channels; the first one is the axis colour
    // itself.
    QColor deriveChannelColor(int axisIndex, int channelIndexOnAxis) const;

    // Zoom helpers (multiply axis range; <1 = zoom in)
    void zoomXBy(double factor);
    void zoomYBy(double factor, int yAxisIndex = -1);   // -1 = all Y axes
    void zoomBothBy(double factor);

    // ---- Two-point measurement -------------------------------------
    // When enabled, left-clicks in the plot interior place markers (up
    // to two). With both placed, a connecting line and a text label
    // show ΔX / ΔY. Right-click clears. setMeasureMode(false) hides
    // everything and restores normal click behaviour.
    void setMeasureMode(bool enabled);
    bool isMeasureMode() const;
    void clearMeasurement();

    // Write the current view to `path`; the format follows the extension:
    // .png (pixel), .pdf (vector), .svg (vector, when built with Qt Svg —
    // see svgExportSupported()). False on unsupported format/write failure.
    bool saveImage(const QString& path);
    static bool svgExportSupported();

public slots:
    void fitAll();
    void saveImageDialog();   // PNG (pixel) / SVG / PDF (vector)
    void togglePause();

signals:
    void pausedChanged(bool paused);
    void yAxesChanged();     // emitted when axes are added/removed/renamed
    void measureModeChanged(bool enabled);
    void lineDisplayModeChanged(DisplayMode m);
    // The app theme flipped (light/dark): axis base colours changed, so
    // hosts should re-derive their channel pens (deriveChannelColor).
    void themePaletteChanged();

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;
    void changeEvent(QEvent* ev) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    // Re-derive every plot colour (background, axes, grid, legend,
    // crosshair, …) from the current QPalette. Called once in the ctor
    // and again on QEvent::PaletteChange.
    void applyThemeColors();

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
