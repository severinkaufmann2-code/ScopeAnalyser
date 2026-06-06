#include "AnalyserPlot.h"

#include "AddChannelDialog.h"
#include "SaveChartDialog.h"

#include "scope/analyser/FormulaEngine.h"
#include "scope/plot/ScopePlot.h"
#include "scope/plot/PlotLayout.h"
#include "scope/converter/SignalIO.h"

#include <qcustomplot.h>

#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextStream>
#include <QVBoxLayout>

#include <array>
#include <filesystem>
#include <limits>

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
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setMaximumWidth(320);

    auto* addChBtn    = new QPushButton("+ Add channel…", this);
    auto* editChBtn   = new QPushButton("Edit channel…", this);
    auto* removeChBtn = new QPushButton("− Remove channel", this);
    auto* addAxisBtn  = new QPushButton("+ Y axis", this);
    auto* delAxisBtn  = new QPushButton("− Y axis", this);
    auto* saveBtn     = new QPushButton("Save layout…", this);
    auto* loadBtn     = new QPushButton("Load layout…", this);
    auto* openChartBtn= new QPushButton("Open chart…", this);
    auto* saveChartBtn= new QPushButton("Save chart…", this);
    auto* redrawBtn   = new QPushButton("Redraw",   this);

    auto* chBtnRow = new QHBoxLayout();
    chBtnRow->addWidget(addChBtn);
    chBtnRow->addWidget(editChBtn);
    chBtnRow->addWidget(removeChBtn);
    auto* axisBtnRow = new QHBoxLayout();
    axisBtnRow->addWidget(addAxisBtn);
    axisBtnRow->addWidget(delAxisBtn);
    auto* layoutBtnRow = new QHBoxLayout();
    layoutBtnRow->addWidget(saveBtn);
    layoutBtnRow->addWidget(loadBtn);
    auto* chartBtnRow = new QHBoxLayout();
    chartBtnRow->addWidget(openChartBtn);
    chartBtnRow->addWidget(saveChartBtn);

    auto* viewRow = new QHBoxLayout();
    viewRow->addWidget(new QLabel("View:", this));
    auto* viewCombo = new QComboBox(this);
    viewCombo->addItem("Time  (t [s])");
    viewCombo->addItem("Frequency  (f [Hz])");
    viewCombo->setToolTip(
        "Time: show only time-domain channels (recorded / imported / "
        "derived from time-domain).\n"
        "Frequency: show only frequency-domain channels (output of "
        "FFT). The X-axis label updates accordingly.");
    viewRow->addWidget(viewCombo, /*stretch=*/1);

    auto* leftLayout = new QVBoxLayout();
    leftLayout->addLayout(viewRow);
    leftLayout->addWidget(new QLabel("Channels", this));
    leftLayout->addWidget(table_, /*stretch=*/1);
    leftLayout->addLayout(chBtnRow);
    leftLayout->addLayout(axisBtnRow);
    leftLayout->addLayout(layoutBtnRow);
    leftLayout->addLayout(chartBtnRow);
    leftLayout->addWidget(redrawBtn);

    auto* root = new QHBoxLayout(this);
    root->addLayout(leftLayout);
    root->addWidget(scope_, /*stretch=*/1);

    connect(redrawBtn,    &QPushButton::clicked, this, &AnalyserPlot::redrawAll);
    connect(saveBtn,      &QPushButton::clicked, this, &AnalyserPlot::saveLayoutDialog);
    connect(loadBtn,      &QPushButton::clicked, this, &AnalyserPlot::loadLayoutDialog);
    connect(addChBtn,     &QPushButton::clicked, this, &AnalyserPlot::addChannelDialog);
    connect(editChBtn,    &QPushButton::clicked, this, &AnalyserPlot::editChannelDialog);
    connect(saveChartBtn, &QPushButton::clicked, this, &AnalyserPlot::saveChartDialog);
    connect(openChartBtn, &QPushButton::clicked, this, &AnalyserPlot::openChartDialog);
    connect(viewCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx){
        viewDomain_ = (idx == 1) ? scope::core::Signal::Domain::Frequency
                                 : scope::core::Signal::Domain::Time;
        scope_->plot()->xAxis->setLabel(
            viewDomain_ == scope::core::Signal::Domain::Frequency
                ? "f [Hz]" : "t [s]");
        rebuildTable();
        // Fresh data shape — let the next redraw auto-fit X.
        hasDrawnYet_ = false;
        redrawForActiveChannels();
    });
    connect(table_, &QTableWidget::cellDoubleClicked, this,
            [this](int, int){ editChannelDialog(); });
    // Click on a graph in the chart → select its row in the channels
    // table (drives the row-selection highlight, so the user can see
    // which channel they just clicked).
    connect(scope_->plot(), &QCustomPlot::plottableClick, this,
            [this](QCPAbstractPlottable* p, int /*dataIndex*/, QMouseEvent*){
        auto* gr = qobject_cast<QCPGraph*>(p);
        if (!gr) return;
        QString clickedName;
        for (auto it = graphs_.constBegin(); it != graphs_.constEnd(); ++it) {
            if (it.value() == gr) { clickedName = it.key(); break; }
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
    connect(removeChBtn,  &QPushButton::clicked, this, [this]{
        const int r = table_->currentRow();
        if (r < 0) return;
        const QString name = table_->item(r, /*ColName=*/1)->text();
        const auto resp = QMessageBox::question(
            this, "Remove channel?",
            QString("Remove channel '%1' from the store?").arg(name),
            QMessageBox::Yes | QMessageBox::Cancel);
        if (resp == QMessageBox::Yes) store_.remove(name);
    });
    connect(addAxisBtn, &QPushButton::clicked, this, [this]{
        scope_->addYAxis();
        rebuildAxisCombos();
    });
    connect(delAxisBtn, &QPushButton::clicked, this, [this]{
        const int last = scope_->yAxisCount() - 1;
        if (last <= 0) return;
        QString err;
        if (!scope_->removeYAxis(last, &err)) {
            QMessageBox::information(this, "Can't remove", err);
            return;
        }
        rebuildAxisCombos();
        recolorChannels();
        redrawForActiveChannels();
    });

    connect(&store_, &scope::core::SignalStore::channelAdded,
            this, &AnalyserPlot::onChannelAdded);
    connect(&store_, &scope::core::SignalStore::channelRemoved,
            this, &AnalyserPlot::onChannelRemoved);
    connect(scope_, &scope::plot::ScopePlot::yAxesChanged,
            this, [this]{ rebuildAxisCombos(); });

    rebuildTable();
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
    // Snapshot whatever's currently shown into the sticky cache so we
    // don't lose user-set visibility / axis when the View combo flips
    // between Time and Frequency.
    for (int r = 0; r < table_->rowCount(); ++r) {
        const QString name = table_->item(r, ColName) ? table_->item(r, ColName)->text() : QString();
        if (name.isEmpty()) continue;
        bool vis = false; int axis = 0;
        if (auto* cb = qobject_cast<QCheckBox*>(table_->cellWidget(r, ColVis))) vis = cb->isChecked();
        if (auto* co = qobject_cast<QComboBox*>(table_->cellWidget(r, ColAxis))) axis = co->currentIndex();
        savedRowState_[name] = {vis, axis};
    }
    auto& prev = savedRowState_;

    table_->setRowCount(0);
    const bool firstTime = prev.isEmpty();
    const auto names = store_.channelNames();
    for (const auto& name : names) {
        // Only show channels in the currently-selected domain.
        if (auto s = store_.get(name); s && s->meta().domain != viewDomain_) continue;
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
    if (!graphs_.contains(name)) return;
    scope_->setGraphYAxis(graphs_[name], axisIndex);
    recolorChannels();
    scope_->plot()->replot(QCustomPlot::rpQueuedReplot);
}

void AnalyserPlot::onVisibilityChanged(int /*row*/) {
    redrawForActiveChannels();
}

void AnalyserPlot::recolorChannels() {
    // For each axis, give the Nth channel on it the Nth shade derived from
    // that axis's base colour.
    QHash<int, int> perAxisCounter;
    for (int r = 0; r < table_->rowCount(); ++r) {
        const QString name = table_->item(r, ColName)->text();
        if (!graphs_.contains(name)) continue;
        const int axisIdx = axisIndexForRow(r);
        const int onAxis  = perAxisCounter[axisIdx]++;
        const QColor c = scope_->deriveChannelColor(axisIdx, onAxis);
        graphs_[name]->setPen(QPen(c));
    }
}

void AnalyserPlot::onChannelAdded(QString /*name*/)   { rebuildTable(); redrawForActiveChannels(); }
void AnalyserPlot::onChannelRemoved(QString name) {
    if (graphs_.contains(name)) {
        scope_->plot()->removeGraph(graphs_[name]);
        graphs_.remove(name);
    }
    rebuildTable();
    redrawForActiveChannels();
}

void AnalyserPlot::redrawAll() { redrawForActiveChannels(); }

void AnalyserPlot::redrawForActiveChannels() {
    const auto active = activeChannels();
    auto* plot = scope_->plot();

    for (auto it = graphs_.begin(); it != graphs_.end(); ) {
        if (!active.contains(it.key())) {
            plot->removeGraph(it.value());
            it = graphs_.erase(it);
        } else { ++it; }
    }

    double xMin = std::numeric_limits<double>::infinity();
    double xMax = -std::numeric_limits<double>::infinity();

    double base = std::numeric_limits<double>::infinity();
    for (const auto& name : active) {
        auto sig = store_.get(name);
        if (!sig) continue;
        auto view = sig->snapshotForRead();
        if (view.count > 0) base = std::min(base, view.timestamps[0] / 1e9);
    }
    if (base == std::numeric_limits<double>::infinity()) base = 0;

    for (int r = 0; r < table_->rowCount(); ++r) {
        const QString name = table_->item(r, ColName)->text();
        if (!active.contains(name)) continue;
        auto sig = store_.get(name);
        if (!sig) continue;

        const int axisIndex = axisIndexForRow(r);

        QCPGraph* graph = graphs_.value(name, nullptr);
        if (!graph) {
            graph = plot->addGraph(plot->xAxis, scope_->yAxis(axisIndex));
            graph->setName(name);
            // Adaptive sampling is left at its QCustomPlot default (true):
            // it draws one min/max line per pixel column for dense data,
            // which is essential for smooth pan/zoom on signals with
            // hundreds of thousands of samples. The "histogram bars" the
            // user once reported turned out to be a real comb pattern
            // produced by the old central-difference Derivative on
            // quantised data, not a rendering bug — the run-based
            // Derivative no longer produces that pattern.
            graphs_[name] = graph;
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

void AnalyserPlot::saveLayoutDialog() {
    QFileDialog dlg(this, "Save plot layout");
    dlg.setAcceptMode(QFileDialog::AcceptSave);
    dlg.setNameFilters({"Scope plot layout (*.scolayout)", "All files (*)"});
    dlg.setDefaultSuffix("scolayout");
    if (dlg.exec() != QDialog::Accepted) return;
    const auto sel = dlg.selectedFiles();
    if (sel.isEmpty()) return;
    QString path = sel.first();
    if (!path.endsWith(".scolayout", Qt::CaseInsensitive)) path += ".scolayout";

    scope::plot::PlotLayout layout;
    for (int i = 0; i < scope_->yAxisCount(); ++i) {
        auto* ax = scope_->yAxis(i);
        scope::plot::PlotLayoutAxis a;
        a.label = ax->label();
        a.side  = (ax->axisType() == QCPAxis::atRight) ? "right" : "left";
        a.hasRange = true;
        a.min = ax->range().lower;
        a.max = ax->range().upper;
        layout.axes.append(a);
    }
    // Flush whatever's currently in the table into the sticky cache so
    // the latest user edits aren't missed for the channels currently
    // visible. Then walk EVERY channel in the store — the table only
    // shows the current View (Time or Frequency); we want to persist
    // all of them.
    for (int r = 0; r < table_->rowCount(); ++r) {
        const QString name = table_->item(r, ColName)
                                 ? table_->item(r, ColName)->text()
                                 : QString();
        if (name.isEmpty()) continue;
        bool vis = false; int axis = 0;
        if (auto* cb = qobject_cast<QCheckBox*>(table_->cellWidget(r, ColVis))) vis = cb->isChecked();
        if (auto* co = qobject_cast<QComboBox*>(table_->cellWidget(r, ColAxis))) axis = co->currentIndex();
        savedRowState_[name] = {vis, axis};
    }

    for (const auto& name : store_.channelNames()) {
        auto sig = store_.get(name);
        if (!sig) continue;
        scope::plot::PlotLayoutChannel c;
        c.name = name;
        c.axisIndex = savedRowState_.value(name, {true, 0}).second;
        c.domain = (sig->meta().domain == scope::core::Signal::Domain::Frequency)
                       ? "frequency" : "time";
        // For formula-derived channels, FormulaEngine::evaluate stamps
        // "<name> = <expr>" into sourceSymbol. Extract just the
        // right-hand side so reload can re-evaluate it.
        const QString src = sig->meta().sourceSymbol;
        const int eq = src.indexOf('=');
        if (eq > 0) {
            const QString lhs = src.left(eq).trimmed();
            if (lhs == name) c.formula = src.mid(eq + 1).trimmed();
        }
        layout.channels.append(c);
    }
    QString err;
    if (!layout.saveToFile(std::filesystem::path(path.toStdString()), &err)) {
        QMessageBox::critical(this, "Save failed", err);
    }
}

void AnalyserPlot::loadLayoutDialog() {
    QFileDialog dlg(this, "Load plot layout");
    dlg.setAcceptMode(QFileDialog::AcceptOpen);
    dlg.setNameFilters({"Scope plot layout (*.scolayout)", "All files (*)"});
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

    // Remove extra axes (keep Y1).
    while (scope_->yAxisCount() > 1) {
        QString rmErr;
        // Move any graphs off the to-be-removed axis to Y1 first.
        const int idx = scope_->yAxisCount() - 1;
        for (int g = 0; g < scope_->plot()->graphCount(); ++g) {
            if (scope_->plot()->graph(g)->valueAxis() == scope_->yAxis(idx))
                scope_->setGraphYAxis(scope_->plot()->graph(g), 0);
        }
        if (!scope_->removeYAxis(idx, &rmErr)) break;
    }
    // Add axes from the layout (Y1 already exists at index 0).
    for (int i = 0; i < layout.axes.size(); ++i) {
        const auto& la = layout.axes[i];
        const Qt::Alignment side = (la.side == "right") ? Qt::AlignRight : Qt::AlignLeft;
        if (i == 0) {
            scope_->yAxis(0)->setLabel(la.label);
            if (la.hasRange) scope_->yAxis(0)->setRange(la.min, la.max);
        } else {
            const int newIdx = scope_->addYAxis(la.label, side);
            if (la.hasRange) scope_->yAxis(newIdx)->setRange(la.min, la.max);
        }
    }
    rebuildAxisCombos();

    // First pass: re-evaluate any formula-derived channels that aren't
    // already in the store. Doing this before the assignment loop means
    // the channel rows already exist (via the store's channelAdded
    // signal → rebuildTable) when we look them up below.
    QStringList formulaErrors;
    for (const auto& c : layout.channels) {
        if (c.formula.isEmpty()) continue;
        if (store_.contains(c.name)) continue;
        QString err;
        if (!engine_.evaluate(c.name + " = " + c.formula, &err)) {
            formulaErrors << QString("%1: %2").arg(c.name, err);
        }
    }
    if (!formulaErrors.isEmpty()) {
        QMessageBox::warning(this, "Some formulas failed",
            "Couldn't re-evaluate:\n" + formulaErrors.join("\n"));
    }

    // Apply channel assignments; remember pending for unknown channels.
    pendingAssignments_.clear();
    for (const auto& c : layout.channels) {
        bool found = false;
        for (int r = 0; r < table_->rowCount(); ++r) {
            if (table_->item(r, ColName)->text() == c.name) {
                setAxisIndexForRow(r, c.axisIndex);
                found = true;
                break;
            }
        }
        if (!found) pendingAssignments_.insert(c.name, c.axisIndex);
    }
    redrawForActiveChannels();
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

    // Formula-derived channels have sourceSymbol set to "<name> = <expr>"
    // by FormulaEngine::evaluate. Anything else (recorded / imported)
    // doesn't have an editable formula.
    const QString src = sig->meta().sourceSymbol;
    const int eq = src.indexOf('=');
    QString origName, expr;
    if (eq > 0) {
        origName = src.left(eq).trimmed();
        expr     = src.mid(eq + 1).trimmed();
    }
    if (origName.isEmpty() || expr.isEmpty() || origName != name) {
        QMessageBox::information(this, "Not a formula channel",
            QString("'%1' wasn't created with a formula, so there's "
                    "nothing to edit here. Use Remove + Add to replace "
                    "it with a derived channel.").arg(name));
        return;
    }

    AddChannelDialog dlg(store_, engine_, this);
    dlg.setEditMode(origName, expr);
    dlg.exec();
}

void AnalyserPlot::saveChartDialog() {
    if (store_.size() == 0) {
        QMessageBox::information(this, "Nothing to save",
            "The store is empty — add channels first.");
        return;
    }
    const auto xr = scope_->plot()->xAxis->range();
    SaveChartDialog dlg(xr.lower, xr.upper, this);
    if (dlg.exec() != QDialog::Accepted) return;

    const auto fmtChoice = dlg.format();
    const converter::FileFormat fmt =
          (fmtChoice == SaveChartDialog::Format::Csv)  ? converter::FileFormat::Csv
        : (fmtChoice == SaveChartDialog::Format::Mdf4) ? converter::FileFormat::Mdf4
                                                       : converter::FileFormat::Hdf5;
    const bool csv = (fmt == converter::FileFormat::Csv);
    const bool customRange = dlg.useCustomRange();
    const core::TimestampNs fromNs = customRange
        ? static_cast<core::TimestampNs>(dlg.fromSec() * 1e9)
        : std::numeric_limits<core::TimestampNs>::min();
    const core::TimestampNs toNs = customRange
        ? static_cast<core::TimestampNs>(dlg.toSec()   * 1e9)
        : std::numeric_limits<core::TimestampNs>::max();
    const bool incTime    = dlg.includeTimeDomain();
    const bool incFreq    = dlg.includeFrequencyDomain();
    const bool incDerived = dlg.includeDerivedChannels();
    const bool splitFiles = dlg.splitDomainsIntoTwoFiles();

    // A signal is "derived" if its source symbol is "<name> = <expr>" with
    // lhs == its own name (the stamp FormulaEngine::evaluate leaves).
    auto isDerived = [](const std::shared_ptr<core::Signal>& s) {
        const QString src = s->meta().sourceSymbol;
        const int eq = src.indexOf('=');
        if (eq <= 0) return false;
        return src.left(eq).trimmed() == s->meta().name;
    };

    // Build trimmed copies, partitioned by domain, after filters.
    std::vector<std::shared_ptr<core::Signal>> timeChans, freqChans;
    for (const auto& n : store_.channelNames()) {
        auto src = store_.get(n);
        if (!src) continue;
        const bool freq = (src->meta().domain == core::Signal::Domain::Frequency);
        if (freq && !incFreq) continue;
        if (!freq && !incTime) continue;
        if (!incDerived && isDerived(src)) continue;
        auto view = src->snapshotForRead();
        auto vs   = src->readAsDouble();
        if (view.count == 0) continue;

        std::vector<core::TimestampNs> tsBuf;
        std::vector<double>            vsBuf;
        tsBuf.reserve(view.count);
        vsBuf.reserve(view.count);
        // Time range filter only applies to time-domain signals — for
        // frequency, the "timestamps" encode Hz and the customRange
        // numbers are in seconds, so they wouldn't be meaningful.
        for (std::size_t i = 0; i < view.count; ++i) {
            const core::TimestampNs t = view.timestamps[i];
            if (!freq && (t < fromNs || t > toNs)) continue;
            tsBuf.push_back(t);
            vsBuf.push_back(i < vs.size() ? vs[i] : 0.0);
        }
        if (tsBuf.empty()) continue;
        core::Signal::Meta meta = src->meta();
        meta.dataType = core::DataType::Float64;
        auto trimmed = std::make_shared<core::Signal>(meta);
        trimmed->append(tsBuf.data(),
                        reinterpret_cast<const std::byte*>(vsBuf.data()),
                        tsBuf.size());
        if (freq) freqChans.push_back(std::move(trimmed));
        else      timeChans.push_back(std::move(trimmed));
    }
    if (timeChans.empty() && freqChans.empty()) {
        QMessageBox::warning(this, "Nothing to save",
            "No channels matched the current filters / time range.");
        return;
    }

    QFileDialog fileDlg(this,
        QString("Save chart (.%1)").arg(converter::defaultSuffix(fmt)));
    fileDlg.setAcceptMode(QFileDialog::AcceptSave);
    fileDlg.setNameFilters(converter::nameFilters(fmt));
    fileDlg.setDefaultSuffix(converter::defaultSuffix(fmt));
    if (fileDlg.exec() != QDialog::Accepted) return;
    const auto sel = fileDlg.selectedFiles();
    if (sel.isEmpty()) return;
    const QString basePath = sel.first();

    // Helper: insert a suffix before the extension.
    auto withSuffix = [](const QString& p, const QString& suffix) {
        const int dot = p.lastIndexOf('.');
        return (dot > p.lastIndexOf('/') && dot > p.lastIndexOf('\\'))
            ? p.left(dot) + suffix + p.mid(dot)
            : p + suffix;
    };

    converter::SaveOptions saveOpts;
    saveOpts.csv = dlg.csvOptions();

    auto writeOne = [&](const QString& outPath,
                        const std::vector<std::shared_ptr<core::Signal>>& chans,
                        QString* err) -> bool {
        return converter::saveFile(
            std::filesystem::path(outPath.toStdString()),
            chans, fmt, saveOpts, err);
    };

    const bool haveBoth = !timeChans.empty() && !freqChans.empty();
    const bool reallySplit = splitFiles && haveBoth;
    QStringList wrote;
    QString err;

    if (reallySplit) {
        const QString tPath = withSuffix(basePath, "_time");
        const QString fPath = withSuffix(basePath, "_frequency");
        if (!writeOne(tPath, timeChans, &err)) {
            QMessageBox::critical(this, "Save failed", err); return;
        }
        if (!writeOne(fPath, freqChans, &err)) {
            QMessageBox::critical(this, "Save failed", err); return;
        }
        wrote << QString("%1 time-domain channel(s) → %2")
                     .arg(timeChans.size()).arg(QFileInfo(tPath).fileName())
              << QString("%1 frequency-domain channel(s) → %2")
                     .arg(freqChans.size()).arg(QFileInfo(fPath).fileName());
    } else {
        // Single file, combined.
        std::vector<std::shared_ptr<core::Signal>> all;
        all.reserve(timeChans.size() + freqChans.size());
        for (auto& s : timeChans) all.push_back(s);
        for (auto& s : freqChans) all.push_back(s);
        if (!writeOne(basePath, all, &err)) {
            QMessageBox::critical(this, "Save failed", err); return;
        }
        wrote << QString("%1 channel(s) → %2  "
                         "(time: %3, frequency: %4)")
                     .arg(all.size())
                     .arg(QFileInfo(basePath).fileName())
                     .arg(timeChans.size())
                     .arg(freqChans.size());
    }
    QMessageBox::information(this, "Saved", wrote.join("\n"));
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

    auto r = converter::loadFile(std::filesystem::path(path.toStdString()));
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

    for (auto& s : r.channels) {
        auto meta = s->meta();
        meta.name = uniqueStoreName(store_, meta.name);
        s->setMeta(meta);
        store_.add(s);
    }
    QMessageBox::information(this, "Opened",
        QString("Loaded %1 channel(s) from %2")
            .arg(r.channels.size()).arg(QFileInfo(path).fileName()));
}

}  // namespace scope::analyser::ui
