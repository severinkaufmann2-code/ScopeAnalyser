#include "AnalyserPlot.h"

#include "AddChannelDialog.h"
#include "scope/converter/CsvWriter.h"
#include "scope/converter/SaveChartDialog.h"

#include "scope/analyser/FormulaEngine.h"
#include "scope/plot/ScopePlot.h"
#include "scope/plot/PlotLayout.h"
#include "scope/converter/SignalIO.h"
#include "scope/converter/HtmlExport.h"
#include "scope/converter/BusyRunner.h"
#include "scope/style/StyleKit.h"

#include <qcustomplot.h>

#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextStream>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <limits>
#include <utility>

namespace scope::analyser::ui {

namespace {
enum Col { ColVis = 0, ColName, ColAxis, ColCount };
}

AnalyserPlot::AnalyserPlot(scope::core::SignalStore& store,
                           FormulaEngine&            engine,
                           QWidget*                  parent)
    : QWidget(parent), store_(store), engine_(engine) {
    scope_ = new scope::plot::ScopePlot(this);
    scope_->plot()->xAxis->setLabel("t [s]");

    table_ = new QTableWidget(0, ColCount, this);
    table_->setHorizontalHeaderLabels({"", "Channel", "Axis"});
    table_->horizontalHeader()->setSectionResizeMode(ColName, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(ColVis,  QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(ColAxis, QHeaderView::ResizeToContents);
    table_->verticalHeader()->setVisible(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->setShowGrid(false);
    style::installEmptyHint(table_,
        "No channels in this view.\n\nRecord a signal, open a chart, or "
        "add a formula channel with “＋”.");

    // ---- Top action bar: file / layout / redraw ----------------------
    auto* actionBar = new QWidget(this);
    style::applyToolbarStrip(actionBar);
    auto* ab = new QHBoxLayout(actionBar);
    ab->setContentsMargins(8, 4, 8, 4);
    ab->setSpacing(4);
    auto barBtn = [&](style::Glyph glyph, const QString& text,
                      const QString& tip) {
        auto* b = new QToolButton(actionBar);
        b->setIcon(style::icon(glyph));
        b->setText(text);
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        b->setToolTip(tip);
        ab->addWidget(b);
        return b;
    };
    auto* openChartBtn = barBtn(style::Glyph::FolderOpen, "Open chart…",
        "Open .h5 / .mf4 / .csv / .txt / .json — the channels land in the "
        "shared signal store. An embedded plot layout is restored automatically.");
    auto* saveChartBtn = barBtn(style::Glyph::Save, "Save chart…",
        "Export channels to HDF5 / MDF4 / CSV / JSON / HTML with per-domain "
        "filters and time range; the current plot layout is embedded. HTML "
        "writes a self-contained interactive chart that opens in any browser "
        "offline and re-opens here.");
    ab->addSpacing(12);
    auto* saveLayoutBtn = barBtn(style::Glyph::Save, "Save layout…",
        "Save Y axes, channel→axis assignments, and view mode to a "
        ".scoana file (no signal data).");
    auto* loadLayoutBtn = barBtn(style::Glyph::FolderOpen, "Load layout…",
        "Restore Y axes, channel→axis assignments, and view mode from a "
        ".scoana file.");
    ab->addSpacing(12);
    // Undo / redo over the plot document (channels, axes, assignments, view).
    undoBtn_ = new QToolButton(actionBar);
    undoBtn_->setIcon(style::icon(style::Glyph::Undo));
    undoBtn_->setToolTip("Undo the last change to channels, axes, assignments, "
                         "or view mode (Ctrl+Z).");
    undoBtn_->setShortcut(QKeySequence::Undo);
    undoBtn_->setEnabled(false);
    redoBtn_ = new QToolButton(actionBar);
    redoBtn_->setIcon(style::icon(style::Glyph::Redo));
    redoBtn_->setToolTip("Redo the change undone last (Ctrl+Y / Ctrl+Shift+Z).");
    redoBtn_->setShortcut(QKeySequence::Redo);
    redoBtn_->setEnabled(false);
    ab->addWidget(undoBtn_);
    ab->addWidget(redoBtn_);
    ab->addStretch();
    auto* redrawBtn = new QPushButton(
        style::icon(style::Glyph::Refresh, Qt::white), "Redraw", actionBar);
    redrawBtn->setProperty("accent", true);
    redrawBtn->setToolTip(
        "Re-evaluate every formula channel against the current source data, "
        "then refresh the chart. The chart deliberately stays frozen while "
        "recording until you redraw.");
    ab->addWidget(redrawBtn);

    // ---- Left panel: view / channels / axes ---------------------------
    viewCombo_ = new QComboBox(this);
    viewCombo_->addItem("Time  (t [s])");
    viewCombo_->addItem("Frequency  (f [Hz])");
    viewCombo_->addItem("XY  (channel vs channel)");
    viewCombo_->setToolTip(
        "Time: show only time-domain channels (recorded / imported / "
        "derived from time-domain).\n"
        "Frequency: show only frequency-domain channels (FFT).\n"
        "XY: pick one channel as the X axis; Y candidates are filtered "
        "to channels with the same domain as X.");

    // X-channel selector for XY view. Sits directly under the View combo
    // and is hidden in Time / Frequency mode.
    xyXRow_ = new QWidget(this);
    auto* xyXLayout = new QHBoxLayout(xyXRow_);
    xyXLayout->setContentsMargins(0, 0, 0, 0);
    xyXLayout->addWidget(new QLabel("X axis:", xyXRow_));
    xyXCombo_ = new QComboBox(xyXRow_);
    xyXLayout->addWidget(xyXCombo_, /*stretch=*/1);
    xyXRow_->setVisible(false);

    auto smallBtn = [this](style::Glyph glyph, const QString& tip) {
        auto* b = new QToolButton(this);
        b->setIcon(style::icon(glyph));
        b->setToolTip(tip);
        return b;
    };
    auto* addChBtn    = smallBtn(style::Glyph::Plus,
        "Add a channel — a formula over existing channels, or a copy "
        "(autocomplete with Tab).");
    auto* editChBtn   = smallBtn(style::Glyph::Pencil,
        "Edit the selected formula channel (double-click also works).");
    auto* removeChBtn = smallBtn(style::Glyph::Minus,
        "Remove the selected channel(s) from the store. Ctrl/Shift-click "
        "to select several rows and remove them all at once.");

    auto* chHeader = new QHBoxLayout();
    chHeader->setSpacing(2);
    chHeader->addWidget(style::sectionLabel("Channels", this));
    chHeader->addStretch();
    chHeader->addWidget(addChBtn);
    chHeader->addWidget(editChBtn);
    chHeader->addWidget(removeChBtn);

    // Y axes are managed in the plot toolbar (Y+ / Y−) and per-axis via
    // right-click on the axis itself.
    auto* leftPanel = new QWidget(this);
    leftPanel->setMinimumWidth(250);
    auto* leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(8, 6, 4, 8);
    leftLayout->setSpacing(6);
    leftLayout->addWidget(style::sectionLabel("View", leftPanel));
    leftLayout->addWidget(viewCombo_);
    leftLayout->addWidget(xyXRow_);
    leftLayout->addSpacing(4);
    leftLayout->addLayout(chHeader);
    leftLayout->addWidget(table_, /*stretch=*/1);

    auto* split = new QSplitter(Qt::Horizontal, this);
    split->addWidget(leftPanel);
    split->addWidget(scope_);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({320, 1100});

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(actionBar);
    root->addWidget(split, /*stretch=*/1);

    connect(redrawBtn,     &QPushButton::clicked, this, &AnalyserPlot::redrawAll);
    connect(saveLayoutBtn, &QToolButton::clicked, this, &AnalyserPlot::saveLayoutDialog);
    connect(loadLayoutBtn, &QToolButton::clicked, this, &AnalyserPlot::loadLayoutDialog);
    connect(addChBtn,      &QToolButton::clicked, this, &AnalyserPlot::addChannelDialog);
    connect(editChBtn,     &QToolButton::clicked, this, &AnalyserPlot::editChannelDialog);
    connect(saveChartBtn,  &QToolButton::clicked, this, &AnalyserPlot::saveChartDialog);
    connect(openChartBtn,  &QToolButton::clicked, this, &AnalyserPlot::openChartDialog);
    connect(viewCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx){
        const auto oldDomain = activeDomain();
        viewMode_ = (idx == 2) ? ViewMode::XY
                  : (idx == 1) ? ViewMode::Frequency
                               : ViewMode::Time;
        xyXRow_->setVisible(viewMode_ == ViewMode::XY);
        if (viewMode_ == ViewMode::XY) {
            rebuildXyXCombo();
            xyChannel_ = xyXCombo_->currentData().toString();
        }
        const auto newDomain = activeDomain();

        // Plottable type differs across modes (Graph vs Curve), so the
        // map can't just be repurposed.
        clearAllPlottables();

        // If the new view lives in a different domain than the old one,
        // swap out the Y-axis configuration too: snapshot the current
        // axes / row state into the old domain, then restore the new
        // domain's previously-saved set.
        if (oldDomain != newDomain) {
            snapshotIntoDomain(oldDomain);
            applyFromDomain(newDomain);
        }

        // Reset the X axis label for non-XY; XY sets it from the chosen
        // channel inside redrawForActiveChannels.
        if (viewMode_ == ViewMode::Time) {
            scope_->plot()->xAxis->setLabel("t [s]");
        } else if (viewMode_ == ViewMode::Frequency) {
            scope_->plot()->xAxis->setLabel("f [Hz]");
        }
        rebuildTable();
        hasDrawnYet_ = false;
        redrawForActiveChannels();
        scheduleCommit();   // view-mode change is a document edit
    });
    connect(xyXCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int){
        const auto oldDomain = activeDomain();
        xyChannel_ = xyXCombo_->currentData().toString();
        const auto newDomain = activeDomain();

        clearAllPlottables();
        if (oldDomain != newDomain) {
            snapshotIntoDomain(oldDomain);
            applyFromDomain(newDomain);
        }
        rebuildTable();
        hasDrawnYet_ = false;
        redrawForActiveChannels();
        scheduleCommit();   // XY X-channel change is a document edit
    });
    connect(table_, &QTableWidget::cellDoubleClicked, this,
            [this](int, int){ editChannelDialog(); });
    // Click on a plottable in the chart → select its row in the channels
    // table. Works for both QCPGraph (Time / Frequency) and QCPCurve (XY).
    connect(scope_->plot(), &QCustomPlot::plottableClick, this,
            [this](QCPAbstractPlottable* p, int /*dataIndex*/, QMouseEvent*){
        if (!p) return;
        QString clickedName;
        for (auto it = plotted_.constBegin(); it != plotted_.constEnd(); ++it) {
            if (it.value() == p) { clickedName = it.key(); break; }
        }
        if (clickedName.isEmpty()) return;
        for (int r = 0; r < table_->rowCount(); ++r) {
            if (table_->item(r, ColName)->text() == clickedName) {
                table_->selectRow(r);
                table_->scrollToItem(table_->item(r, ColName));
                return;
            }
        }
    });
    connect(removeChBtn,  &QToolButton::clicked, this, [this]{
        // Collect the names up front: removing a channel rebuilds the table,
        // so we can't read rows mid-loop.
        QStringList names;
        for (const auto& idx : table_->selectionModel()->selectedRows()) {
            if (auto* it = table_->item(idx.row(), ColName)) names << it->text();
        }
        if (names.isEmpty()) return;
        const QString title  = names.size() == 1 ? "Remove channel?"
                                                 : "Remove channels?";
        const QString prompt = names.size() == 1
            ? QString("Remove channel '%1' from the store?").arg(names.first())
            : QString("Remove these %1 channels from the store?\n\n%2")
                  .arg(names.size()).arg(names.join("\n"));
        const auto resp = QMessageBox::question(
            this, title, prompt, QMessageBox::Yes | QMessageBox::Cancel);
        if (resp == QMessageBox::Yes)
            for (const auto& name : names) store_.remove(name);
    });

    // A new/replaced source fires channelAdded, which may feed derived
    // channels. Coalesce a burst (e.g. opening a multi-channel file) into a
    // single recompute via a short single-shot timer.
    recomputeTimer_ = new QTimer(this);
    recomputeTimer_->setSingleShot(true);
    recomputeTimer_->setInterval(0);
    connect(recomputeTimer_, &QTimer::timeout, this, [this]{
        engine_.recomputeAll();   // emits channelAdded → redraw via onChannelAdded
    });

    connect(&store_, &scope::core::SignalStore::channelAdded,
            this, &AnalyserPlot::onChannelAdded);
    connect(&store_, &scope::core::SignalStore::channelRemoved,
            this, &AnalyserPlot::onChannelRemoved);
    // channelDataChanged (a growing recording) is intentionally NOT wired to
    // a redraw: the chart stays frozen while recording / streaming and only
    // refreshes on an explicit Redraw. See redrawAll().
    // Axes are added/removed/renamed from the plot's toolbar (Y+ / Y−) and
    // its axis context menu — refresh the per-row combos and re-derive the
    // colours, which depend on each channel's axis index.
    connect(scope_, &scope::plot::ScopePlot::yAxesChanged,
            this, [this]{
        rebuildAxisCombos();
        recolorChannels();
        scheduleCommit();   // axes added / removed / renamed is a document edit
    });
    // Light/dark flips change the axis base palette → re-derive trace pens
    // and table swatches.
    connect(scope_, &scope::plot::ScopePlot::themePaletteChanged,
            this, [this]{ recolorChannels(); });

    rebuildXyXCombo();
    rebuildTable();

    // ---- Undo / redo wiring ------------------------------------------
    commitTimer_ = new QTimer(this);
    commitTimer_->setSingleShot(true);
    commitTimer_->setInterval(0);
    undo_ = std::make_unique<scope::core::UndoStack<UndoSnap>>(
        [this]{ return captureState(); },
        [this](const UndoSnap& s){ restoreState(s); },
        [this]{ refreshUndoButtons(); });
    connect(commitTimer_, &QTimer::timeout, this, [this]{ undo_->commit(); });
    connect(undoBtn_, &QToolButton::clicked, this, [this]{ undo_->undo(); });
    connect(redoBtn_, &QToolButton::clicked, this, [this]{ undo_->redo(); });

    // Seed the baseline. Most commit points are reached through scheduleCommit
    // calls added to the store/axis/view handlers below; this captures the
    // starting (usually empty) state so the first edit has something to undo to.
    undo_->reset();
    refreshUndoButtons();
}

void AnalyserPlot::scheduleRecompute() {
    // Don't reschedule for the channelAdded emissions that recomputeAll
    // itself produces — that would spin. start() restarts (coalesces) an
    // already-pending timer.
    if (recomputeTimer_ && !engine_.isRecomputing())
        recomputeTimer_->start();
}

AnalyserPlot::UndoSnap AnalyserPlot::captureState() {
    UndoSnap snap;
    snap.layout = currentLayout();
    for (const auto& name : store_.channelNames())
        if (auto s = store_.get(name))
            snap.channels.append({name, std::move(s)});
    return snap;
}

void AnalyserPlot::restoreState(const UndoSnap& snap) {
    // 1. Reconcile the store to exactly the snapshot's channel set. Removing
    //    extras and re-adding the held shared_ptrs is cheap (no data copy) and
    //    never re-reads a file. store_.add replaces an existing channel.
    QSet<QString> want;
    want.reserve(snap.channels.size());
    for (const auto& [name, sig] : snap.channels) want.insert(name);
    for (const auto& n : store_.channelNames())
        if (!want.contains(n)) store_.remove(n);
    for (const auto& [name, sig] : snap.channels) store_.add(sig);

    // 2. Re-apply the layout. Recalculate keeps formula channels editable and
    //    recomputes them from the (now-restored) sources — deterministic, so
    //    the values match the snapshot. Replace drops any axes added since.
    applyLayout(snap.layout, FormulaImport::Recalculate, LayoutApply::Replace);
}

void AnalyserPlot::scheduleCommit() {
    // Gate out non-edits: a restore in progress, or a formula-recompute pass
    // (its channelAdded burst isn't a user edit). The coalescing timer folds a
    // burst of real edits into a single history entry.
    if (!undo_ || undo_->applying() || engine_.isRecomputing()) return;
    commitTimer_->start();
}

void AnalyserPlot::refreshUndoButtons() {
    if (!undo_) return;
    undoBtn_->setEnabled(undo_->canUndo());
    redoBtn_->setEnabled(undo_->canRedo());
}

int AnalyserPlot::pickAxisForUnit(const QString& unit) const {
    if (unit.isEmpty()) return 0;
    for (int i = 0; i < scope_->yAxisCount(); ++i) {
        auto* ax = scope_->yAxis(i);
        if (ax && ax->label().contains(unit, Qt::CaseInsensitive)) return i;
    }
    return 0;
}

int AnalyserPlot::axisIndexForRow(int row) const {
    if (row < 0 || row >= table_->rowCount()) return 0;
    auto* co = qobject_cast<QComboBox*>(table_->cellWidget(row, ColAxis));
    return co ? co->currentIndex() : 0;
}

void AnalyserPlot::setAxisIndexForRow(int row, int axisIndex) {
    if (row < 0 || row >= table_->rowCount()) return;
    auto* co = qobject_cast<QComboBox*>(table_->cellWidget(row, ColAxis));
    if (!co) return;
    if (axisIndex < 0) axisIndex = 0;
    if (axisIndex >= co->count()) axisIndex = co->count() - 1;
    co->setCurrentIndex(axisIndex);
}

QSet<QString> AnalyserPlot::activeChannels() const {
    QSet<QString> out;
    for (int r = 0; r < table_->rowCount(); ++r) {
        if (auto* cb = qobject_cast<QCheckBox*>(table_->cellWidget(r, ColVis))) {
            if (cb->isChecked()) out.insert(table_->item(r, ColName)->text());
        }
    }
    return out;
}

void AnalyserPlot::rebuildTable() {
    // savedRowState_ is kept continuously up-to-date by the per-row
    // handlers (onAxisComboChanged / onVisibilityChanged) and by the
    // per-domain snapshot/restore in the View / XY-X combo handlers.
    // The old "snapshot current widgets into the sticky cache" prelude
    // that used to live here is now redundant — and in the load path
    // it actively clobbered the layout's restored axis assignments by
    // overwriting them with stale widget values. So just trust the
    // sticky cache and rebuild.
    auto& prev = savedRowState_;

    table_->setRowCount(0);
    const bool firstTime = prev.isEmpty();
    const auto names = store_.channelNames();

    // XY mode: Y candidates share the X channel's domain. Mixing time
    // and frequency axes on the same XY plot doesn't make sense, and
    // the user agreed in the design discussion that strictly matching
    // domains is the right rule.
    scope::core::Signal::Domain xyDomain = scope::core::Signal::Domain::Time;
    bool xyHasX = false;
    if (viewMode_ == ViewMode::XY && !xyChannel_.isEmpty()) {
        if (auto xs = store_.get(xyChannel_)) {
            xyDomain = xs->meta().domain;
            xyHasX = true;
        }
    }

    for (const auto& name : names) {
        auto s = store_.get(name);
        if (!s) continue;
        switch (viewMode_) {
            case ViewMode::Time:
                if (s->meta().domain != scope::core::Signal::Domain::Time) continue;
                break;
            case ViewMode::Frequency:
                if (s->meta().domain != scope::core::Signal::Domain::Frequency) continue;
                break;
            case ViewMode::XY:
                // No X picked yet → empty Y list; user picks X first.
                if (!xyHasX) continue;
                // Skip the X channel itself — it appears in the X combo
                // above the table.
                if (name == xyChannel_) continue;
                // Same-domain filter.
                if (s->meta().domain != xyDomain) continue;
                break;
        }
        const int r = table_->rowCount();
        table_->insertRow(r);

        auto* vis = new QCheckBox();
        bool defaultVis = true;
        int defaultAxis = 0;
        if (prev.contains(name)) {
            defaultVis  = prev[name].first;
            defaultAxis = prev[name].second;
        } else if (pendingAssignments_.contains(name)) {
            defaultAxis = pendingAssignments_.take(name);
        } else {
            auto s = store_.get(name);
            if (s) defaultAxis = pickAxisForUnit(s->meta().unit);
        }
        vis->setChecked(firstTime || defaultVis);
        table_->setCellWidget(r, ColVis, vis);
        connect(vis, &QCheckBox::toggled, this, [this, r](bool){ onVisibilityChanged(r); });

        auto* nameItem = new QTableWidgetItem(name);
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
        table_->setItem(r, ColName, nameItem);

        auto* combo = new QComboBox();
        for (int a = 0; a < scope_->yAxisCount(); ++a) {
            combo->addItem(scope_->yAxis(a)->label());
        }
        combo->setCurrentIndex(std::min(defaultAxis, combo->count() - 1));
        table_->setCellWidget(r, ColAxis, combo);
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this, r](int idx){ onAxisComboChanged(r, idx); });
    }
    if (firstTime) redrawForActiveChannels();
}

