#include "scope/plot/ScopePlot.h"

#include <qcustomplot.h>

#include <QAction>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QShortcut>
#ifdef SCOPE_HAVE_QTSVG
#include <QSvgGenerator>
#endif
#include <QComboBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <cmath>
#include <limits>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>

namespace scope::plot {

namespace {

constexpr double kZoomStep = 0.85;
constexpr double kPanFraction = 0.10;

// Press-and-hold on a Zoom or Move button keeps going: after kHoldDelayMs the
// action repeats every kHoldStartMs, and each repeat shortens the gap by
// kHoldDecay down to kHoldMinMs, so a held button ramps up instead of crawling
// at one step per tick. The delay is long enough that an ordinary click never
// repeats by accident.
//
// kHoldMinMs is one display frame: replot() is queued and coalesces, so firing
// faster than the screen refreshes only drops frames. To keep accelerating past
// that the *step* has to grow, which is what kHoldBoost does — it multiplies
// the pan fraction once the interval has bottomed out, up to kHoldBoostMax, so
// a held Move ends up covering ~50 view-widths a second instead of 4.
//
// Zoom deliberately takes no boost: its step is a ratio, so holding it is
// already geometric (0.85 per frame is ~1/750 of the range per second).
constexpr int    kHoldDelayMs   = 500;
constexpr int    kHoldStartMs   = 140;
constexpr int    kHoldMinMs     = 16;
constexpr double kHoldDecay     = 0.82;
constexpr double kHoldBoost     = 1.05;
constexpr double kHoldBoostMax  = 8.0;

// The arrow keys pan by that same kPanFraction step, and holding one speeds up
// too. The keyboard's repeat rate belongs to the OS, though, so only the step
// can grow here: every repeat that lands within kKeyRampGapMs of the previous
// one, in the same direction, multiplies it by kKeyPanBoost up to the same
// kHoldBoostMax ceiling. At a typical 30 repeats a second that reaches the cap
// in about a second. A longer pause — or a change of direction — starts over,
// so a single tap always moves exactly one step, and tapping fast ramps up the
// way holding does.
constexpr int    kKeyRampGapMs  = 300;
constexpr double kKeyPanBoost   = 1.08;

// Holding an arrow also takes the wheel over: while one is down, a notch moves
// the view along that arrow instead of zooming, and feeds the same ramp — so
// scrolling on a held arrow is how you cover ground fast, and each notch goes
// further than the last. Rolling the wheel backwards moves back the other way.
//
// The release is read from the application-wide event filter, but a grab or a
// lost focus can swallow it, and a stuck flag would leave the wheel unable to
// zoom. kArrowHeldStaleMs is the backstop: auto-repeat refreshes the clock ~30
// times a second, so a genuinely held key never goes stale, while a missed
// release clears itself in two seconds.
constexpr int    kArrowHeldStaleMs = 2000;

bool isArrowKey(int key) {
    return key == Qt::Key_Left || key == Qt::Key_Right
        || key == Qt::Key_Up   || key == Qt::Key_Down;
}

// Axis (and therefore channel) base colours per theme. Y1 is the neutral
// "ink" colour; the rest are picked for contrast against the respective
// plot background.
const std::array<QColor, 8> kAxisPaletteLight = {
    QColor( 30, 30, 30),    // Y1 - neutral dark
    QColor(217, 83, 25),    // Y2 - orange
    QColor( 60,120, 30),    // Y3 - green
    QColor(126, 47,142),    // Y4 - purple
    QColor(  0,114,189),    // Y5 - blue
    QColor(237,177, 32),    // Y6 - yellow
    QColor(162, 20, 47),    // Y7 - red
    QColor( 77,190,238),    // Y8 - light blue
};

const std::array<QColor, 8> kAxisPaletteDark = {
    QColor(231,234,238),    // Y1 - neutral light
    QColor(255,159, 90),    // Y2 - orange
    QColor(126,206, 88),    // Y3 - green
    QColor(199,146,234),    // Y4 - purple
    QColor( 77,171,247),    // Y5 - blue
    QColor(255,212, 59),    // Y6 - yellow
    QColor(255,107,107),    // Y7 - red
    QColor(102,217,239),    // Y8 - cyan
};

// 0..1 mix of a towards b.
QColor mixColor(const QColor& a, const QColor& b, double t) {
    return QColor(static_cast<int>(a.red()   + (b.red()   - a.red())   * t),
                  static_cast<int>(a.green() + (b.green() - a.green()) * t),
                  static_cast<int>(a.blue()  + (b.blue()  - a.blue())  * t));
}

QColor withAlphaCh(QColor c, int a) { c.setAlpha(a); return c; }

// Theme detection by background lightness — works for any palette the app
// (or a standalone host) installs, with no dependency on the style lib.
const std::array<QColor, 8>& axisPaletteFor(const QWidget* w) {
    const bool dark = w->palette().color(QPalette::Base).lightness() < 128;
    return dark ? kAxisPaletteDark : kAxisPaletteLight;
}

QPointF toPlotCoords(QCustomPlot* plot, QPointF pixel) {
    return {plot->xAxis->pixelToCoord(pixel.x()),
            plot->yAxis->pixelToCoord(pixel.y())};
}

// Small painted camera icon for the screenshot button — drawn here so the
// plot lib keeps zero dependency on the style lib. The mid gray reads on
// both the light and dark theme.
QIcon cameraIcon() {
    QIcon icon;
    const QColor c(0x8a, 0x91, 0x9c);
    for (int size : {16, 20, 24, 32}) {
        for (int dpr : {1, 2}) {
            QPixmap pm(size * dpr, size * dpr);
            pm.fill(Qt::transparent);
            {
                QPainter p(&pm);
                p.setRenderHint(QPainter::Antialiasing);
                p.scale(size * dpr / 24.0, size * dpr / 24.0);
                p.setPen(QPen(c, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                // Body, viewfinder bump, lens.
                p.drawRoundedRect(QRectF(3, 7, 18, 12.5), 2.5, 2.5);
                QPainterPath bump;
                bump.moveTo(8.6, 7);
                bump.lineTo(10, 4.4);
                bump.lineTo(14, 4.4);
                bump.lineTo(15.4, 7);
                p.drawPath(bump);
                p.drawEllipse(QPointF(12, 13.2), 3.6, 3.6);
            }
            pm.setDevicePixelRatio(dpr);
            icon.addPixmap(pm);
        }
    }
    return icon;
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

    // ---- Arrow-key pan ramp --------------------------------------
    // Direction of the last arrow pan, the step multiplier it used, and how
    // long ago it fired. A QShortcut has no release to reset on, so the gap
    // between repeats is what tells a held key from a fresh press.
    QPointF        keyPanDir;
    double         keyPanBoost{1.0};
    QElapsedTimer  keyPanClock;
    // Whether that arrow is still down — set by the shortcut, cleared by the
    // key release (or by the window losing focus with the key still down).
    bool           arrowDown{false};

    // Step multiplier for the next arrow-key pan in direction `dir`.
    double nextKeyPanBoost(QPointF dir) {
        const bool ramping = keyPanClock.isValid()
                          && keyPanClock.elapsed() <= kKeyRampGapMs
                          && dir == keyPanDir;
        keyPanBoost = ramping ? std::min(kHoldBoostMax,
                                         keyPanBoost * kKeyPanBoost)
                              : 1.0;
        keyPanDir = dir;
        keyPanClock.start();
        arrowDown = true;
        return keyPanBoost;
    }

    // Is an arrow key down right now? Decides whether the wheel moves the
    // view instead of zooming.
    bool arrowHeld() const {
        return arrowDown && keyPanClock.isValid()
            && keyPanClock.elapsed() <= kArrowHeldStaleMs;
    }
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
    // Arrow keys arrive as QShortcuts, which fire on press and say nothing
    // about release; the release has to be caught wherever the focus happens
    // to be, hence application-wide. eventFilter() only ever reads it.
    if (auto* app = QCoreApplication::instance()) app->installEventFilter(this);

    // Track the default Y axis as index 0; applyThemeColors() at the end of
    // the ctor gives it the first palette entry.
    impl_->yAxes.append(impl_->plot->yAxis);
    impl_->plot->yAxis->setLabel("Y1");
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
    // "toolbarStrip" + WA_StyledBackground hooks the app stylesheet's
    // toolbar-zone styling (strip background + bottom hairline) when the
    // shell theme is active; without a stylesheet it stays a plain row.
    impl_->toolbar = new QWidget(this);
    impl_->toolbar->setProperty("scopeRole", "toolbarStrip");
    impl_->toolbar->setAttribute(Qt::WA_StyledBackground, true);
    auto* tb = new QHBoxLayout(impl_->toolbar);
    tb->setContentsMargins(8, 4, 8, 4);
    tb->setSpacing(2);

    // Related actions live in thin-bordered boxes ("button groups"), some
    // with a caption to their right:  [⤢ ↕] Fit   [↔+ ↔− ↕+ ↕−] Zoom
    // [← → ↑ ↓] Move  …  [Y+ Y−]. The box styling comes from the app
    // stylesheet via the "btnGroup" role.
    auto makeGroup = [&]() -> QHBoxLayout* {
        auto* box = new QWidget(impl_->toolbar);
        box->setProperty("scopeRole", "btnGroup");
        box->setAttribute(Qt::WA_StyledBackground, true);
        auto* l = new QHBoxLayout(box);
        l->setContentsMargins(3, 1, 3, 1);
        l->setSpacing(1);
        tb->addWidget(box);
        return l;
    };
    auto makeBtnInto = [this](QHBoxLayout* into, const QString& text,
                              const QString& tooltip, std::function<void()> fn) {
        auto* b = new QToolButton(impl_->toolbar);
        b->setText(text);
        b->setToolTip(tooltip);
        b->setAutoRaise(true);
        connect(b, &QToolButton::clicked, this, fn);
        into->addWidget(b);
        return b;
    };
    // Zoom and Move buttons repeat while held, accelerating as described above;
    // fn is handed the current boost. Fit and Y+/Y- are one-shot actions and
    // keep the plain clicked() wiring of makeBtnInto.
    auto makeHoldBtnInto = [this](QHBoxLayout* into, const QString& text,
                                  const QString& tooltip,
                                  std::function<void(double)> fn) {
        auto* b = new QToolButton(impl_->toolbar);
        b->setText(text);
        b->setToolTip(tooltip);
        b->setAutoRaise(true);
        // One timer per button: pressed() acts at once and arms the delay, each
        // timeout acts again and arms a shorter one, released() stops. Qt's own
        // autoRepeat can't accelerate, and it re-emits pressed() on every tick,
        // which leaves nowhere to reset the ramp. isDown() covers the holds that
        // never see a release — button hidden or disabled, window deactivated
        // mid-hold.
        auto* timer = new QTimer(b);
        timer->setSingleShot(true);
        struct Ramp { int gap; double boost; };
        auto r = std::make_shared<Ramp>(Ramp{kHoldStartMs, 1.0});
        connect(timer, &QTimer::timeout, this, [b, fn, timer, r]{
            if (!b->isDown()) return;
            fn(r->boost);
            timer->start(r->gap);       // this gap, then ramp for the next
            if (r->gap > kHoldMinMs) {
                r->gap = std::max(kHoldMinMs,
                                  static_cast<int>(r->gap * kHoldDecay));
            } else {
                // Interval is at the frame floor; grow the step instead.
                r->boost = std::min(kHoldBoostMax, r->boost * kHoldBoost);
            }
        });
        connect(b, &QToolButton::pressed, this, [fn, timer, r]{
            fn(1.0);
            *r = Ramp{kHoldStartMs, 1.0};
            timer->start(kHoldDelayMs);
        });
        connect(b, &QToolButton::released, timer, &QTimer::stop);
        into->addWidget(b);
        return b;
    };
    auto addCaption = [&](const QString& text) {
        auto* cap = new QLabel(text, impl_->toolbar);
        cap->setProperty("scopeRole", "dim");
        tb->addSpacing(3);
        tb->addWidget(cap);
    };

    {   // [⤢ ↕] Fit
        auto* g = makeGroup();
        makeBtnInto(g, QString::fromUtf8("⤢"),
                    "Fit all data into view — X and all Y axes  (Home)",
                    [this]{ fitAll(); });
        makeBtnInto(g, QString::fromUtf8("↕"),
                    "Fit each Y axis to the data inside the current X "
                    "window. X range stays the same.",
                    [this]{
                        const auto xr = impl_->plot->xAxis->range();
                        rescaleYAxesToWindow(xr.lower, xr.upper);
                    });
        addCaption(QString::fromUtf8("Fit"));
    }
    tb->addSpacing(10);
    {   // [↔+ ↔− ↕+ ↕−] Zoom
        auto* g = makeGroup();
        makeHoldBtnInto(g, QString::fromUtf8("↔ +"),
                    "Zoom X in  (Ctrl+Scroll up).  Hold to keep zooming.",
                    [this](double){ zoomXBy(kZoomStep); });
        makeHoldBtnInto(g, QString::fromUtf8("↔ −"),
                    "Zoom X out  (Ctrl+Scroll down).  Hold to keep zooming.",
                    [this](double){ zoomXBy(1.0 / kZoomStep); });
        makeHoldBtnInto(g, QString::fromUtf8("↕ +"),
                    "Zoom all Y in  (Shift+Scroll up).  Hold to keep zooming.",
                    [this](double){ zoomYBy(kZoomStep); });
        makeHoldBtnInto(g, QString::fromUtf8("↕ −"),
                    "Zoom all Y out  (Shift+Scroll down).  Hold to keep zooming.",
                    [this](double){ zoomYBy(1.0 / kZoomStep); });
        addCaption(QString::fromUtf8("Zoom"));
    }
    tb->addSpacing(10);
    {   // [← → ↑ ↓] Move — the arrow keys' step, on the toolbar
        auto* g = makeGroup();
        makeHoldBtnInto(g, QString::fromUtf8("←"),
                    "Move the view left — show earlier X  (Left arrow).  "
                    "Hold to keep moving, faster the longer you hold.  "
                    "Scrolling with the arrow held moves too, instead of "
                    "zooming.",
                    [this](double k){ panBy(-kPanFraction * k, 0); });
        makeHoldBtnInto(g, QString::fromUtf8("→"),
                    "Move the view right — show later X  (Right arrow).  "
                    "Hold to keep moving, faster the longer you hold.  "
                    "Scrolling with the arrow held moves too, instead of "
                    "zooming.",
                    [this](double k){ panBy( kPanFraction * k, 0); });
        makeHoldBtnInto(g, QString::fromUtf8("↑"),
                    "Move the view up — every Y axis shows higher values  "
                    "(Up arrow).  Hold to keep moving, faster the longer you "
                    "hold.  Scrolling with the arrow held moves too, instead "
                    "of zooming.",
                    [this](double k){ panBy(0,  kPanFraction * k); });
        makeHoldBtnInto(g, QString::fromUtf8("↓"),
                    "Move the view down — every Y axis shows lower values  "
                    "(Down arrow).  Hold to keep moving, faster the longer you "
                    "hold.  Scrolling with the arrow held moves too, instead "
                    "of zooming.",
                    [this](double k){ panBy(0, -kPanFraction * k); });
        addCaption(QString::fromUtf8("Move"));
    }
    tb->addSpacing(10);
    {
        auto* mb = new QToolButton(impl_->toolbar);
        mb->setText(QString::fromUtf8("Δ Measure"));
        mb->setToolTip(
            "Click two points to mark Δx / Δy / 1/|Δx|.  Right-click "
            "clears.  Click again to leave measurement mode.");
        mb->setCheckable(true);
        mb->setAutoRaise(true);
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
    tb->addSpacing(10);
    {   // [Y+ Y−]
        auto* g = makeGroup();
        makeBtnInto(g, QString::fromUtf8("Y+"),
                    "Add a new Y axis (alternates left / right)",
                    [this]{ addYAxis(); });
        makeBtnInto(g, QString::fromUtf8("Y−"),
                    "Remove the last Y axis (it must have no channels "
                    "assigned). Right-click an axis to remove a specific one.",
                    [this]{
                        const int last = yAxisCount() - 1;
                        if (last <= 0) return;
                        QString err;
                        if (!removeYAxis(last, &err)) {
                            QMessageBox::information(this, "Can't remove", err);
                        }
                    });
    }
    tb->addSpacing(10);
    {
        auto* b = new QToolButton(impl_->toolbar);
        b->setIcon(cameraIcon());
        b->setToolTip("Save the current view as an image — PNG (pixel), or\n"
                      "SVG / PDF (vector: stays sharp at any zoom).");
        b->setAutoRaise(true);
        connect(b, &QToolButton::clicked, this, &ScopePlot::saveImageDialog);
        tb->addWidget(b);
    }
    tb->addSpacing(8);
    {
        impl_->pauseBtn = new QToolButton(impl_->toolbar);
        impl_->pauseBtn->setText(QString::fromUtf8("⏸  Pause"));
        impl_->pauseBtn->setToolTip("Freeze the plot while recording continues");
        impl_->pauseBtn->setAutoRaise(true);
        connect(impl_->pauseBtn, &QToolButton::clicked,
                this, &ScopePlot::togglePause);
        tb->addWidget(impl_->pauseBtn);
        impl_->pauseBtn->setVisible(false);
    }
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
    // Arrow keys are the Move buttons on the keyboard: same step, same
    // directions (Up shows higher values), and holding one accelerates.
    auto panKey = [this](double dx, double dy) {
        return [this, dx, dy] {
            const double k = impl_->nextKeyPanBoost(QPointF(dx, dy));
            panBy(dx * kPanFraction * k, dy * kPanFraction * k);
        };
    };
    sc(QKeySequence(Qt::Key_Left),  panKey(-1,  0));
    sc(QKeySequence(Qt::Key_Right), panKey( 1,  0));
    sc(QKeySequence(Qt::Key_Up),    panKey( 0,  1));
    sc(QKeySequence(Qt::Key_Down),  panKey( 0, -1));

    applyThemeColors();
}

ScopePlot::~ScopePlot() = default;

void ScopePlot::applyThemeColors() {
    auto* plot = impl_->plot;
    const QPalette pal = palette();
    const bool   dark   = pal.color(QPalette::Base).lightness() < 128;
    const QColor canvas = pal.color(QPalette::Base);    // plot area
    const QColor frame  = pal.color(QPalette::Window);  // outside the axes
    const QColor text   = pal.color(QPalette::Text);
    const QColor sub    = mixColor(text, frame, 0.35);  // ticks, secondary ink
    const QColor gridC  = mixColor(text, canvas, dark ? 0.84 : 0.88);

    plot->setBackground(QBrush(frame));
    plot->axisRect()->setBackground(QBrush(canvas));

    styleAxis(plot->xAxis, sub);
    const auto& axisColors = axisPaletteFor(this);
    for (int i = 0; i < impl_->yAxes.size(); ++i) {
        styleAxis(impl_->yAxes[i], axisColors[i % axisColors.size()]);
    }
    auto styleGrid = [&](QCPAxis* ax) {
        ax->grid()->setPen(QPen(gridC, 1));
        ax->grid()->setZeroLinePen(QPen(mixColor(text, canvas, dark ? 0.6 : 0.66), 1));
    };
    styleGrid(plot->xAxis);
    for (auto* ax : impl_->yAxes) styleGrid(ax);

    plot->legend->setBrush(QBrush(withAlphaCh(canvas, 235)));
    plot->legend->setBorderPen(QPen(mixColor(text, frame, 0.75)));
    plot->legend->setTextColor(text);

    const QColor crossC = withAlphaCh(sub, 170);
    impl_->crossV->setPen(QPen(crossC, 1, Qt::DashLine));
    impl_->crossH->setPen(QPen(crossC, 1, Qt::DashLine));
    impl_->crossText->setColor(text);
    impl_->crossText->setBrush(QBrush(withAlphaCh(canvas, 210)));
    impl_->crossText->setPadding(QMargins(4, 2, 4, 2));

    const QColor accent = pal.color(QPalette::Highlight);
    impl_->regionRect->setPen(QPen(accent, 1, Qt::DashLine));
    impl_->regionRect->setBrush(QBrush(withAlphaCh(accent, 40)));

    // Measurement overlay keeps its orange identity; only the label's
    // backdrop follows the canvas.
    impl_->measureText->setBrush(QBrush(withAlphaCh(canvas, 215)));
}

void ScopePlot::changeEvent(QEvent* ev) {
    QWidget::changeEvent(ev);
    if (ev->type() == QEvent::PaletteChange) {
        applyThemeColors();
        emit themePaletteChanged();
        impl_->plot->replot(QCustomPlot::rpQueuedReplot);
    }
}

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
    const auto& axisColors = axisPaletteFor(this);
    styleAxis(ax, axisColors[idx % axisColors.size()]);
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
    const bool dark = palette().color(QPalette::Base).lightness() < 128;
    return axisBaseColorFor(dark, axisIndex);
}

QColor ScopePlot::axisBaseColorFor(bool dark, int axisIndex) {
    if (axisIndex < 0) axisIndex = 0;
    const auto& axisColors = dark ? kAxisPaletteDark : kAxisPaletteLight;
    return axisColors[axisIndex % axisColors.size()];
}

QColor ScopePlot::deriveChannelColor(int axisIndex, int channelIndexOnAxis) const {
    const bool dark = palette().color(QPalette::Base).lightness() < 128;
    return deriveChannelColorFor(dark, axisIndex, channelIndexOnAxis);
}

QColor ScopePlot::deriveChannelColorFor(bool dark, int axisIndex,
                                        int channelIndexOnAxis) {
    // The first channel on each axis renders in the axis's exact base colour
    // (so the trace and the axis label match perfectly).
    const QColor base = axisBaseColorFor(dark, axisIndex);
    if (channelIndexOnAxis == 0) return base;

    int h, s, v, a;
    base.getHsv(&h, &s, &v, &a);

    // Neutral base (the default Y1): shades of gray are indistinguishable,
    // so further channels take distinct hues from the axis palette instead
    // (skipping its neutral first entry). This is the common single-axis,
    // many-channels case.
    if (s < 40) {
        const auto& pal = dark ? kAxisPaletteDark : kAxisPaletteLight;
        const int n = static_cast<int>(pal.size()) - 1;
        return pal[1 + ((channelIndexOnAxis - 1) % n)];
    }

    // Coloured axis: subsequent channels share the hue but get distinct
    // shades by adjusting saturation + value.
    const int idx = ((channelIndexOnAxis % 6) + 6) % 6;
    if (idx == 0) return base;
    static const int kDeltaSat[6] = {  0,  +20,  -30,  +40,  -50,  +60};
    static const int kDeltaVal[6] = {  0,  -40,  +40,  -70,  +70,  -90};
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

void ScopePlot::saveImageDialog() {
    const QString fPng = "PNG image (*.png)";
    const QString fSvg = "SVG vector image (*.svg)";
    const QString fPdf = "PDF document (*.pdf)";
    QFileDialog dlg(this, "Save chart image");
    dlg.setAcceptMode(QFileDialog::AcceptSave);
#ifdef SCOPE_HAVE_QTSVG
    dlg.setNameFilters({fPng, fSvg, fPdf});
#else
    dlg.setNameFilters({fPng, fPdf});
#endif
    dlg.setDefaultSuffix("png");
    connect(&dlg, &QFileDialog::filterSelected, &dlg, [&](const QString& f) {
        dlg.setDefaultSuffix(f == fSvg ? "svg" : f == fPdf ? "pdf" : "png");
    });
    if (dlg.exec() != QDialog::Accepted) return;
    const auto sel = dlg.selectedFiles();
    if (sel.isEmpty()) return;
    QString path = sel.first();

    // The extension wins; a missing one falls back to the chosen filter.
    QString fmt;
    if      (path.endsWith(".svg", Qt::CaseInsensitive)) fmt = "svg";
    else if (path.endsWith(".pdf", Qt::CaseInsensitive)) fmt = "pdf";
    else if (path.endsWith(".png", Qt::CaseInsensitive)) fmt = "png";
    else {
        const QString f = dlg.selectedNameFilter();
        fmt = (f == fSvg) ? "svg" : (f == fPdf) ? "pdf" : "png";
        path += "." + fmt;
    }

    if (!saveImage(path)) {
        QMessageBox::critical(this, "Save failed",
                              QString("Couldn't write %1").arg(path));
    }
}

bool ScopePlot::svgExportSupported() {
#ifdef SCOPE_HAVE_QTSVG
    return true;
#else
    return false;
#endif
}

bool ScopePlot::saveImage(const QString& path) {
    if (path.endsWith(".png", Qt::CaseInsensitive)) {
        return impl_->plot->savePng(path);
    }
    if (path.endsWith(".pdf", Qt::CaseInsensitive)) {
        // Vector output — stays sharp at any zoom level in the viewer.
        return impl_->plot->savePdf(path, 0, 0, QCP::epAllowCosmetic,
                                    "ScopeAnalyser", QString());
    }
    if (path.endsWith(".svg", Qt::CaseInsensitive)) {
#ifdef SCOPE_HAVE_QTSVG
        QSvgGenerator gen;
        gen.setFileName(path);
        const QSize sz = impl_->plot->size();
        gen.setSize(sz);
        gen.setViewBox(QRect(QPoint(0, 0), sz));
        gen.setTitle("ScopeAnalyser chart");
        QCPPainter painter;
        if (!painter.begin(&gen)) return false;
        impl_->plot->toPainter(&painter, sz.width(), sz.height());
        painter.end();
        return true;
#else
        return false;
#endif
    }
    return false;
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
        // findBegin gives the first sample at/right of the cursor; take the
        // nearer of it and its left neighbour so the read-out shows the
        // closest sample, not the next one along.
        auto it = g->data()->findBegin(xCoord, /*expandedRange=*/false);
        if (it != g->data()->constBegin()) {
            auto prev = it - 1;
            if (it == g->data()->constEnd()
                || std::abs(prev->key - xCoord) <= std::abs(it->key - xCoord)) {
                it = prev;
            }
        }
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
    // Arrow up/down state, from anywhere in the application: the wheel needs
    // to know whether an arrow is being held. Never consumed — this only
    // watches. Auto-repeat releases are ignored; the platforms that send them
    // pair each one with a fresh press, so the key is still down.
    if (ev->type() == QEvent::KeyRelease) {
        auto* k = static_cast<QKeyEvent*>(ev);
        if (!k->isAutoRepeat() && isArrowKey(k->key())) impl_->arrowDown = false;
    } else if (ev->type() == QEvent::WindowDeactivate) {
        impl_->arrowDown = false;       // released over some other window
    }

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
            //  0. An arrow key held     → move the view along that arrow
            //                             instead of zooming: the notch
            //                             carries the step (backwards goes
            //                             back), and feeds the same ramp the
            //                             held key does, so each notch covers
            //                             more than the one before.
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
            if (impl_->arrowHeld()) {
                const QPointF dir = impl_->keyPanDir;
                const double  k   = impl_->nextKeyPanBoost(dir);
                const double  d   = kPanFraction * k * notches;
                panBy(dir.x() * d, dir.y() * d);
            } else if (m & Qt::ControlModifier) {
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
            break;
        }
        default:
            break;
    }
    return false;
}

}  // namespace scope::plot
