#include "scope/plot/ScopePlot.h"

#include <qcustomplot.h>

#include <QAction>
#include <QContextMenuEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QShortcut>
#include <QComboBox>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <cmath>
#include <limits>

#include <spdlog/spdlog.h>

#include <array>
#include <cmath>
#include <limits>

namespace scope::plot {

namespace {

constexpr double kZoomStep = 0.85;
constexpr double kPanFraction = 0.10;

const std::array<QColor, 8> kAxisPalette = {
    QColor( 30, 30, 30),    // Y1 - neutral dark
    QColor(217, 83, 25),    // Y2 - orange
    QColor( 60,120, 30),    // Y3 - green
    QColor(126, 47,142),    // Y4 - purple
    QColor(  0,114,189),    // Y5 - blue
    QColor(237,177, 32),    // Y6 - yellow
    QColor(162, 20, 47),    // Y7 - red
    QColor( 77,190,238),    // Y8 - light blue
};

QPointF toPlotCoords(QCustomPlot* plot, QPointF pixel) {
    return {plot->xAxis->pixelToCoord(pixel.x()),
            plot->yAxis->pixelToCoord(pixel.y())};
}

void styleAxis(QCPAxis* ax, const QColor& c) {
    ax->setBasePen(QPen(c));
    ax->setTickPen(QPen(c));
    ax->setSubTickPen(QPen(c));
    ax->setTickLabelColor(c);
    ax->setLabelColor(c);
}

}  // namespace

struct ScopePlot::Impl {
    QCustomPlot*    plot{nullptr};
    QWidget*        toolbar{nullptr};
    QToolButton*    pauseBtn{nullptr};

    bool            paused{false};
    bool            pauseSupported{false};

    QList<QCPAxis*> yAxes;          // index 0 = plot->yAxis
    // When true, this axis still auto-fits to its data on rescaleAllYAxes().
    // Set to false the moment the user manually zooms / pans the axis (wheel,
    // region zoom, +/− button, context-menu Set range), so the live-preview
    // tick stops fighting the user's manual zoom. Re-enabled by fitAll() or
    // the per-axis "Auto-scale this axis" context-menu action.
    QHash<QCPAxis*, bool> autoFit;

    QCPItemStraightLine* crossV{nullptr};
    QCPItemStraightLine* crossH{nullptr};
    QCPItemText*         crossText{nullptr};

    bool                 regionDragging{false};
    QPointF              regionStartPx;
    QCPItemRect*         regionRect{nullptr};

    // True while the user is holding the left mouse button down on the plot
    // (used to detect pan-via-QCustomPlot-iRangeDrag and mark axes manual).
    bool                 leftButtonDown{false};

    // ---- Two-point measurement -----------------------------------
    bool                 measureMode{false};
    QPointF              measureP1{std::numeric_limits<double>::quiet_NaN(),
                                   std::numeric_limits<double>::quiet_NaN()};
    QPointF              measureP2{std::numeric_limits<double>::quiet_NaN(),
                                   std::numeric_limits<double>::quiet_NaN()};
    QCPItemEllipse*      measureDot1{nullptr};
    QCPItemEllipse*      measureDot2{nullptr};
    QCPItemLine*         measureLine{nullptr};
    QCPItemText*         measureText{nullptr};