void AnalyserPlot::rebuildAxisCombos() {
    for (int r = 0; r < table_->rowCount(); ++r) {
        auto* combo = qobject_cast<QComboBox*>(table_->cellWidget(r, ColAxis));
        if (!combo) continue;
        const int cur = combo->currentIndex();
        QSignalBlocker blocker(combo);
        combo->clear();
        for (int a = 0; a < scope_->yAxisCount(); ++a)
            combo->addItem(scope_->yAxis(a)->label());
        combo->setCurrentIndex(std::min(std::max(cur, 0), combo->count() - 1));
    }
}

void AnalyserPlot::onAxisComboChanged(int row, int axisIndex) {
    if (row < 0 || row >= table_->rowCount()) return;
    const QString name = table_->item(row, ColName)->text();
    // Keep savedRowState_ in lockstep with the live combo so the next
    // domain-snapshot picks up the change. Without this the user's axis
    // pick lives only in the widget; the next View-combo flip would
    // snapshot a stale value into stateByDomain_ and the assignment
    // would silently disappear when the user came back.
    auto& st = savedRowState_[name];
    st.second = axisIndex;
    auto* p = plotted_.value(name, nullptr);
    if (p) p->setValueAxis(scope_->yAxis(axisIndex));
    recolorChannels();
    scope_->plot()->replot(QCustomPlot::rpQueuedReplot);
    scheduleCommit();   // channel→axis reassignment is a document edit
}

