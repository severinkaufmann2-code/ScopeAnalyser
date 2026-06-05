#include "AnalyserPlot.h"

#include "AddChannelDialog.h"
#include "SaveChartDialog.h"

#include "scope/analyser/FormulaEngine.h"
#include "scope/plot/ScopePlot.h"
#include "scope/plot/PlotLayout.h"
#include "scope/converter/CsvWriter.h"
#include "scope/core/Hdf5Session.h"

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

    auto* leftLayout = new QVBoxLayout();
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
    connect(table_, &QTableWidget::cellDoubleClicked, this,
            [this](int, int){ editChannelDialog(); });
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
    QHash<QString, std::pair<bool,int>> prev;
    for (int r = 0; r < table_->rowCount(); ++r) {
        const QString name = table_->item(r, ColName) ? table_->item(r, ColName)->text() : QString();
        if (name.isEmpty()) continue;
        bool vis = false; int axis = 0;
        if (auto* cb = qobject_cast<QCheckBox*>(table_->cellWidget(r, ColVis))) vis = cb->isChecked();
        if (auto* co = qobject_cast<QComboBox*>(table_->cellWidget(r, ColAxis))) axis = co->currentIndex();
        prev[name] = {vis, axis};
    }

    table_->setRowCount(0);
    const bool firstTime = prev.isEmpty();
    const auto names = store_.channelNames();
    for (const auto& name : names) {
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
            graphs_[name] = graph;
        } else if (graph->valueAxis() != scope_->yAxis(axisIndex)) {
            scope_->setGraphYAxis(graph, axisIndex);
        }
        // Re-apply every redraw so nothing can resurrect impulse/step
        // line style or adaptive sampling behind our back.
        graph->setLineStyle(QCPGraph::lsLine);
        graph->setScatterStyle(QCPScatterStyle::ssNone);
        graph->setAdaptiveSampling(false);
        graph->setBrush(Qt::NoBrush);

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

    if (active.isEmpty() || xMin == std::numeric_limits<double>::infinity()) {
        xMin = 0; xMax = 1;
    }
    plot->xAxis->setRange(xMin, xMax);
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
    for (int r = 0; r < table_->rowCount(); ++r) {
        scope::plot::PlotLayoutChannel c;
        c.name = table_->item(r, ColName)->text();
        c.axisIndex = axisIndexForRow(r);
        // For formula-derived channels, FormulaEngine::evaluate stamps
        // "<name> = <expr>" into sourceSymbol. Extract just the
        // right-hand side so reload can re-evaluate it.
        if (auto sig = store_.get(c.name)) {
            const QString src = sig->meta().sourceSymbol;
            const int eq = src.indexOf('=');
            if (eq > 0) {
                const QString lhs = src.left(eq).trimmed();
                if (lhs == c.name) c.formula = src.mid(eq + 1).trimmed();
            }
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
    // Default the custom range to the current plot view.
    const auto xr = scope_->plot()->xAxis->range();
    SaveChartDialog dlg(xr.lower, xr.upper, this);
    if (dlg.exec() != QDialog::Accepted) return;

    const bool csv = (dlg.format() == SaveChartDialog::Format::Csv);
    const bool customRange = dlg.useCustomRange();
    const core::TimestampNs fromNs = customRange
        ? static_cast<core::TimestampNs>(dlg.fromSec() * 1e9)
        : std::numeric_limits<core::TimestampNs>::min();
    const core::TimestampNs toNs = customRange
        ? static_cast<core::TimestampNs>(dlg.toSec()   * 1e9)
        : std::numeric_limits<core::TimestampNs>::max();

    // Build trimmed Signal copies of every channel in the store.
    std::vector<std::shared_ptr<core::Signal>> outChans;
    for (const auto& n : store_.channelNames()) {
        auto src = store_.get(n);
        if (!src) continue;
        auto view = src->snapshotForRead();
        auto vs   = src->readAsDouble();
        if (view.count == 0) continue;
        std::vector<core::TimestampNs> tsBuf;
        std::vector<double>            vsBuf;
        tsBuf.reserve(view.count);
        vsBuf.reserve(view.count);
        for (std::size_t i = 0; i < view.count; ++i) {
            const core::TimestampNs t = view.timestamps[i];
            if (t < fromNs || t > toNs) continue;
            tsBuf.push_back(t);
            vsBuf.push_back(i < vs.size() ? vs[i] : 0.0);
        }
        if (tsBuf.empty()) continue;
        core::Signal::Meta meta = src->meta();
        // Always export as Float64 — values were already widened on read.
        meta.dataType = core::DataType::Float64;
        auto trimmed = std::make_shared<core::Signal>(meta);
        trimmed->append(tsBuf.data(),
                        reinterpret_cast<const std::byte*>(vsBuf.data()),
                        tsBuf.size());
        outChans.push_back(std::move(trimmed));
    }
    if (outChans.empty()) {
        QMessageBox::warning(this, "Nothing to save",
            "No samples fell inside the selected time range.");
        return;
    }

    QFileDialog fileDlg(this, csv ? "Save chart (.csv)" : "Save chart (.h5)");
    fileDlg.setAcceptMode(QFileDialog::AcceptSave);
    if (csv) {
        fileDlg.setNameFilters({"CSV (*.csv)", "Text (*.txt)", "All files (*)"});
        fileDlg.setDefaultSuffix("csv");
    } else {
        fileDlg.setNameFilters({"Scope sessions (*.h5)", "All files (*)"});
        fileDlg.setDefaultSuffix("h5");
    }
    if (fileDlg.exec() != QDialog::Accepted) return;
    const auto sel = fileDlg.selectedFiles();
    if (sel.isEmpty()) return;
    QString path = sel.first();

    QString err;
    if (csv) {
        if (!converter::writeCsv(
                std::filesystem::path(path.toStdString()),
                outChans, dlg.csvOptions(), &err)) {
            QMessageBox::critical(this, "Save failed", err);
            return;
        }
    } else {
        auto session = core::Hdf5Session::create(
            std::filesystem::path(path.toStdString()), &err);
        if (!session) {
            QMessageBox::critical(this, "Save failed", err);
            return;
        }
        for (const auto& s : outChans) {
            if (!session->addChannel(s->meta(), &err)) continue;
            auto v = s->snapshotForRead();
            if (v.count > 0) {
                session->appendSamples(s->meta().name,
                                       v.timestamps, v.values, v.count, &err);
            }
        }
        session->flush();
    }
    QMessageBox::information(this, "Saved",
        QString("Wrote %1 channel(s) to %2")
            .arg(outChans.size()).arg(QFileInfo(path).fileName()));
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

// Split "<name> [<unit>]" into name + unit. If the bracketed unit is
// missing, returns name=trimmed input and unit empty.
void splitNameUnit(const QString& header, QString* name, QString* unit) {
    QString h = header.trimmed();
    const int open  = h.lastIndexOf('[');
    const int close = h.lastIndexOf(']');
    if (open > 0 && close > open && close == h.size() - 1) {
        *name = h.left(open).trimmed();
        *unit = h.mid(open + 1, close - open - 1).trimmed();
    } else {
        *name = h;
        *unit = QString();
    }
}

// Pick the column delimiter by counting candidates in the header line.
QChar autoDetectDelim(const QString& headerLine) {
    const std::array<QChar, 4> candidates{QChar(','), QChar(';'),
                                          QChar('\t'), QChar('|')};
    QChar best = ',';
    int bestCount = -1;
    for (QChar c : candidates) {
        const int n = headerLine.count(c);
        if (n > bestCount) { bestCount = n; best = c; }
    }
    return best;
}

// Parse a CSV chart produced by AnalyserPlot::saveChartDialog or the
// Converter's Save CSV. Auto-detects the column delimiter and the
// time-mode (shared single time column vs per-signal pairs).
bool loadCsvChart(const QString& path,
                  std::vector<std::shared_ptr<core::Signal>>* out,
                  QString* errorOut) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorOut) *errorOut = "Couldn't open file.";
        return false;
    }
    QTextStream ts(&f);
    QString headerLine;
    do {
        headerLine = ts.readLine();
    } while (!headerLine.isNull() && headerLine.trimmed().isEmpty());
    if (headerLine.isNull()) {
        if (errorOut) *errorOut = "Empty file.";
        return false;
    }

    const QChar sep = autoDetectDelim(headerLine);
    const QStringList headers = headerLine.split(sep);
    if (headers.size() < 2) {
        if (errorOut) *errorOut = "Need at least one time column and one value column.";
        return false;
    }

    // Detect time-mode: shared if first column starts with "t " or is "t".
    // Per-signal if columns alternate t_<name> / <name>.
    auto isTimeHeader = [](const QString& h){
        const QString t = h.trimmed();
        return t == "t" || t.startsWith("t ") || t.startsWith("t[");
    };
    const bool shared = isTimeHeader(headers[0])
                     && (headers.size() < 3 || !headers[2].trimmed().startsWith("t_"));

    // Build per-signal targets (name, unit, time-column-index, value-column-index).
    struct Target { QString name, unit; int tCol{-1}, vCol{-1}; };
    std::vector<Target> targets;
    if (shared) {
        for (int i = 1; i < headers.size(); ++i) {
            Target t;
            splitNameUnit(headers[i], &t.name, &t.unit);
            t.tCol = 0;
            t.vCol = i;
            if (!t.name.isEmpty()) targets.push_back(std::move(t));
        }
    } else {
        // Expect pairs: t_<name>, <name>. Skip any prefix "t_".
        for (int i = 0; i + 1 < headers.size(); i += 2) {
            Target t;
            splitNameUnit(headers[i + 1], &t.name, &t.unit);
            t.tCol = i;
            t.vCol = i + 1;
            if (!t.name.isEmpty()) targets.push_back(std::move(t));
        }
    }
    if (targets.empty()) {
        if (errorOut) *errorOut = "No data columns found.";
        return false;
    }

    // Per-target accumulators.
    std::vector<std::vector<core::TimestampNs>> tsBufs(targets.size());
    std::vector<std::vector<double>>            vsBufs(targets.size());

    while (!ts.atEnd()) {
        const QString line = ts.readLine();
        if (line.trimmed().isEmpty()) continue;
        const QStringList parts = line.split(sep);
        for (std::size_t k = 0; k < targets.size(); ++k) {
            const Target& tg = targets[k];
            if (tg.tCol >= parts.size() || tg.vCol >= parts.size()) continue;
            QString tStr = parts[tg.tCol].trimmed();
            QString vStr = parts[tg.vCol].trimmed();
            if (tStr.isEmpty() || vStr.isEmpty()) continue;
            // Tolerate both '.' and ',' as decimal separator.
            tStr.replace(',', '.');
            vStr.replace(',', '.');
            bool okT = false, okV = false;
            const double tSec = tStr.toDouble(&okT);
            const double v    = vStr.toDouble(&okV);
            if (!okT || !okV) continue;
            tsBufs[k].push_back(static_cast<core::TimestampNs>(tSec * 1e9));
            vsBufs[k].push_back(v);
        }
    }

    for (std::size_t k = 0; k < targets.size(); ++k) {
        if (tsBufs[k].empty()) continue;
        core::Signal::Meta m;
        m.name = targets[k].name;
        m.unit = targets[k].unit;
        m.dataType = core::DataType::Float64;
        m.sourceSymbol = QString::fromStdString(
            std::filesystem::path(path.toStdString()).filename().string());
        auto sig = std::make_shared<core::Signal>(m);
        sig->append(tsBufs[k].data(),
                    reinterpret_cast<const std::byte*>(vsBufs[k].data()),
                    tsBufs[k].size());
        out->push_back(std::move(sig));
    }
    return true;
}

}  // namespace