    ScopePlot::DisplayMode lineDisplayMode{ScopePlot::DisplayMode::Line};
};

ScopePlot::ScopePlot(QWidget* parent)
    : QWidget(parent), impl_(std::make_unique<Impl>()) {

    impl_->plot = new QCustomPlot(this);
    impl_->plot->setInteractions(QCP::iRangeDrag | QCP::iSelectPlottables
                                  | QCP::iSelectAxes);
    impl_->plot->axisRect()->setRangeDrag(Qt::Horizontal | Qt::Vertical);
    impl_->plot->axisRect()->setRangeZoom(Qt::Horizontal | Qt::Vertical);
    impl_->plot->legend->setVisible(true);
    impl_->plot->setFocusPolicy(Qt::ClickFocus);
    impl_->plot->setContextMenuPolicy(Qt::DefaultContextMenu);
    impl_->plot->installEventFilter(this);

    // Track the default Y axis as index 0 and give it the first palette entry.
    impl_->yAxes.append(impl_->plot->yAxis);
    impl_->plot->yAxis->setLabel("Y1");
    styleAxis(impl_->plot->yAxis, kAxisPalette[0]);
    impl_->autoFit[impl_->plot->yAxis] = true;

    // Hook range-changed on the primary Y axis so QCustomPlot's iRangeDrag
    // pan also marks the axis as manual. addYAxis() does the same for new axes.
    connect(impl_->plot->yAxis,
            QOverload<const QCPRange&>::of(&QCPAxis::rangeChanged), this,
            [this](const QCPRange&){
                if (impl_->leftButtonDown) impl_->autoFit[impl_->plot->yAxis] = false;
            });

    // Same gating for the X axis: once the user pans X (iRangeDrag), the
    // host (e.g. AnalyserPlot) shouldn't auto-refit X on every redraw.
    impl_->autoFit[impl_->plot->xAxis] = true;
    connect(impl_->plot->xAxis,
            QOverload<const QCPRange&>::of(&QCPAxis::rangeChanged), this,
            [this](const QCPRange&){
                if (impl_->leftButtonDown) impl_->autoFit[impl_->plot->xAxis] = false;
            });

    // ---- Toolbar -------------------------------------------------------
    impl_->toolbar = new QWidget(this);
    auto* tb = new QHBoxLayout(impl_->toolbar);
    tb->setContentsMargins(2, 2, 2, 2);
    tb->setSpacing(2);

    auto makeBtn = [this, tb](const QString& text, const QString& tooltip,
                              void (ScopePlot::* slot)()) {
        auto* b = new QToolButton(impl_->toolbar);
        b->setText(text);
        b->setToolTip(tooltip);
        b->setAutoRaise(false);
        connect(b, &QToolButton::clicked, this, slot);
        tb->addWidget(b);
        return b;
    };
    auto makeBtnLambda = [this, tb](const QString& text, const QString& tooltip,
                                    std::function<void()> fn) {
        auto* b = new QToolButton(impl_->toolbar);
        b->setText(text);
        b->setToolTip(tooltip);
        b->setAutoRaise(false);
        connect(b, &QToolButton::clicked, this, fn);
        tb->addWidget(b);
        return b;
    };

    makeBtn(QString::fromUtf8("⤢  Fit"),
            "Fit all data into view  (Home)",
            &ScopePlot::fitAll);
    makeBtnLambda(QString::fromUtf8("↔ +"),
                  "Zoom X in  (Ctrl+Scroll up)",
                  [this]{ zoomXBy(kZoomStep); });
    makeBtnLambda(QString::fromUtf8("↔ −"),
                  "Zoom X out  (Ctrl+Scroll down)",
                  [this]{ zoomXBy(1.0 / kZoomStep); });
    makeBtnLambda(QString::fromUtf8("↕ +"),
                  "Zoom all Y in  (Shift+Scroll up)",
                  [this]{ zoomYBy(kZoomStep); });
    makeBtnLambda(QString::fromUtf8("↕ −"),
                  "Zoom all Y out  (Shift+Scroll down)",
                  [this]{ zoomYBy(1.0 / kZoomStep); });
    tb->addSpacing(8);
    makeBtnLambda(QString::fromUtf8("↕ → Y"),
                  "Fit each Y axis to the data inside the current X "
                  "window. X range stays the same.",
                  [this]{
                      const auto xr = impl_->plot->xAxis->range();
                      rescaleYAxesToWindow(xr.lower, xr.upper);
                  });
    tb->addSpacing(8);
    {
        auto* mb = new QToolButton(impl_->toolbar);
        mb->setText(QString::fromUtf8("Δ Measure"));
        mb->setToolTip(
            "Click two points to mark Δx / Δy / 1/|Δx|.  Right-click "
            "clears.  Click again to leave measurement mode.");
        mb->setCheckable(true);
        mb->setAutoRaise(false);
        connect(mb, &QToolButton::toggled, this,
                [this](bool on){ setMeasureMode(on); });
        connect(this, &ScopePlot::measureModeChanged, mb,
                [mb](bool on){
                    QSignalBlocker b(mb);
                    mb->setChecked(on);
                });
        tb->addWidget(mb);
    }
    tb->addSpacing(8);
    {
        auto* combo = new QComboBox(impl_->toolbar);
        combo->addItem("Line");
        combo->addItem("Points");
        combo->addItem("Line + points");
        combo->setToolTip(
            "Line: connected only (fast on big signals).\n"
            "Points: scatter dots at every sample — lets you see the\n"
            "        sample rate of a signal directly.\n"
            "Line + points: both.");
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int i){
            setLineDisplayMode(static_cast<DisplayMode>(i));
        });
        connect(this, &ScopePlot::lineDisplayModeChanged, combo,
                [combo](DisplayMode m){
            QSignalBlocker b(combo);
            combo->setCurrentIndex(static_cast<int>(m));
        });
        tb->addWidget(combo);
    }
    tb->addSpacing(8);
    makeBtnLambda(QString::fromUtf8("Y+"),
                  "Add a new Y axis (alternates left / right)",
                  [this]{ addYAxis(); });
    tb->addSpacing(8);
    makeBtn(QString::fromUtf8("PNG…"),
            "Save the current view as a PNG image",
            &ScopePlot::savePngDialog);
    tb->addSpacing(8);
    impl_->pauseBtn = makeBtn(QString::fromUtf8("⏸  Pause"),
                              "Freeze the plot while recording continues",
                              &ScopePlot::togglePause);
    impl_->pauseBtn->setVisible(false);
    tb->addStretch();

    // ---- Crosshair + region rect ---------------------------------------
    impl_->crossV = new QCPItemStraightLine(impl_->plot);
    impl_->crossV->point1->setCoords(0, 0);
    impl_->crossV->point2->setCoords(0, 1);
    impl_->crossV->setPen(QPen(QColor(120, 120, 120), 1, Qt::DashLine));
    impl_->crossV->setVisible(false);
    impl_->crossH = new QCPItemStraightLine(impl_->plot);
    impl_->crossH->point1->setCoords(0, 0);
    impl_->crossH->point2->setCoords(1, 0);
    impl_->crossH->setPen(QPen(QColor(120, 120, 120), 1, Qt::DashLine));
    impl_->crossH->setVisible(false);
    impl_->crossText = new QCPItemText(impl_->plot);
    impl_->crossText->setPositionAlignment(Qt::AlignTop | Qt::AlignLeft);
    impl_->crossText->position->setType(QCPItemPosition::ptAxisRectRatio);
    impl_->crossText->position->setCoords(0.01, 0.01);
    impl_->crossText->setFont(QFont("monospace", 8));
    impl_->crossText->setColor(QColor(60, 60, 60));
    impl_->crossText->setVisible(false);

    impl_->regionRect = new QCPItemRect(impl_->plot);
    impl_->regionRect->setPen(QPen(QColor(0, 102, 204), 1, Qt::DashLine));
    impl_->regionRect->setBrush(QBrush(QColor(0, 102, 204, 40)));
    impl_->regionRect->setVisible(false);

    // ---- Measurement items (hidden until measureMode is on) --------
    const QColor measureColor(220, 100, 0);
    auto mkDot = [&](QCPItemEllipse*& dot) {
        dot = new QCPItemEllipse(impl_->plot);
        dot->setPen(QPen(measureColor, 2));
        dot->setBrush(QBrush(measureColor));
        dot->setVisible(false);
    };
    mkDot(impl_->measureDot1);
    mkDot(impl_->measureDot2);
    impl_->measureLine = new QCPItemLine(impl_->plot);
    impl_->measureLine->setPen(QPen(measureColor, 1, Qt::DashLine));
    impl_->measureLine->setVisible(false);
    impl_->measureText = new QCPItemText(impl_->plot);
    impl_->measureText->setPositionAlignment(Qt::AlignBottom | Qt::AlignLeft);
    impl_->measureText->setFont(QFont("monospace", 9));
    impl_->measureText->setColor(measureColor);
    impl_->measureText->setPadding(QMargins(4, 2, 4, 2));
    impl_->measureText->setBrush(QBrush(QColor(255, 255, 255, 200)));
    impl_->measureText->setPen(QPen(measureColor));
    impl_->measureText->setVisible(false);

    // ---- Layout --------------------------------------------------------
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(impl_->toolbar);
    root->addWidget(impl_->plot, /*stretch=*/1);

    // ---- Keyboard shortcuts -------------------------------------------
    auto sc = [this](QKeySequence k, std::function<void()> fn) {
        auto* s = new QShortcut(k, this);
        connect(s, &QShortcut::activated, this, fn);
    };
    sc(QKeySequence(Qt::Key_Home),  [this]{ fitAll(); });
    sc(QKeySequence(Qt::Key_Plus),  [this]{ zoomBothBy(kZoomStep); });
    sc(QKeySequence(Qt::Key_Equal), [this]{ zoomBothBy(kZoomStep); });
    sc(QKeySequence(Qt::Key_Minus), [this]{ zoomBothBy(1.0 / kZoomStep); });
    sc(QKeySequence(Qt::Key_Left),  [this]{ panBy(-kPanFraction, 0); });
    sc(QKeySequence(Qt::Key_Right), [this]{ panBy( kPanFraction, 0); });
    sc(QKeySequence(Qt::Key_Up),    [this]{ panBy(0, -kPanFraction); });
    sc(QKeySequence(Qt::Key_Down),  [this]{ panBy(0,  kPanFraction); });
}