void AnalyserPlot::onVisibilityChanged(int row) {
    // Mirror the visibility checkbox into savedRowState_ — same reason
    // as onAxisComboChanged.
    if (row >= 0 && row < table_->rowCount()) {
        const QString name = table_->item(row, ColName)->text();
        if (auto* cb = qobject_cast<QCheckBox*>(table_->cellWidget(row, ColVis))) {
            savedRowState_[name].first = cb->isChecked();
        }
    }
    redrawForActiveChannels();
}

void AnalyserPlot::recolorChannels() {
    // For each axis, the Nth ROW assigned to it gets the Nth shade of that
    // axis's base colour — counted over all rows, visible or not, so
    // toggling a channel's visibility never re-colours the other traces.
    // Hidden rows keep their reserved colour as a hollow swatch.
    QHash<int, int> perAxisCounter;
    for (int r = 0; r < table_->rowCount(); ++r) {
        auto* nameItem = table_->item(r, ColName);
        const QString name = nameItem->text();
        const int axisIdx = axisIndexForRow(r);
        const int onAxis  = perAxisCounter[axisIdx]++;
        const QColor c = scope_->deriveChannelColor(axisIdx, onAxis);
        if (plotted_.contains(name)) {
            plotted_[name]->setPen(QPen(c));
            nameItem->setIcon(style::colorSwatch(c));
        } else {
            nameItem->setIcon(style::hollowSwatch(c));
        }
    }
}

