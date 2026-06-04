#include "scope/plot/ScopePlot.h"

#include <qcustomplot.h>

#include <QFileDialog>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QShortcut>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <cmath>

namespace scope::plot {

namespace {

// Zoom multiplier per scroll notch (TwinCAT-style ≈ 15%).
constexpr double kZoomStep = 0.85;

// Pan fraction per key press (10% of visible range).
constexpr double kPanFraction = 0.10;

QPointF toPlotCoords(QCustomPlot* plot, QPointF pixel) {
    return {plot->xAxis->pixelToCoord(pixel.x()),
            plot->yAxis->pixelToCoord(pixel.y())};
}

}  // namespace

struct ScopePlot::Impl {
    QCustomPlot*  plot{nullptr};
    QWidget*      toolbar{nullptr};
    QToolButton*  pauseBtn{nullptr};

    bool          paused{false};
    bool          pauseSupported{false};

    // Crosshair items.
    QCPItemStraightLine* crossV{nullptr};
    QCPItemStraightLine* crossH{nullptr};
    QCPItemText*         crossText{nullptr};

    // Region-zoom drag state.
    bool                 regionDragging{false};
    QPointF              regionStartPx;
    QCPItemRect*         regionRect{nullptr};
};

ScopePlot::ScopePlot(QWidget* parent)
    : QWidget(parent), impl_(std::make_unique<Impl>()) {

    impl_->plot = new QCustomPlot(this);
    impl_->plot->setInteractions(QCP::iRangeDrag | QCP::iSelectPlottables);
    impl_->plot->axisRect()->setRangeDrag(Qt::Horizontal | Qt::Vertical);
    impl_->plot->axisRect()->setRangeZoom(Qt::Horizontal | Qt::Vertical);
    impl_->plot->legend->setVisible(true);
    impl_->plot->setFocusPolicy(Qt::ClickFocus);
    impl_->plot->installEventFilter(this);

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
    // Lambda variant for parameterised slots.
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
                  "Zoom Y in  (Shift+Scroll up)",
                  [this]{ zoomYBy(kZoomStep); });
    makeBtnLambda(QString::fromUtf8("↕ −"),
                  "Zoom Y out  (Shift+Scroll down)",
                  [this]{ zoomYBy(1.0 / kZoomStep); });
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

    // ---- Crosshair items (owned by plot) -------------------------------
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
    sc(QKeySequence(Qt::Key_Equal), [this]{ zoomBothBy(kZoomStep); });  // unshifted '+'
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

void ScopePlot::fitAll() {
    impl_->plot->rescaleAxes();
    impl_->plot->replot(QCustomPlot::rpQueuedReplot);
}

void ScopePlot::zoomBothBy(double factor) {
    impl_->plot->xAxis->scaleRange(factor);
    impl_->plot->yAxis->scaleRange(factor);
    impl_->plot->replot(QCustomPlot::rpQueuedReplot);
}

void ScopePlot::zoomXBy(double factor) {
    impl_->plot->xAxis->scaleRange(factor);
    impl_->plot->replot(QCustomPlot::rpQueuedReplot);
}

void ScopePlot::zoomYBy(double factor) {
    impl_->plot->yAxis->scaleRange(factor);
    impl_->plot->replot(QCustomPlot::rpQueuedReplot);
}

void ScopePlot::zoomAt(QPointF mouseInPlot, double factorX, double factorY) {
    if (factorX > 0) impl_->plot->xAxis->scaleRange(factorX, mouseInPlot.x());
    if (factorY > 0) impl_->plot->yAxis->scaleRange(factorY, mouseInPlot.y());
    impl_->plot->replot(QCustomPlot::rpQueuedReplot);
}

void ScopePlot::panBy(double fracX, double fracY) {
    const auto rx = impl_->plot->xAxis->range();
    const auto ry = impl_->plot->yAxis->range();
    impl_->plot->xAxis->setRange(rx.lower + fracX * rx.size(),
                                 rx.upper + fracX * rx.size());
    impl_->plot->yAxis->setRange(ry.lower + fracY * ry.size(),
                                 ry.upper + fracY * ry.size());
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
    // Tiny drags shouldn't actually zoom (likely a click).
    if (dxPx > 4 && dyPx > 4) {
        impl_->plot->xAxis->setRange(std::min(a.x(), b.x()),
                                     std::max(a.x(), b.x()));
        impl_->plot->yAxis->setRange(std::min(a.y(), b.y()),
                                     std::max(a.y(), b.y()));
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
    const auto p = toPlotCoords(impl_->plot, mousePx);
    impl_->crossV->point1->setCoords(p.x(), 0);
    impl_->crossV->point2->setCoords(p.x(), 1);
    impl_->crossH->point1->setCoords(0, p.y());
    impl_->crossH->point2->setCoords(1, p.y());

    QString text = QString("X = %1\n").arg(p.x(), 0, 'g', 6);
    for (int i = 0; i < impl_->plot->graphCount(); ++i) {
        auto* g = impl_->plot->graph(i);
        if (!g->visible() || g->data()->isEmpty()) continue;
        auto it = g->data()->findBegin(p.x(), /*expandedRange=*/false);
        if (it == g->data()->constEnd()) continue;
        text += QString("%1 = %2\n").arg(g->name()).arg(it->value, 0, 'g', 6);
    }
    impl_->crossText->setText(text.trimmed());
    impl_->crossV->setVisible(true);
    impl_->crossH->setVisible(true);
    impl_->crossText->setVisible(true);
    impl_->plot->replot(QCustomPlot::rpQueuedReplot);
}

bool ScopePlot::eventFilter(QObject* obj, QEvent* ev) {
    if (obj != impl_->plot) return false;

    switch (ev->type()) {
        case QEvent::Wheel: {
            auto* w = static_cast<QWheelEvent*>(ev);
            const double notches = w->angleDelta().y() / 120.0;
            if (notches == 0) return false;
            const double factor = std::pow(kZoomStep, notches);
            const auto p = toPlotCoords(impl_->plot, w->position());
            const Qt::KeyboardModifiers m = w->modifiers();
            if (m & Qt::ControlModifier)        zoomAt(p, factor, 0);
            else if (m & Qt::ShiftModifier)     zoomAt(p, 0, factor);
            else                                 zoomAt(p, factor, factor);
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
        default:
            break;
    }
    return false;
}

}  // namespace scope::plot