ScopePlot::~ScopePlot() = default;

QCustomPlot* ScopePlot::plot() const { return impl_->plot; }

void ScopePlot::setPauseSupported(bool enabled) {
    impl_->pauseSupported = enabled;
    impl_->pauseBtn->setVisible(enabled);
}

bool ScopePlot::isPaused() const { return impl_->paused; }

void ScopePlot::setPaused(bool paused) {
    if (impl_->paused == paused) return;
    impl_->paused = paused;
    impl_->pauseBtn->setText(paused ? QString::fromUtf8("▶  Resume")
                                    : QString::fromUtf8("⏸  Pause"));
    emit pausedChanged(paused);
}

void ScopePlot::togglePause() { setPaused(!impl_->paused); }

// ---- Measurement ----------------------------------------------------------
bool ScopePlot::isMeasureMode() const { return impl_->measureMode; }

void ScopePlot::setMeasureMode(bool enabled) {
    if (impl_->measureMode == enabled) return;
    impl_->measureMode = enabled;
    if (!enabled) clearMeasurement();
    // Crosshair conflicts visually with markers; hide it while measuring.
    impl_->plot->setCursor(enabled ? Qt::CrossCursor : Qt::ArrowCursor);
    emit measureModeChanged(enabled);
}

// ---- Display mode ---------------------------------------------------------
ScopePlot::DisplayMode ScopePlot::lineDisplayMode() const {
    return impl_->lineDisplayMode;
}

void ScopePlot::applyLineDisplayModeTo(QCPGraph* graph) const {
    if (!graph) return;
    switch (impl_->lineDisplayMode) {
        case DisplayMode::Line:
            graph->setLineStyle(QCPGraph::lsLine);
            graph->setScatterStyle(QCPScatterStyle::ssNone);
            break;
        case DisplayMode::Points:
            graph->setLineStyle(QCPGraph::lsNone);
            graph->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssDisc, 4));
            break;
        case DisplayMode::Both:
            graph->setLineStyle(QCPGraph::lsLine);
            graph->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssDisc, 4));
            break;
    }
}

void ScopePlot::setLineDisplayMode(DisplayMode m) {
    if (impl_->lineDisplayMode == m) return;
    impl_->lineDisplayMode = m;
    for (int g = 0; g < impl_->plot->graphCount(); ++g) {
        applyLineDisplayModeTo(impl_->plot->graph(g));
    }
    impl_->plot->replot(QCustomPlot::rpQueuedReplot);
    emit lineDisplayModeChanged(m);
}

void ScopePlot::clearMeasurement() {
    impl_->measureP1 = {std::numeric_limits<double>::quiet_NaN(),
                        std::numeric_limits<double>::quiet_NaN()};
    impl_->measureP2 = impl_->measureP1;
    impl_->measureDot1->setVisible(false);
    impl_->measureDot2->setVisible(false);
    impl_->measureLine->setVisible(false);
    impl_->measureText->setVisible(false);
    impl_->plot->replot(QCustomPlot::rpQueuedReplot);
}

// ---- Multi-Y-axis API ----------------------------------------------------

int ScopePlot::yAxisCount() const { return impl_->yAxes.size(); }

QCPAxis* ScopePlot::yAxis(int index) const {
    if (index < 0 || index >= impl_->yAxes.size()) return nullptr;
    return impl_->yAxes[index];
}