void AnalyserPlot::onChannelAdded(QString /*name*/) {
    rebuildXyXCombo();
    rebuildTable();
    redrawForActiveChannels();
    // A new / replaced source may feed derived channels — recompute them
    // (coalesced; a no-op while recomputeAll is the one doing the adding).
    scheduleRecompute();
    // A user-driven add (open chart, add formula) is a document edit. Adds
    // that come from a restore or from the recompute pass are gated out inside
    // scheduleCommit().
    scheduleCommit();
}
void AnalyserPlot::onChannelRemoved(QString name) {
    if (plotted_.contains(name)) {
        scope_->plot()->removePlottable(plotted_[name]);
        plotted_.remove(name);
    }
    if (name == xyChannel_) {
        xyChannel_.clear();
        rebuildXyXCombo();
    } else {
        rebuildXyXCombo();
    }
    rebuildTable();
    redrawForActiveChannels();
    scheduleCommit();   // a removed channel is a document edit
}

void AnalyserPlot::clearAllPlottables() {
    auto* plot = scope_->plot();
    for (auto it = plotted_.begin(); it != plotted_.end(); ++it) {
        plot->removePlottable(it.value());
    }
    plotted_.clear();
}

void AnalyserPlot::rebuildXyXCombo() {
    if (!xyXCombo_) return;
    QSignalBlocker b(xyXCombo_);
    const QString prev = xyXCombo_->currentData().toString().isEmpty()
                            ? xyChannel_
                            : xyXCombo_->currentData().toString();
    xyXCombo_->clear();
    xyXCombo_->addItem("— (choose) —", QString());
    for (const auto& n : store_.channelNames()) {
        auto s = store_.get(n);
        if (!s) continue;
        const QString unitTag =
            s->meta().unit.isEmpty() ? QString()
                                     : QString(" [%1]").arg(s->meta().unit);
        const QString domainTag =
            s->meta().domain == scope::core::Signal::Domain::Frequency
                ? QString("  (freq)") : QString();
        xyXCombo_->addItem(n + unitTag + domainTag, n);
    }
    const int idx = prev.isEmpty() ? 0 : xyXCombo_->findData(prev);
    xyXCombo_->setCurrentIndex(idx >= 0 ? idx : 0);
    xyChannel_ = xyXCombo_->currentData().toString();
}

// ---- Per-domain Y-axis machinery --------------------------------------

scope::core::Signal::Domain AnalyserPlot::activeDomain() const {
    using Domain = scope::core::Signal::Domain;
    switch (viewMode_) {
        case ViewMode::Time:      return Domain::Time;
        case ViewMode::Frequency: return Domain::Frequency;
        case ViewMode::XY:
            if (auto s = store_.get(xyChannel_)) return s->meta().domain;
            return Domain::Time;
    }
    return Domain::Time;
}

void AnalyserPlot::snapshotIntoDomain(scope::core::Signal::Domain d) {
    PerDomainState& st = stateByDomain_[static_cast<int>(d)];

    // 1. Y axes — pull label / side / current range from ScopePlot.
    st.axes.clear();
    for (int i = 0; i < scope_->yAxisCount(); ++i) {
        auto* ax = scope_->yAxis(i);
        if (!ax) continue;
        scope::plot::PlotLayoutAxis a;
        a.label    = ax->label();
        a.side     = (ax->axisType() == QCPAxis::atRight) ? "right" : "left";
        a.hasRange = true;
        a.min      = ax->range().lower;
        a.max      = ax->range().upper;
        st.axes.append(std::move(a));
    }

    // 2. Per-row vis+axis snapshot. Pull from the LIVE table widgets,
    //    not just savedRowState_ — if the user toggled something
    //    between the last rebuildTable and now, savedRowState_ would
    //    still be stale and we'd persist the wrong assignment.
    for (int r = 0; r < table_->rowCount(); ++r) {
        const QString name = table_->item(r, ColName)
                                 ? table_->item(r, ColName)->text() : QString();
        if (name.isEmpty()) continue;
        bool vis = true; int axis = 0;
        if (auto* cb = qobject_cast<QCheckBox*>(table_->cellWidget(r, ColVis)))
            vis = cb->isChecked();
        if (auto* co = qobject_cast<QComboBox*>(table_->cellWidget(r, ColAxis)))
            axis = co->currentIndex();
        savedRowState_[name] = {vis, axis};
    }
    st.rowState = savedRowState_;
}

void AnalyserPlot::applyFromDomain(scope::core::Signal::Domain d) {
    PerDomainState& st = stateByDomain_[static_cast<int>(d)];

    // 1. Strip the plot down to a single Y0 axis. clearAllPlottables()
    //    should run first so we don't try to remove an axis that has
    //    plottables bound to it.
    while (scope_->yAxisCount() > 1) {
        QString err;
        if (!scope_->removeYAxis(scope_->yAxisCount() - 1, &err)) {
            // If we can't remove, just stop — leftover axes will be
            // hidden by rebuildTable's filter rather than removed.
            break;
        }
    }

    // 2. Restore the saved axis set. If the per-domain state is empty
    //    (first time entering this domain), leave the current Y0
    //    in place as a sensible default.
    if (!st.axes.isEmpty()) {
        const auto& first = st.axes[0];
        if (auto* y0 = scope_->yAxis(0)) {
            y0->setLabel(first.label);
            if (first.hasRange) y0->setRange(first.min, first.max);
        }
        for (int i = 1; i < st.axes.size(); ++i) {
            const auto& a = st.axes[i];
            const Qt::Alignment side =
                (a.side == "right") ? Qt::AlignRight : Qt::AlignLeft;
            const int idx = scope_->addYAxis(a.label, side);
            if (a.hasRange) scope_->yAxis(idx)->setRange(a.min, a.max);
        }
    }

    // 3. Restore per-row sticky state. rebuildTable reads from this map.
    savedRowState_ = st.rowState;

    rebuildAxisCombos();
}

void AnalyserPlot::redrawAll() {
    // The explicit, user-driven refresh. This is the single place that
    // recomputes derived/formula channels against the current source data
    // before redrawing — tab switches and live recording deliberately don't,
    // so this button is how the user pulls the chart up to date.
    QStringList errs;
    engine_.recomputeAll(&errs);
    if (!errs.isEmpty()) {
        QMessageBox::warning(this, "Some formulas failed",
            "Couldn't re-evaluate (kept any saved data as-is):\n"
                + errs.join("\n"));
    }
    redrawForActiveChannels();
}

namespace {

// Linear interpolation of Y at time t, given Y's monotonic timestamp
// array. Returns NaN outside Y's range so the curve leaves a gap at that
// X sample — clamping would draw a flat line of fabricated points where
// no Y data exists.
double interpAt(const scope::core::TimestampNs* ts,
                const std::vector<double>& vs,
                std::size_t n,
                scope::core::TimestampNs t) {
    if (n == 0) return std::numeric_limits<double>::quiet_NaN();
    if (t < ts[0] || t > ts[n-1])
        return std::numeric_limits<double>::quiet_NaN();
    if (t == ts[0])    return vs.front();
    if (t == ts[n-1])  return vs.back();
    auto* lo = std::lower_bound(ts, ts + n, t);
    const std::size_t hi = lo - ts;
    if (hi == 0) return vs[0];
    const std::size_t loIdx = hi - 1;
    const double dt = static_cast<double>(ts[hi] - ts[loIdx]);
    if (dt == 0) return vs[loIdx];
    const double frac = static_cast<double>(t - ts[loIdx]) / dt;
    return vs[loIdx] + frac * (vs[hi] - vs[loIdx]);
}

}  // namespace