void AnalyserPlot::openChartDialog() {
    QFileDialog dlg(this, "Open chart");
    dlg.setAcceptMode(QFileDialog::AcceptOpen);
    dlg.setNameFilters({"Scope chart (*.h5 *.csv *.txt)",
                        "HDF5 (*.h5)",
                        "CSV / text (*.csv *.txt)",
                        "All files (*)"});
    if (dlg.exec() != QDialog::Accepted) return;
    const auto sel = dlg.selectedFiles();
    if (sel.isEmpty()) return;
    const QString path = sel.first();

    std::vector<std::shared_ptr<core::Signal>> loaded;
    QString err;
    if (path.endsWith(".h5", Qt::CaseInsensitive)) {
        auto session = core::Hdf5Session::openForRead(
            std::filesystem::path(path.toStdString()), &err);
        if (!session) {
            QMessageBox::critical(this, "Open failed",
                err.isEmpty() ? QString("Couldn't open file.") : err);
            return;
        }
        loaded = session->loadAllSignals(&err);
    } else {
        if (!loadCsvChart(path, &loaded, &err)) {
            QMessageBox::critical(this, "Open failed", err);
            return;
        }
    }
    if (loaded.empty()) {
        QMessageBox::warning(this, "Empty file",
            err.isEmpty() ? QString("No channels found in this file.") : err);
        return;
    }

    for (auto& s : loaded) {
        auto meta = s->meta();
        meta.name = uniqueStoreName(store_, meta.name);
        s->setMeta(meta);
        store_.add(s);
    }
    QMessageBox::information(this, "Opened",
        QString("Loaded %1 channel(s) from %2")
            .arg(loaded.size()).arg(QFileInfo(path).fileName()));
}

}  // namespace scope::analyser::ui