int ScopePlot::addYAxis(const QString& label, Qt::Alignment side) {
    QCPAxis::AxisType type;
    if (side == Qt::Alignment()) {
        // Alternate: even index → left, odd index → right
        type = (impl_->yAxes.size() % 2 == 0) ? QCPAxis::atLeft : QCPAxis::atRight;
    } else if (side & Qt::AlignRight) {
        type = QCPAxis::atRight;
    } else {
        type = QCPAxis::atLeft;
    }
    auto* ax = impl_->plot->axisRect()->addAxis(type);
    const int idx = impl_->yAxes.size();
    const QString actualLabel = label.isEmpty()
                                  ? QString("Y%1").arg(idx + 1)
                                  : label;
    ax->setLabel(actualLabel);
    ax->setVisible(true);
    styleAxis(ax, kAxisPalette[idx % kAxisPalette.size()]);
    impl_->yAxes.append(ax);
    impl_->autoFit[ax] = true;
    connect(ax, QOverload<const QCPRange&>::of(&QCPAxis::rangeChanged), this,
            [this, ax](const QCPRange&){
                if (impl_->leftButtonDown) impl_->autoFit[ax] = false;
            });
    impl_->plot->replot(QCustomPlot::rpQueuedReplot);
    emit yAxesChanged();
    return idx;
}

bool ScopePlot::removeYAxis(int index, QString* errOut) {
    if (index == 0) {
        if (errOut) *errOut = "Cannot remove the primary Y axis (Y1).";
        return false;
    }
    if (index < 0 || index >= impl_->yAxes.size()) {
        if (errOut) *errOut = "Invalid Y axis index.";
        return false;
    }
    auto* ax = impl_->yAxes[index];
    for (int i = 0; i < impl_->plot->graphCount(); ++i) {
        if (impl_->plot->graph(i)->valueAxis() == ax) {
            if (errOut) *errOut = QString("Y axis '%1' is still in use by '%2'. "
                                          "Reassign that channel first.")
                                      .arg(ax->label())
                                      .arg(impl_->plot->graph(i)->name());
            return false;
        }
    }
    impl_->plot->axisRect()->removeAxis(ax);
    impl_->yAxes.removeAt(index);
    impl_->autoFit.remove(ax);
    impl_->plot->replot(QCustomPlot::rpQueuedReplot);
    emit yAxesChanged();
    return true;
}

void ScopePlot::setGraphYAxis(QCPGraph* graph, int axisIndex) {
    if (!graph || axisIndex < 0 || axisIndex >= impl_->yAxes.size()) return;
    graph->setValueAxis(impl_->yAxes[axisIndex]);
    rescaleAllYAxes();
    impl_->plot->replot(QCustomPlot::rpQueuedReplot);
}

int ScopePlot::graphYAxisIndex(QCPGraph* graph) const {
    if (!graph) return 0;
    for (int i = 0; i < impl_->yAxes.size(); ++i) {
        if (graph->valueAxis() == impl_->yAxes[i]) return i;
    }
    return 0;
}

int ScopePlot::closestYAxisToPos(QPointF pixelPos) const {
    // QCPAxis::selectTest() returns -1 when the axis's selectable parts are
    // disabled (the default), so we can't rely on it. Compute the axis's
    // pixel-X directly: left axes sit at axisRect.left() - offset, right axes
    // at axisRect.right() + offset. Then take horizontal distance to the
    // mouse cursor — hovering near a side picks that side's nearest axis.
    int best = 0;
    double bestDist = std::numeric_limits<double>::infinity();
    const QRect rect = impl_->plot->axisRect()->rect();
    for (int i = 0; i < impl_->yAxes.size(); ++i) {
        auto* ax = impl_->yAxes[i];
        double axisX = 0;
        if (ax->axisType() == QCPAxis::atLeft) {
            axisX = rect.left() - ax->offset();
        } else if (ax->axisType() == QCPAxis::atRight) {
            axisX = rect.right() + ax->offset();
        } else {
            continue;
        }
        const double d = std::abs(pixelPos.x() - axisX);
        if (d < bestDist) { bestDist = d; best = i; }
    }
    return best;
}

QColor ScopePlot::axisBaseColor(int axisIndex) const {
    if (axisIndex < 0) axisIndex = 0;
    return kAxisPalette[axisIndex % kAxisPalette.size()];
}

QColor ScopePlot::deriveChannelColor(int axisIndex, int channelIndexOnAxis) const {
    // The first channel on each axis renders in the axis's exact base colour
    // (so the trace and the axis label match perfectly). Subsequent channels
    // share the hue but get distinct shades by adjusting saturation + value.
    const QColor base = axisBaseColor(axisIndex);
    const int idx = ((channelIndexOnAxis % 6) + 6) % 6;
    if (idx == 0) return base;

    static const int kDeltaSat[6] = {  0,  +20,  -30,  +40,  -50,  +60};
    static const int kDeltaVal[6] = {  0,  -40,  +40,  -70,  +70,  -90};
    int h, s, v, a;
    base.getHsv(&h, &s, &v, &a);
    if (h < 0) h = 0;   // grayscale → fall back to deterministic hue
    s = std::clamp(s + kDeltaSat[idx], 60, 255);
    v = std::clamp(v + kDeltaVal[idx], 30, 255);
    return QColor::fromHsv(h, s, v, a);
}