void AnalyserPlot::redrawForActiveChannels() {
    const auto active = activeChannels();
    auto* plot = scope_->plot();

    // Drop plottables whose rows are no longer active (visibility toggled
    // off, channel removed from current view).
    for (auto it = plotted_.begin(); it != plotted_.end(); ) {
        if (!active.contains(it.key())) {
            plot->removePlottable(it.value());
            it = plotted_.erase(it);
        } else { ++it; }
    }

    double xMin = std::numeric_limits<double>::infinity();
    double xMax = -std::numeric_limits<double>::infinity();

    if (viewMode_ == ViewMode::XY) {
        // ------------ XY: curves of one channel's values vs another -----
        auto xSig = xyChannel_.isEmpty() ? nullptr : store_.get(xyChannel_);
        if (xSig) {
            const QString unit = xSig->meta().unit;
            plot->xAxis->setLabel(
                unit.isEmpty() ? xyChannel_
                               : QString("%1 [%2]").arg(xyChannel_, unit));
        } else {
            plot->xAxis->setLabel("(choose an X channel)");
        }

        for (int r = 0; r < table_->rowCount(); ++r) {
            const QString name = table_->item(r, ColName)->text();
            if (!active.contains(name)) continue;
            if (!xSig) continue;
            auto ySig = store_.get(name);
            if (!ySig) continue;

            const int axisIndex = axisIndexForRow(r);

            // Wrong plottable type left over from a previous mode → drop.
            auto* existing = plotted_.value(name, nullptr);
            auto* curve = qobject_cast<QCPCurve*>(existing);
            if (existing && !curve) {
                plot->removePlottable(existing);
                plotted_.remove(name);
                existing = nullptr;
            }
            if (!curve) {
                curve = new QCPCurve(plot->xAxis, scope_->yAxis(axisIndex));
                curve->setName(name);
                plotted_[name] = curve;
            } else if (curve->valueAxis() != scope_->yAxis(axisIndex)) {
                curve->setValueAxis(scope_->yAxis(axisIndex));
            }

            // Pair X-signal values with Y-signal values. If both have
            // byte-identical timestamp arrays we can pair by index —
            // fast-path with zero per-sample work. Otherwise we linearly
            // interpolate Y onto X's grid.
            auto xView = xSig->snapshotForRead();
            auto yView = ySig->snapshotForRead();
            auto xVals = xSig->readAsDouble();
            auto yVals = ySig->readAsDouble();
            const bool sameGrid =
                xView.count == yView.count
                && (xView.count == 0
                    || std::memcmp(xView.timestamps, yView.timestamps,
                                   xView.count * sizeof(scope::core::TimestampNs)) == 0);
            QVector<double> xs, ys;
            xs.reserve(static_cast<int>(xView.count));
            ys.reserve(static_cast<int>(xView.count));
            for (std::size_t i = 0; i < xView.count; ++i) {
                const double xv = (i < xVals.size()) ? xVals[i] : 0.0;
                const double yv = sameGrid
                    ? ((i < yVals.size()) ? yVals[i] : 0.0)
                    : interpAt(yView.timestamps, yVals, yView.count,
                               xView.timestamps[i]);
                xs.push_back(xv);
                ys.push_back(yv);
                xMin = std::min(xMin, xv);
                xMax = std::max(xMax, xv);
            }
            curve->setData(xs, ys);
        }
    } else {
        // ------------ Time / Frequency: regular graphs ----------------
        double base = std::numeric_limits<double>::infinity();
        for (const auto& name : active) {
            auto sig = store_.get(name);
            if (!sig) continue;
            auto view = sig->snapshotForRead();
            if (view.count == 0) continue;
            double cand = view.timestamps[0] / 1e9;
            // Derived channels that drop leading samples (Slice, Gate)
            // remember their source's origin — anchor the axis there so the
            // segment stays at its true position (e.g. 30–50 s) even when
            // shown without the source. Frequency view: spectra carry the
            // source's TIME origin in the same field, which must not move
            // the Hz axis.
            if (viewMode_ == ViewMode::Time) {
                cand = std::min(cand, sig->displayOriginNs() / 1e9);
            }
            base = std::min(base, cand);
        }
        if (base == std::numeric_limits<double>::infinity()) base = 0;

        for (int r = 0; r < table_->rowCount(); ++r) {
            const QString name = table_->item(r, ColName)->text();
            if (!active.contains(name)) continue;
            auto sig = store_.get(name);
            if (!sig) continue;

            const int axisIndex = axisIndexForRow(r);

            auto* existing = plotted_.value(name, nullptr);
            auto* graph = qobject_cast<QCPGraph*>(existing);
            if (existing && !graph) {
                plot->removePlottable(existing);
                plotted_.remove(name);
                existing = nullptr;
            }
            if (!graph) {
                graph = plot->addGraph(plot->xAxis, scope_->yAxis(axisIndex));
                graph->setName(name);
                // Adaptive sampling is left at its QCustomPlot default
                // (true): one min/max line per pixel column for dense
                // data, essential for smooth pan/zoom on signals with
                // hundreds of thousands of samples.
                plotted_[name] = graph;
                scope_->applyLineDisplayModeTo(graph);
            } else if (graph->valueAxis() != scope_->yAxis(axisIndex)) {
                scope_->setGraphYAxis(graph, axisIndex);
            }

            auto view = sig->snapshotForRead();
            auto values = sig->readAsDouble();
            QVector<double> xs, ys;
            xs.reserve(static_cast<int>(view.count));
            ys.reserve(static_cast<int>(view.count));
            for (std::size_t i = 0; i < view.count; ++i) {
                const double x = view.timestamps[i] / 1e9 - base;
                xs.push_back(x);
                const double v = (i < values.size()) ? values[i] : 0.0;
                ys.push_back(v);
                xMin = std::min(xMin, x);
                xMax = std::max(xMax, x);
            }
            graph->setData(xs, ys, /*alreadySorted=*/true);
        }
    }

    recolorChannels();

    // The X/Y data is now on the graphs. By default we keep the user's
    // current view — toggling a channel's visibility shouldn't yank the
    // chart back to its full extent. Only refit when:
    //   - This is the first time anything is being drawn (otherwise the
    //     user would see a useless [0, 1] empty plot until they hit Fit).
    //   - rescaleAllYAxes() still respects per-axis autoFit, so axes
    //     the user manually zoomed stay where they are; axes that
    //     haven't been touched track new data as usual.
    const bool haveData = !active.isEmpty()
                       && xMin != std::numeric_limits<double>::infinity();
    if (!hasDrawnYet_ && haveData) {
        // First-ever draw: set X to the data range so the user isn't
        // stuck looking at [0, 1].
        plot->xAxis->setRange(xMin, xMax);
        hasDrawnYet_ = true;
    }
    // rescaleAllYAxes is gated on per-axis autoFit, so manually-zoomed
    // Y axes are preserved; untouched axes track new data.
    scope_->rescaleAllYAxes();
    plot->replot(QCustomPlot::rpQueuedReplot);
}

