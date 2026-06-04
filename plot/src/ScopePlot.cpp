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
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

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

    QCPItemStraightLine* crossV{nullptr};
    QCPItemStraightLine* crossH{nullptr};
    QCPItemText*         crossText{nullptr};

    bool                 regionDragging{false};
    QPointF              regionStartPx;
    QCPItemRect*         regionRect{nullptr};
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

void ScopePlot::fitAll() {
    impl_->plot->xAxis->rescale();
    rescaleAllYAxes();
    impl_->plot->replot(QCustomPlot::rpQueuedReplot);
}

void ScopePlot::zoomXBy(double factor) {
    impl_->plot->xAxis->scaleRange(factor);
    impl_->plot->replot(QCustomPlot::rpQueuedReplot);
}

void ScopePlot::zoomYBy(double factor, int yAxisIndex) {
    if (yAxisIndex < 0) {
        for (auto* ax : impl_->yAxes) ax->scaleRange(factor);
    } else if (yAxisIndex < impl_->yAxes.size()) {
        impl_->yAxes[yAxisIndex]->scaleRange(factor);
    }
    impl_->plot->replot(QCustomPlot::rpQueuedReplot);
}

void ScopePlot::zoomBothBy(double factor) {
    impl_->plot->xAxis->scaleRange(factor);
    for (auto* ax : impl_->yAxes) ax->scaleRange(factor);
    impl_->plot->replot(QCustomPlot::rpQueuedReplot);
}

void ScopePlot::zoomAt(QPointF mousePx, double factorX, double factorY,
                       int yAxisIndex) {
    if (factorX > 0) {
        const double xCenter = impl_->plot->xAxis->pixelToCoord(mousePx.x());
        impl_->plot->xAxis->scaleRange(factorX, xCenter);
    }
    if (factorY > 0) {
        if (yAxisIndex < 0) {
            for (auto* ax : impl_->yAxes) {
                const double yCenter = ax->pixelToCoord(mousePx.y());
                ax->scaleRange(factorY, yCenter);
            }
        } else if (yAxisIndex < impl_->yAxes.size()) {
            auto* ax = impl_->yAxes[yAxisIndex];
            const double yCenter = ax->pixelToCoord(mousePx.y());
            ax->scaleRange(factorY, yCenter);
        }
    }
    impl_->plot->replot(QCustomPlot::rpQueuedReplot);
}

void ScopePlot::panBy(double fracX, double fracY) {
    const auto rx = impl_->plot->xAxis->range();
    impl_->plot->xAxis->setRange(rx.lower + fracX * rx.size(),
                                 rx.upper + fracX * rx.size());
    for (auto* ax : impl_->yAxes) {
        const auto ry = ax->range();
        ax->setRange(ry.lower + fracY * ry.size(),
                     ry.upper + fracY * ry.size());
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
        // Apply the same fraction-of-range zoom to each Y axis based on
        // their own pixel-mapped equivalent of the rectangle.
        for (auto* ax : impl_->yAxes) {
            const double y0 = ax->pixelToCoord(impl_->regionStartPx.y());
            const double y1 = ax->pixelToCoord(endPx.y());
            ax->setRange(std::min(y0, y1), std::max(y0, y1));
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
        impl_->plot->replot(QCustomPlot::rpQueuedReplot);
    } else if (chosen == fit) {
        // Rescale only this axis to its assigned graphs.
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
            const double notches = w->angleDelta().y() / 120.0;
            if (notches == 0) return false;
            const double factor = std::pow(kZoomStep, notches);
            const QPointF mousePx = w->position();
            const Qt::KeyboardModifiers m = w->modifiers();
            if (m & Qt::ControlModifier)        zoomAt(mousePx, factor, 0,    -1);
            else if (m & Qt::ShiftModifier) {
                const int idx = closestYAxisToPos(mousePx);
                zoomAt(mousePx, 0, factor, idx);
            } else {
                zoomAt(mousePx, factor, factor, -1);
            }
            return true;
        }
        case QEvent::MouseButtonPress: {
            auto* me = static_cast<QMouseEvent*>(ev);
            if (me->button() == Qt::LeftButton
                && (me->modifiers() & Qt::ControlModifier)) {
                beginRegionZoom(me->position());
                return true;
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
                return true;
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
            break;
        }
        default:
            break;
    }
    return false;
}

}  // namespace scope::plot