void ScopePlot::rescaleAllYAxes() {
    for (int i = 0; i < impl_->yAxes.size(); ++i) {
        auto* ax = impl_->yAxes[i];
        // Skip axes the user has manually zoomed/panned — they're in
        // manual mode until fitAll() or per-axis "Auto-scale".
        if (!impl_->autoFit.value(ax, true)) continue;
        bool found = false;
        double min =  std::numeric_limits<double>::infinity();
        double max = -std::numeric_limits<double>::infinity();
        for (int g = 0; g < impl_->plot->graphCount(); ++g) {
            auto* gr = impl_->plot->graph(g);
            if (gr->valueAxis() != ax) continue;
            if (!gr->visible()) continue;
            bool axFound = false;
            const auto range = gr->data()->valueRange(axFound);
            if (axFound) {
                min = std::min(min, range.lower);
                max = std::max(max, range.upper);
                found = true;
            }
        }
        if (found) {
            if (min == max) { min -= 0.5; max += 0.5; }
            const double margin = 0.05 * (max - min);
            ax->setRange(min - margin, max + margin);
        }
    }
}

bool ScopePlot::xAxisIsAutoFit() const {
    return impl_->autoFit.value(impl_->plot->xAxis, true);
}

void ScopePlot::rescaleYAxesToWindow(double xMin, double xMax) {
    if (xMin >= xMax) return;
    for (int i = 0; i < impl_->yAxes.size(); ++i) {
        auto* ax = impl_->yAxes[i];
        bool found = false;
        double mn =  std::numeric_limits<double>::infinity();
        double mx = -std::numeric_limits<double>::infinity();
        for (int g = 0; g < impl_->plot->graphCount(); ++g) {
            auto* gr = impl_->plot->graph(g);
            if (gr->valueAxis() != ax) continue;
            if (!gr->visible()) continue;
            auto data = gr->data();
            auto it    = data->findBegin(xMin, /*expandedRange=*/false);
            const auto end = data->findEnd(xMax, /*expandedRange=*/false);
            for (; it != end; ++it) {
                const double k = it->key;
                if (k < xMin || k > xMax) continue;
                const double v = it->value;
                if (v < mn) mn = v;
                if (v > mx) mx = v;
                found = true;
            }
        }
        if (!found) continue;   // leave this axis untouched
        if (mn == mx) { mn -= 0.5; mx += 0.5; }
        const double margin = 0.05 * (mx - mn);
        ax->setRange(mn - margin, mx + margin);
    }
    impl_->plot->replot(QCustomPlot::rpQueuedReplot);
}

void ScopePlot::fitAll() {
    // Re-arm auto-fit on every axis (X + all Ys) and rescale.
    impl_->plot->xAxis->rescale();
    impl_->autoFit[impl_->plot->xAxis] = true;
    for (auto* ax : impl_->yAxes) impl_->autoFit[ax] = true;
    rescaleAllYAxes();
    impl_->plot->replot(QCustomPlot::rpQueuedReplot);
}

void ScopePlot::zoomXBy(double factor) {
    impl_->plot->xAxis->scaleRange(factor);
    impl_->autoFit[impl_->plot->xAxis] = false;
    impl_->plot->replot(QCustomPlot::rpQueuedReplot);
}

void ScopePlot::zoomYBy(double factor, int yAxisIndex) {
    if (yAxisIndex < 0) {
        for (auto* ax : impl_->yAxes) {
            ax->scaleRange(factor);
            impl_->autoFit[ax] = false;
        }
    } else if (yAxisIndex < impl_->yAxes.size()) {
        auto* ax = impl_->yAxes[yAxisIndex];
        ax->scaleRange(factor);
        impl_->autoFit[ax] = false;
    }
    impl_->plot->replot(QCustomPlot::rpQueuedReplot);
}

void ScopePlot::zoomBothBy(double factor) {
    impl_->plot->xAxis->scaleRange(factor);
    impl_->autoFit[impl_->plot->xAxis] = false;
    for (auto* ax : impl_->yAxes) {
        ax->scaleRange(factor);
        impl_->autoFit[ax] = false;
    }
    impl_->plot->replot(QCustomPlot::rpQueuedReplot);
}

void ScopePlot::zoomAt(QPointF mousePx, double factorX, double factorY,
                       int yAxisIndex) {
    if (factorX > 0) {
        const double xCenter = impl_->plot->xAxis->pixelToCoord(mousePx.x());
        impl_->plot->xAxis->scaleRange(factorX, xCenter);
        impl_->autoFit[impl_->plot->xAxis] = false;
    }
    if (factorY > 0) {
        if (yAxisIndex < 0) {
            for (auto* ax : impl_->yAxes) {
                const double yCenter = ax->pixelToCoord(mousePx.y());
                ax->scaleRange(factorY, yCenter);
                impl_->autoFit[ax] = false;
            }
        } else if (yAxisIndex < impl_->yAxes.size()) {
            auto* ax = impl_->yAxes[yAxisIndex];
            const double yCenter = ax->pixelToCoord(mousePx.y());
            ax->scaleRange(factorY, yCenter);
            impl_->autoFit[ax] = false;
        }
    }
    impl_->plot->replot(QCustomPlot::rpQueuedReplot);
}

void ScopePlot::panBy(double fracX, double fracY) {
    const auto rx = impl_->plot->xAxis->range();
    impl_->plot->xAxis->setRange(rx.lower + fracX * rx.size(),
                                 rx.upper + fracX * rx.size());
    impl_->autoFit[impl_->plot->xAxis] = false;
    for (auto* ax : impl_->yAxes) {
        const auto ry = ax->range();
        ax->setRange(ry.lower + fracY * ry.size(),
                     ry.upper + fracY * ry.size());
        impl_->autoFit[ax] = false;
    }
    impl_->plot->replot(QCustomPlot::rpQueuedReplot);
}

void ScopePlot::savePngDialog() {
    QFileDialog dlg(this, "Save plot as PNG");
    dlg.setAcceptMode(QFileDialog::AcceptSave);
    dlg.setNameFilters({"PNG image (*.png)", "All files (*)"});
    dlg.setDefaultSuffix("png");
    if (dlg.exec() != QDialog::Accepted) return;
    const auto sel = dlg.selectedFiles();
    if (sel.isEmpty()) return;
    QString path = sel.first();
    if (!path.endsWith(".png", Qt::CaseInsensitive)) path += ".png";
    impl_->plot->savePng(path);
}