scope::plot::PlotLayout AnalyserPlot::currentLayout() {
    // Capture the live table state into the active domain so the
    // returned layout reflects what's on screen. snapshotIntoDomain
    // reads directly from the table widgets, so the active domain
    // gets the most recent edits; other domains were snapshot when
    // we last switched away from them.
    snapshotIntoDomain(activeDomain());

    scope::plot::PlotLayout layout;
    layout.viewMode  = (viewMode_ == ViewMode::XY)        ? "xy"
                     : (viewMode_ == ViewMode::Frequency) ? "frequency"
                                                          : "time";
    layout.xyChannel = xyChannel_;

    auto channelDomainKey = [](const std::shared_ptr<scope::core::Signal>& sig) {
        return sig->meta().domain == scope::core::Signal::Domain::Frequency
                   ? QString("frequency") : QString("time");
    };
    using Domain = scope::core::Signal::Domain;
    for (Domain d : {Domain::Time, Domain::Frequency}) {
        const QString key = (d == Domain::Frequency) ? "frequency" : "time";
        const auto& st = stateByDomain_.value(static_cast<int>(d));
        QList<scope::plot::PlotLayoutAxis> axes = st.axes;
        if (axes.isEmpty() && d == activeDomain()) {
            // Fallback — should never trigger because we just snapshot
            // the active domain, but kept defensive.
            for (int i = 0; i < scope_->yAxisCount(); ++i) {
                auto* ax = scope_->yAxis(i);
                if (!ax) continue;
                scope::plot::PlotLayoutAxis a;
                a.label    = ax->label();
                a.side     = (ax->axisType() == QCPAxis::atRight) ? "right" : "left";
                a.hasRange = true;
                a.min      = ax->range().lower;
                a.max      = ax->range().upper;
                axes.append(std::move(a));
            }
        }
        layout.axesByDomain.insert(key, axes);

        QList<scope::plot::PlotLayoutChannel> chans;
        for (const auto& name : store_.channelNames()) {
            auto sig = store_.get(name);
            if (!sig) continue;
            if (channelDomainKey(sig) != key) continue;
            scope::plot::PlotLayoutChannel c;
            c.name      = name;
            c.axisIndex = st.rowState.value(name, {true, 0}).second;
            c.domain    = key;
            // The engine is the authoritative record of a channel's formula.
            // Scraping sourceSymbol used to drop the formula whenever a
            // reopened file (e.g. CSV) had overwritten it.
            c.formula   = engine_.formulaFor(name);
            chans.append(std::move(c));
        }
        layout.channelsByDomain.insert(key, chans);
    }

    // v1 mirror — populate the flat lists from the active domain so an
    // older binary still sees a coherent (if partial) view.
    {
        const QString activeKey =
            (activeDomain() == Domain::Frequency) ? "frequency" : "time";
        layout.axes     = layout.axesByDomain.value(activeKey);
        layout.channels = layout.channelsByDomain.value(activeKey);
    }
    return layout;
}

scope::converter::HtmlExportView AnalyserPlot::htmlExportView() {
    // Freshen the active domain's snapshot so the export matches the screen
    // (the inactive domain keeps its last-shown state, like currentLayout).
    snapshotIntoDomain(activeDomain());

    using Domain = scope::core::Signal::Domain;
    auto build = [&](Domain d) {
        converter::HtmlChartView cv;
        const auto& st = stateByDomain_.value(static_cast<int>(d));
        QList<scope::plot::PlotLayoutAxis> axes = st.axes;
        if (axes.isEmpty()) {
            scope::plot::PlotLayoutAxis a;
            a.label = "Y1";
            a.side  = "left";
            axes.append(a);
        }
        for (int i = 0; i < axes.size(); ++i) {
            converter::HtmlAxis ha;
            ha.label = axes[i].label;
            ha.right = axes[i].side == "right";
            ha.color = scope::plot::ScopePlot::axisBaseColorFor(false, i).name();
            cv.axes.append(ha);
        }
        QHash<int, int> perAxis;
        for (const auto& name : store_.channelNames()) {
            auto sig = store_.get(name);
            if (!sig || sig->meta().domain != d || sig->sampleCount() == 0)
                continue;
            const auto rs = st.rowState.value(name, {true, 0});
            converter::HtmlChannel hc;
            hc.name      = name;
            hc.axisIndex = std::clamp(rs.second, 0,
                                      static_cast<int>(cv.axes.size()) - 1);
            hc.visible   = rs.first;
            const int onAxis = perAxis[hc.axisIndex]++;
            hc.color = scope::plot::ScopePlot::deriveChannelColorFor(
                           false, hc.axisIndex, onAxis).name();
            cv.channels.append(hc);
        }
        return cv;
    };

    converter::HtmlExportView v;
    v.time      = build(Domain::Time);
    v.frequency = build(Domain::Frequency);
    v.initialView = (viewMode_ == ViewMode::XY)        ? "xy"
                  : (viewMode_ == ViewMode::Frequency) ? "frequency"
                                                       : "time";
    v.xyChannel = xyChannel_;
    return v;
}

void AnalyserPlot::saveLayoutDialog() {
    QFileDialog dlg(this, "Save plot layout");
    dlg.setAcceptMode(QFileDialog::AcceptSave);
    dlg.setNameFilters({"Scope analyser layout (*.scoana)", "All files (*)"});
    dlg.setDefaultSuffix("scoana");
    if (dlg.exec() != QDialog::Accepted) return;
    const auto sel = dlg.selectedFiles();
    if (sel.isEmpty()) return;
    QString path = sel.first();
    if (!path.endsWith(".scoana", Qt::CaseInsensitive)) path += ".scoana";

    QString err;
    if (!currentLayout().saveToFile(
            std::filesystem::path(path.toStdString()), &err)) {
        QMessageBox::critical(this, "Save failed", err);
    }
}

void AnalyserPlot::applyLayout(const scope::plot::PlotLayout& layout,
                               FormulaImport mode, LayoutApply axes) {
    // ---- Hydrate stateByDomain_ from the layout ----------------------
    using Domain = scope::core::Signal::Domain;

    // Merge keeps the axes already on screen, so capture any live edits (a
    // renamed axis, a manual range) into the active domain's state first;
    // otherwise applyFromDomain would rebuild from a stale snapshot.
    if (axes == LayoutApply::Merge)
        snapshotIntoDomain(activeDomain());

    auto loadDomain = [&](Domain d, const QString& key) {
        const auto& inAxes  = layout.axesByDomain.value(key);
        const auto& inChans = layout.channelsByDomain.value(key);

        if (axes == LayoutApply::Replace) {
            PerDomainState st;
            st.axes = inAxes;
            // Channels → per-channel axis assignment. Visibility defaults to
            // true (layout doesn't persist visibility — it's transient UI).
            for (const auto& c : inChans)
                st.rowState[c.name] = {true, c.axisIndex};
            stateByDomain_[static_cast<int>(d)] = std::move(st);
            return;
        }

        // Merge: reconcile incoming axes with the existing ones by label —
        // reuse a matching axis (keeping its name / side / range), append the
        // rest, and remap each incoming axis index to the merged index so the
        // assignments land on the right axis. Existing channels' assignments
        // are left untouched, so a second file can't clobber the first's.
        PerDomainState& st = stateByDomain_[static_cast<int>(d)];
        QHash<int, int> remap;
        for (int i = 0; i < inAxes.size(); ++i) {
            int match = -1;
            for (int j = 0; j < st.axes.size(); ++j) {
                if (st.axes[j].label == inAxes[i].label) { match = j; break; }
            }
            if (match < 0) {
                st.axes.append(inAxes[i]);
                match = st.axes.size() - 1;
            }
            remap.insert(i, match);
        }
        for (const auto& c : inChans)
            st.rowState[c.name] = {true, remap.value(c.axisIndex, 0)};
    };
    loadDomain(Domain::Time,      "time");
    loadDomain(Domain::Frequency, "frequency");

    // ---- Formula (math) channels -------------------------------------
    // Applied per channel. A formula channel that arrived WITH its data is a
    // "choice" channel — the open popup's mode decides what happens to it:
    //   Recalculate    — re-evaluate it from the sources (may change values).
    //   ImportDataOnly — keep its exact saved values and strip the formula
    //                    (forget it + clear sourceSymbol) → a plain signal,
    //                    not editable as a formula and never recomputed.
    // A formula channel saved WITHOUT data (only the formula survives, e.g.
    // "No data for math channels") has nothing to keep, so it is always
    // registered and recomputed — even under ImportDataOnly.
    const auto allLayoutChans = [&] {
        QList<scope::plot::PlotLayoutChannel> all;
        for (const auto& list : layout.channelsByDomain) all.append(list);
        return all;
    }();
    for (const auto& c : allLayoutChans) {
        if (c.formula.isEmpty()) continue;
        // Checked before recomputeAll() below adds the formula-only channels.
        const bool hasData = store_.contains(c.name);
        if (mode == FormulaImport::Recalculate || !hasData) {
            engine_.rememberFormula(c.name, c.formula);
        } else {  // ImportDataOnly + data present → freeze as a plain signal
            engine_.forget(c.name);
            if (auto s = store_.get(c.name)) {
                auto m = s->meta();
                if (!m.sourceSymbol.isEmpty()) {
                    m.sourceSymbol.clear();
                    s->setMeta(m);
                }
            }
        }
    }
    // Recomputes whatever got registered: in Recalculate every formula; in
    // ImportDataOnly only the formula-only channels (the data-bearing ones
    // were forgotten, so they keep their saved values).
    QStringList formulaErrors;
    engine_.recomputeAll(&formulaErrors);
    if (!formulaErrors.isEmpty()) {
        QMessageBox::warning(this, "Some formulas failed",
            "Couldn't re-evaluate (kept any saved data as-is):\n"
                + formulaErrors.join("\n"));
    }
    pendingAssignments_.clear();
    for (const auto& c : allLayoutChans) {
        if (!store_.contains(c.name))
            pendingAssignments_.insert(c.name, c.axisIndex);
    }

    // ---- Restore viewMode + XY channel + active-domain axes ----------
    if (layout.viewMode == "xy")             viewMode_ = ViewMode::XY;
    else if (layout.viewMode == "frequency") viewMode_ = ViewMode::Frequency;
    else                                     viewMode_ = ViewMode::Time;
    xyChannel_ = layout.xyChannel;

    // Sync the View combo without re-triggering its slot — the slot
    // would otherwise try to snapshot/restore on top of the state we
    // just hydrated.
    {
        QSignalBlocker b(viewCombo_);
        viewCombo_->setCurrentIndex(viewMode_ == ViewMode::XY        ? 2
                                  : viewMode_ == ViewMode::Frequency ? 1 : 0);
    }
    xyXRow_->setVisible(viewMode_ == ViewMode::XY);
    if (viewMode_ == ViewMode::XY) {
        rebuildXyXCombo();
        if (!xyChannel_.isEmpty()) {
            QSignalBlocker b(xyXCombo_);
            const int idx = xyXCombo_->findData(xyChannel_);
            if (idx >= 0) xyXCombo_->setCurrentIndex(idx);
        }
    }

    clearAllPlottables();
    applyFromDomain(activeDomain());

    // X axis label for non-XY views — XY sets its own from the channel.
    if (viewMode_ == ViewMode::Time)
        scope_->plot()->xAxis->setLabel("t [s]");
    else if (viewMode_ == ViewMode::Frequency)
        scope_->plot()->xAxis->setLabel("f [Hz]");

    rebuildTable();
    hasDrawnYet_ = false;
    redrawForActiveChannels();
}