void ScopePlot::beginRegionZoom(QPointF startPx) {
    impl_->regionDragging = true;
    impl_->regionStartPx = startPx;
    const auto p = toPlotCoords(impl_->plot, startPx);
    impl_->regionRect->topLeft->setCoords(p.x(), p.y());
    impl_->regionRect->bottomRight->setCoords(p.x(), p.y());
    impl_->regionRect->setVisible(true);
    impl_->plot->replot(QCustomPlot::rpQueuedReplot);
}

void ScopePlot::updateRegionZoom(QPointF curPx) {
    const auto a = toPlotCoords(impl_->plot, impl_->regionStartPx);
    const auto b = toPlotCoords(impl_->plot, curPx);
    impl_->regionRect->topLeft->setCoords(std::min(a.x(), b.x()),
                                          std::max(a.y(), b.y()));
    impl_->regionRect->bottomRight->setCoords(std::max(a.x(), b.x()),
                                              std::min(a.y(), b.y()));
    impl_->plot->replot(QCustomPlot::rpQueuedReplot);
}

void ScopePlot::endRegionZoom(QPointF endPx) {
    impl_->regionDragging = false;
    impl_->regionRect->setVisible(false);
    const auto a = toPlotCoords(impl_->plot, impl_->regionStartPx);
    const auto b = toPlotCoords(impl_->plot, endPx);
    const double dxPx = std::abs(endPx.x() - impl_->regionStartPx.x());
    const double dyPx = std::abs(endPx.y() - impl_->regionStartPx.y());
    if (dxPx > 4 && dyPx > 4) {
        impl_->plot->xAxis->setRange(std::min(a.x(), b.x()),
                                     std::max(a.x(), b.x()));
        impl_->autoFit[impl_->plot->xAxis] = false;
        for (auto* ax : impl_->yAxes) {
            const double y0 = ax->pixelToCoord(impl_->regionStartPx.y());
            const double y1 = ax->pixelToCoord(endPx.y());
            ax->setRange(std::min(y0, y1), std::max(y0, y1));
            impl_->autoFit[ax] = false;
        }
    }
    impl_->plot->replot(QCustomPlot::rpQueuedReplot);
}

void ScopePlot::hideCrosshair() {
    impl_->crossV->setVisible(false);
    impl_->crossH->setVisible(false);
    impl_->crossText->setVisible(false);
    impl_->plot->replot(QCustomPlot::rpQueuedReplot);
}

void ScopePlot::updateCrosshair(QPointF mousePx) {
    const double xCoord = impl_->plot->xAxis->pixelToCoord(mousePx.x());
    impl_->crossV->point1->setCoords(xCoord, 0);
    impl_->crossV->point2->setCoords(xCoord, 1);
    impl_->crossH->point1->setCoords(0, impl_->plot->yAxis->pixelToCoord(mousePx.y()));
    impl_->crossH->point2->setCoords(1, impl_->plot->yAxis->pixelToCoord(mousePx.y()));

    QString text = QString("X = %1\n").arg(xCoord, 0, 'g', 6);
    for (int i = 0; i < impl_->plot->graphCount(); ++i) {
        auto* g = impl_->plot->graph(i);
        if (!g->visible() || g->data()->isEmpty()) continue;
        auto it = g->data()->findBegin(xCoord, /*expandedRange=*/false);
        if (it == g->data()->constEnd()) continue;
        text += QString("%1 = %2\n").arg(g->name()).arg(it->value, 0, 'g', 6);
    }
    impl_->crossText->setText(text.trimmed());
    impl_->crossV->setVisible(true);
    impl_->crossH->setVisible(true);
    impl_->crossText->setVisible(true);
    impl_->plot->replot(QCustomPlot::rpQueuedReplot);
}

void ScopePlot::showAxisContextMenu(int axisIndex, QPoint globalPos) {
    if (axisIndex < 0 || axisIndex >= impl_->yAxes.size()) return;
    auto* ax = impl_->yAxes[axisIndex];
    QMenu menu;
    auto* rename = menu.addAction("Rename…");
    auto* range  = menu.addAction("Set range…");
    auto* fit    = menu.addAction("Auto-scale this axis");
    menu.addSeparator();
    auto* remove = menu.addAction("Remove axis");
    if (axisIndex == 0) remove->setEnabled(false);

    QAction* chosen = menu.exec(globalPos);
    if (!chosen) return;

    if (chosen == rename) {
        bool ok = false;
        const QString newLabel = QInputDialog::getText(
            this, "Rename Y axis", "Label:", QLineEdit::Normal,
            ax->label(), &ok);
        if (ok) {
            ax->setLabel(newLabel);
            impl_->plot->replot(QCustomPlot::rpQueuedReplot);
            emit yAxesChanged();
        }
    } else if (chosen == range) {
        bool ok = false;
        const double lo = QInputDialog::getDouble(
            this, "Y axis range", "Min:", ax->range().lower,
            -1e15, 1e15, 6, &ok);
        if (!ok) return;
        const double hi = QInputDialog::getDouble(
            this, "Y axis range", "Max:", ax->range().upper,
            -1e15, 1e15, 6, &ok);
        if (!ok || hi <= lo) return;
        ax->setRange(lo, hi);
        impl_->autoFit[ax] = false;  // user fixed a range manually
        impl_->plot->replot(QCustomPlot::rpQueuedReplot);
    } else if (chosen == fit) {
        // Re-enable auto-fit for this axis and rescale it now.
        impl_->autoFit[ax] = true;
        bool found = false;
        double min =  std::numeric_limits<double>::infinity();
        double max = -std::numeric_limits<double>::infinity();
        for (int g = 0; g < impl_->plot->graphCount(); ++g) {
            auto* gr = impl_->plot->graph(g);
            if (gr->valueAxis() != ax || !gr->visible()) continue;
            bool axFound = false;
            const auto r = gr->data()->valueRange(axFound);
            if (axFound) {
                min = std::min(min, r.lower);
                max = std::max(max, r.upper);
                found = true;
            }
        }
        if (found) {
            if (min == max) { min -= 0.5; max += 0.5; }
            const double margin = 0.05 * (max - min);
            ax->setRange(min - margin, max + margin);
            impl_->plot->replot(QCustomPlot::rpQueuedReplot);
        }
    } else if (chosen == remove) {
        QString err;
        if (!removeYAxis(axisIndex, &err)) {
            QMessageBox::information(this, "Can't remove", err);
        }
    }
}