void AnalyserPlot::remapLayoutChannelNames(
        scope::plot::PlotLayout& layout,
        const QHash<QString, QString>& renamed) {
    auto remap = [&](QString& n) {
        const auto it = renamed.constFind(n);
        if (it != renamed.constEnd()) n = it.value();
    };
    for (auto& list : layout.channelsByDomain)
        for (auto& c : list) remap(c.name);
    for (auto& c : layout.channels) remap(c.name);
    remap(layout.xyChannel);
}

void AnalyserPlot::loadLayoutDialog() {
    QFileDialog dlg(this, "Load plot layout");
    dlg.setAcceptMode(QFileDialog::AcceptOpen);
    dlg.setNameFilters({"Scope analyser layout (*.scoana)", "All files (*)"});
    if (dlg.exec() != QDialog::Accepted) return;
    const auto sel = dlg.selectedFiles();
    if (sel.isEmpty()) return;

    QString err;
    auto layout = scope::plot::PlotLayout::loadFromFile(
        std::filesystem::path(sel.first().toStdString()), &err);
    if (!err.isEmpty()) {
        QMessageBox::critical(this, "Load failed", err);
        return;
    }
    applyLayout(layout);
    // applyLayout recomputes formulas, so its channelAdded bursts are gated
    // out of scheduleCommit — record the loaded layout as one step explicitly.
    scheduleCommit();
}

void AnalyserPlot::addChannelDialog() {
    AddChannelDialog dlg(store_, engine_, this);
    dlg.exec();   // engine emits store changes which arrive via the slot
}

void AnalyserPlot::editChannelDialog() {
    const int row = table_->currentRow();
    if (row < 0) {
        QMessageBox::information(this, "Edit channel",
            "Select a channel in the table first.");
        return;
    }
    const QString name = table_->item(row, ColName)->text();
    auto sig = store_.get(name);
    if (!sig) return;

    // The engine is the source of truth for formulas (survives a reopen
    // even when the file format dropped sourceSymbol). Fall back to
    // scraping sourceSymbol for anything it doesn't know about.
    QString expr = engine_.formulaFor(name);
    if (expr.isEmpty()) {
        const QString src = sig->meta().sourceSymbol;
        const int eq = src.indexOf('=');
        if (eq > 0 && src.left(eq).trimmed() == name)
            expr = src.mid(eq + 1).trimmed();
    }
    if (expr.isEmpty()) {
        QMessageBox::information(this, "Not a formula channel",
            QString("'%1' wasn't created with a formula, so there's "
                    "nothing to edit here. Use Remove + Add to replace "
                    "it with a derived channel.").arg(name));
        return;
    }

    AddChannelDialog dlg(store_, engine_, this);
    dlg.setEditMode(name, expr);
    dlg.exec();
}

void AnalyserPlot::saveChartDialog() {
    if (store_.size() == 0) {
        QMessageBox::information(this, "Nothing to save",
            "The store is empty — add channels first.");
        return;
    }
    const auto xr = scope_->plot()->xAxis->range();
    converter::ui::SaveChartDialog dlg(xr.lower, xr.upper, this);
    dlg.setChannels(store_);
    dlg.setRepeatedTimestamps(converter::scanRepeatedTimestamps(store_));
    if (dlg.exec() != QDialog::Accepted) return;

    const auto fmtChoice = dlg.format();
    const converter::FileFormat fmt =
          (fmtChoice == converter::ui::SaveChartDialog::Format::Csv)  ? converter::FileFormat::Csv
        : (fmtChoice == converter::ui::SaveChartDialog::Format::Mdf4) ? converter::FileFormat::Mdf4
        : (fmtChoice == converter::ui::SaveChartDialog::Format::Html) ? converter::FileFormat::Html
        : (fmtChoice == converter::ui::SaveChartDialog::Format::Json) ? converter::FileFormat::Json
                                                                      : converter::FileFormat::Hdf5;

    QFileDialog fileDlg(this,
        QString("Save chart (.%1)").arg(converter::defaultSuffix(fmt)));
    fileDlg.setAcceptMode(QFileDialog::AcceptSave);
    fileDlg.setNameFilters(converter::nameFilters(fmt));
    fileDlg.setDefaultSuffix(converter::defaultSuffix(fmt));
    if (fileDlg.exec() != QDialog::Accepted) return;
    const auto sel = fileDlg.selectedFiles();
    if (sel.isEmpty()) return;

    converter::ChartSaveFilters filters;
    filters.includeTime               = dlg.includeTimeDomain();
    filters.includeFrequency          = dlg.includeFrequencyDomain();
    // Derived-channel DATA is dropped when the user excluded derived channels
    // OR chose "No data for math channels" (formula-only — recomputed on open).
    filters.includeDerived            = dlg.includeDerivedChannels()
                                        && !dlg.noDataForMathChannels();
    filters.selectedChannels          = dlg.selectedChannels();
    filters.splitDomainsIntoTwoFiles  = dlg.splitDomainsIntoTwoFiles();
    filters.useCustomRange            = dlg.useCustomRange();
    filters.fromSec                   = dlg.fromSec();
    filters.toSec                     = dlg.toSec();

    converter::SaveOptions opts;
    opts.csv = dlg.csvOptions();
    // Embedded metadata (CSV scope-csv header, HDF5 /metadata, MDF4 header,
    // HTML island) so a re-open restores math channels / layout. The
    // Metadata checkboxes decide what goes in: strip the formulas and/or the
    // layout the user opted out of; embed nothing when "Add metadata" is off.
    if (dlg.addMetadata()) {
        auto layout = currentLayout();
        if (!dlg.includeMathFormula()) layout.stripFormulas();
        if (!dlg.includeLayout())      layout.stripLayout();
        opts.layoutJson = layout.toJsonString();
        // On a split export each single-domain file carries only its own
        // domain's layout; fall back to the full layout for a combined write.
        if (filters.splitDomainsIntoTwoFiles) {
            opts.layoutJsonByDomain.insert(
                "time", layout.restrictedToDomain("time").toJsonString());
            opts.layoutJsonByDomain.insert(
                "frequency", layout.restrictedToDomain("frequency").toJsonString());
        }
    }
    // Storable HTML mirrors the live plot (axes / colours / assignments /
    // view). Build the view here on the GUI thread, before the write runs
    // off-thread. The embedded layout (opts.layoutJson) is folded in by the
    // HTML writer so re-opening restores axes / view like the other formats.
    if (fmt == converter::FileFormat::Html) {
        opts.htmlView = htmlExportView();
        opts.htmlViewOnly = dlg.htmlViewOnly();
    }

    // Run the (potentially slow) write off the GUI thread behind a busy
    // dialog. saveChartFromStore reads the store via its concurrent-read
    // path, so this is safe.
    struct SaveOutcome {
        converter::ChartSaveResult result{converter::ChartSaveResult::Failed};
        QStringList messages;
        QString error;
    };
    const QString target = sel.first();
    const auto outcome = converter::ui::runWithBusyDialog(this, tr("Saving chart…"),
        [&]() -> SaveOutcome {
            SaveOutcome o;
            QStringList msgs;
            QString err;
            o.result   = converter::saveChartFromStore(
                target, store_, fmt, filters, opts, &msgs, &err);
            o.messages = std::move(msgs);
            o.error    = std::move(err);
            return o;
        });

    switch (outcome.result) {
        case converter::ChartSaveResult::NothingMatchedFilters:
            QMessageBox::warning(this, "Nothing to save",
                "No channels matched the current filters / time range.");
            return;
        case converter::ChartSaveResult::Failed:
            QMessageBox::critical(this, "Save failed", outcome.error);
            return;
        case converter::ChartSaveResult::Ok:
            QMessageBox::information(this, "Saved", outcome.messages.join("\n"));
            return;
    }
}

namespace {

QString uniqueStoreName(const core::SignalStore& store, const QString& base) {
    if (!store.contains(base)) return base;
    for (int i = 2; i < 10000; ++i) {
        QString c = QString("%1 (%2)").arg(base).arg(i);
        if (!store.contains(c)) return c;
    }
    return base + " (?)";
}

}  // namespace

void AnalyserPlot::openChartDialog() {
    QFileDialog dlg(this, "Open chart");
    dlg.setAcceptMode(QFileDialog::AcceptOpen);
    dlg.setNameFilters(converter::nameFilters(converter::FileFormat::Auto));
    if (dlg.exec() != QDialog::Accepted) return;
    const auto sel = dlg.selectedFiles();
    if (sel.isEmpty()) return;
    const QString path = sel.first();

    // Read off the GUI thread behind a busy dialog — loadFile touches no
    // shared state (it builds brand-new Signals), so this is fully safe.
    const auto r = converter::ui::runWithBusyDialog(this, tr("Opening chart…"),
        [path]() {
            return converter::loadFile(
                std::filesystem::path(path.toStdString()));
        });
    if (!r.ok && r.channels.empty()) {
        QMessageBox::critical(this, "Open failed",
            r.error.isEmpty() ? QString("Couldn't open file.") : r.error);
        return;
    }
    if (r.channels.empty()) {
        QMessageBox::warning(this, "Empty file",
            r.error.isEmpty() ? QString("No channels found in this file.") : r.error);
        return;
    }

    // The CSV loader recognised a vendor-specific layout and parsed it with
    // format-specific guesses (channel names, units, time scaling). Warn the
    // user before committing the channels — the mapping could be wrong, and
    // the Converter offers full manual control if so.
    if (!r.autoDetectedFormat.isEmpty()) {
        const auto resp = QMessageBox::warning(this, "Auto-detected format",
            QString("'%1' was automatically detected as a %2 and loaded "
                    "without the Converter.\n\nChannel names, units and timing "
                    "were guessed from the file and may be wrong — verify the "
                    "plotted data, or open it in the Converter for full "
                    "control.\n\nLoad %3 channel(s)?")
                .arg(QFileInfo(path).fileName(), r.autoDetectedFormat)
                .arg(r.channels.size()),
            QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Ok);
        if (resp != QMessageBox::Ok) return;
    }

    // If the file carries an embedded PlotLayout (any ScopeAnalyser-saved
    // .h5 / .mf4 / .csv / .html), parse it up front. Foreign / pre-layout
    // files leave layoutJson empty.
    scope::plot::PlotLayout layout;
    bool haveLayout = false;
    if (!r.layoutJson.isEmpty()) {
        QString lerr;
        layout = scope::plot::PlotLayout::fromJsonString(r.layoutJson, &lerr);
        haveLayout = lerr.isEmpty();
    }

    // Only math channels that arrived WITH their data offer a real choice
    // (keep the saved values, or recompute). Channels saved formula-only are
    // always recomputed, so they don't need the popup. Count the choosable
    // ones = formula channels whose data is actually in this file.
    FormulaImport formulaMode = FormulaImport::Recalculate;
    if (haveLayout) {
        QSet<QString> loadedNames;
        for (const auto& s : r.channels)
            if (s) loadedNames.insert(s->meta().name);
        int choiceCount = 0;
        for (const auto& list : layout.channelsByDomain)
            for (const auto& c : list)
                if (!c.formula.isEmpty() && loadedNames.contains(c.name))
                    ++choiceCount;

        if (choiceCount > 0) {
            QMessageBox box(this);
            box.setIcon(QMessageBox::Question);
            box.setWindowTitle("Formula channels");
            box.setText(QString("This file contains %1 math channel(s) saved "
                                "with their data.").arg(choiceCount));
            box.setInformativeText(
                "How should they be imported?\n\n"
                "• Import data only — keep the saved values exactly and "
                "discard the formula, so they become plain signal channels "
                "(not editable as formulas, never recalculated).\n"
                "• Import formula — re-run each formula over the current "
                "sources (may change the saved values).\n\n"
                "Math channels saved as formula-only (no data) are recomputed "
                "either way.");
            // All three share ActionRole so QMessageBox lays them out in the
            // order added (left → right): data, formula, then Cancel on the
            // right. Escape / window-close maps to Cancel.
            auto* dataBtn    = box.addButton("Import data only",
                                             QMessageBox::ActionRole);
            auto* formulaBtn = box.addButton("Import formula",
                                             QMessageBox::ActionRole);
            auto* cancelBtn  = box.addButton("Cancel", QMessageBox::ActionRole);
            box.setDefaultButton(dataBtn);
            box.setEscapeButton(cancelBtn);
            box.exec();
            auto* clicked = box.clickedButton();
            if (clicked == dataBtn)         formulaMode = FormulaImport::ImportDataOnly;
            else if (clicked == formulaBtn) formulaMode = FormulaImport::Recalculate;
            else return;   // Cancel / closed — nothing added
        }
    }

    QHash<QString, QString> renamed;   // original → unique store name
    for (auto& s : r.channels) {
        auto meta = s->meta();
        const QString original = meta.name;
        meta.name = uniqueStoreName(store_, meta.name);
        s->setMeta(meta);
        renamed.insert(original, meta.name);
        store_.add(s);
    }

    if (haveLayout) {
        // Channels that collided with the store were renamed (e.g.
        // "Speed (2)"); the embedded layout still uses the original names, so
        // remap its references or the renamed channels would lose their saved
        // axis assignment. Then merge so a second chart doesn't overwrite the
        // first file's Y-axis names / assignments.
        remapLayoutChannelNames(layout, renamed);
        applyLayout(layout, formulaMode, LayoutApply::Merge);
    }

    QMessageBox::information(this, "Opened",
        QString("Loaded %1 channel(s) from %2")
            .arg(r.channels.size()).arg(QFileInfo(path).fileName()));
}

}  // namespace scope::analyser::ui