bool ScopePlot::eventFilter(QObject* obj, QEvent* ev) {
    if (obj != impl_->plot) return false;

    switch (ev->type()) {
        case QEvent::Wheel: {
            auto* w = static_cast<QWheelEvent*>(ev);
            // X11 swaps angleDelta() from y() to x() when Shift is held; high-
            // precision devices may only fill pixelDelta(). Take whichever
            // non-zero source we can find.
            const QPoint d = w->angleDelta();
            int rawDelta = (d.y() != 0) ? d.y() : d.x();
            if (rawDelta == 0) {
                const QPoint p = w->pixelDelta();
                rawDelta = (p.y() != 0) ? p.y() : p.x();
            }
            if (rawDelta == 0) return false;
            const double notches = rawDelta / 120.0;
            const double factor = std::pow(kZoomStep, notches);
            const QPointF mousePx = w->position();
            const Qt::KeyboardModifiers m = w->modifiers();

            // Where is the cursor relative to the inner plot rect?
            const QRect ar = impl_->plot->axisRect()->rect();
            const bool overYAxisArea = (mousePx.x() < ar.left()
                                        || mousePx.x() > ar.right());
            const bool overXAxisArea = (mousePx.y() < ar.top()
                                        || mousePx.y() > ar.bottom());

            spdlog::debug("Wheel: angleDelta=({},{}), pixelDelta=({},{}), "
                          "mods=0x{:x}, mouse=({:.0f},{:.0f}), "
                          "overY={}, overX={}",
                          d.x(), d.y(),
                          w->pixelDelta().x(), w->pixelDelta().y(),
                          static_cast<int>(m), mousePx.x(), mousePx.y(),
                          overYAxisArea, overXAxisArea);

            // Routing (first match wins):
            //  1. Ctrl modifier         → X only
            //  2. Shift / Alt modifier  → if cursor is hovering near a Y
            //                             axis: that axis only. Otherwise
            //                             (mouse in plot interior): all Y
            //                             axes together. This way you can
            //                             still hit "zoom all Y" with Shift
            //                             when you don't want to pick one.
            //  3. Cursor over a Y-axis label area (no modifier) → that axis
            //  4. Cursor over the X-axis label area (no modifier) → X only
            //  5. Default (cursor inside plot rect, no modifier) → zoom both
            if (m & Qt::ControlModifier) {
                zoomAt(mousePx, factor, 0, -1);
            } else if (m & (Qt::ShiftModifier | Qt::AltModifier)) {
                if (overYAxisArea) {
                    const int idx = closestYAxisToPos(mousePx);
                    zoomAt(mousePx, 0, factor, idx);
                } else {
                    zoomAt(mousePx, 0, factor, -1);   // all Y axes
                }
            } else if (overYAxisArea) {
                const int idx = closestYAxisToPos(mousePx);
                zoomAt(mousePx, 0, factor, idx);
            } else if (overXAxisArea) {
                zoomAt(mousePx, factor, 0, -1);
            } else {
                zoomAt(mousePx, factor, factor, -1);
            }
            return true;
        }
        case QEvent::MouseButtonPress: {
            auto* me = static_cast<QMouseEvent*>(ev);
            // Measurement: left = place next point, right = clear.
            if (impl_->measureMode) {
                if (me->button() == Qt::RightButton) {
                    clearMeasurement();
                    return true;
                }
                if (me->button() == Qt::LeftButton) {
                    const QPointF px = me->position();
                    const double x = impl_->plot->xAxis->pixelToCoord(px.x());
                    const double y = impl_->plot->yAxis->pixelToCoord(px.y());
                    // First click → place P1. Second click → P2 + show
                    // deltas. Third click → restart from P1 again.
                    const bool haveP1 = !std::isnan(impl_->measureP1.x());
                    const bool haveP2 = !std::isnan(impl_->measureP2.x());
                    if (!haveP1 || haveP2) {
                        // start fresh
                        impl_->measureP1 = {x, y};
                        impl_->measureP2 = {std::numeric_limits<double>::quiet_NaN(),
                                            std::numeric_limits<double>::quiet_NaN()};
                    } else {
                        impl_->measureP2 = {x, y};
                    }
                    // Refresh visuals.
                    auto setDot = [&](QCPItemEllipse* d, const QPointF& p, bool vis) {
                        if (!vis) { d->setVisible(false); return; }
                        const double r = 4.0;
                        const double xp = impl_->plot->xAxis->coordToPixel(p.x());
                        const double yp = impl_->plot->yAxis->coordToPixel(p.y());
                        d->topLeft->setType(QCPItemPosition::ptAbsolute);
                        d->bottomRight->setType(QCPItemPosition::ptAbsolute);
                        d->topLeft->setCoords(xp - r, yp - r);
                        d->bottomRight->setCoords(xp + r, yp + r);
                        d->setVisible(true);
                    };
                    setDot(impl_->measureDot1, impl_->measureP1,
                           !std::isnan(impl_->measureP1.x()));
                    setDot(impl_->measureDot2, impl_->measureP2,
                           !std::isnan(impl_->measureP2.x()));
                    const bool both = !std::isnan(impl_->measureP1.x())
                                   && !std::isnan(impl_->measureP2.x());
                    impl_->measureLine->setVisible(both);
                    impl_->measureText->setVisible(both);
                    if (both) {
                        impl_->measureLine->start->setType(QCPItemPosition::ptPlotCoords);
                        impl_->measureLine->end->setType(QCPItemPosition::ptPlotCoords);
                        impl_->measureLine->start->setCoords(
                            impl_->measureP1.x(), impl_->measureP1.y());
                        impl_->measureLine->end->setCoords(
                            impl_->measureP2.x(), impl_->measureP2.y());
                        const double dx = impl_->measureP2.x() - impl_->measureP1.x();
                        const double dy = impl_->measureP2.y() - impl_->measureP1.y();
                        const double absDx = std::abs(dx);
                        QString dxStr;
                        if (absDx != 0 && absDx < 1e-3)
                            dxStr = QString("%1 ms").arg(dx * 1e3, 0, 'g', 6);
                        else
                            dxStr = QString("%1 s").arg(dx, 0, 'g', 6);
                        const double freq = (dx != 0) ? 1.0 / std::abs(dx) : 0.0;
                        QString line2;
                        if (freq > 0) {
                            line2 = QString("1/|Δx| = %1 Hz")
                                        .arg(freq, 0, 'g', 6);
                        }
                        impl_->measureText->setText(
                            QString("Δx = %1\nΔy = %2%3")
                                .arg(dxStr)
                                .arg(dy, 0, 'g', 6)
                                .arg(line2.isEmpty() ? QString()
                                                     : "\n" + line2));
                        impl_->measureText->position->setType(QCPItemPosition::ptPlotCoords);
                        const double midX = (impl_->measureP1.x() + impl_->measureP2.x()) / 2.0;
                        const double topY = std::max(impl_->measureP1.y(), impl_->measureP2.y());
                        impl_->measureText->position->setCoords(midX, topY);
                    }
                    impl_->plot->replot(QCustomPlot::rpQueuedReplot);
                    return true;
                }
            }
            if (me->button() == Qt::LeftButton
                && (me->modifiers() & Qt::ControlModifier)) {
                beginRegionZoom(me->position());
                return true;
            }
            if (me->button() == Qt::LeftButton) {
                impl_->leftButtonDown = true;
            }
            break;
        }
        case QEvent::MouseMove: {
            auto* me = static_cast<QMouseEvent*>(ev);
            if (impl_->regionDragging) {
                updateRegionZoom(me->position());
                return true;
            }
            updateCrosshair(me->position());
            break;
        }
        case QEvent::MouseButtonRelease: {
            auto* me = static_cast<QMouseEvent*>(ev);
            if (impl_->regionDragging && me->button() == Qt::LeftButton) {
                endRegionZoom(me->position());
                impl_->leftButtonDown = false;
                return true;
            }
            if (me->button() == Qt::LeftButton) {
                impl_->leftButtonDown = false;
            }
            break;
        }
        case QEvent::Leave:
            hideCrosshair();
            break;
        case QEvent::ContextMenu: {
            auto* cm = static_cast<QContextMenuEvent*>(ev);
            const QPointF pos = cm->pos();
            for (int i = 0; i < impl_->yAxes.size(); ++i) {
                const double d = impl_->yAxes[i]->selectTest(pos, false);
                if (d >= 0 && d < 8.0) {
                    showAxisContextMenu(i, cm->globalPos());
                    return true;
                }
            }
            const double dX = impl_->plot->xAxis->selectTest(pos, false);
            if (dX >= 0 && dX < 8.0) {
                QMenu menu;
                auto* rename = menu.addAction("Rename…");
                auto* range  = menu.addAction("Set range…");
                auto* fit    = menu.addAction("Auto-scale X");
                QAction* chosen = menu.exec(cm->globalPos());
                if (!chosen) return true;
                if (chosen == rename) {
                    bool ok = false;
                    const QString newLabel = QInputDialog::getText(
                        this, "Rename X axis", "Label:",
                        QLineEdit::Normal,
                        impl_->plot->xAxis->label(), &ok);
                    if (ok) {
                        impl_->plot->xAxis->setLabel(newLabel);
                        impl_->plot->replot(QCustomPlot::rpQueuedReplot);
                    }
                } else if (chosen == range) {
                    bool ok = false;
                    const double lo = QInputDialog::getDouble(
                        this, "X axis range", "Min:",
                        impl_->plot->xAxis->range().lower,
                        -1e15, 1e15, 6, &ok);
                    if (!ok) return true;
                    const double hi = QInputDialog::getDouble(
                        this, "X axis range", "Max:",
                        impl_->plot->xAxis->range().upper,
                        -1e15, 1e15, 6, &ok);
                    if (!ok || hi <= lo) return true;
                    impl_->plot->xAxis->setRange(lo, hi);
                    impl_->autoFit[impl_->plot->xAxis] = false;
                    impl_->plot->replot(QCustomPlot::rpQueuedReplot);
                } else if (chosen == fit) {
                    impl_->autoFit[impl_->plot->xAxis] = true;
                    impl_->plot->xAxis->rescale();
                    impl_->plot->replot(QCustomPlot::rpQueuedReplot);
                }
                return true;
            }
            break;
        }
        default:
            break;
    }
    return false;
}

}  // namespace scope::plot
