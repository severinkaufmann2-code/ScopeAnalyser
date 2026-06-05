#include "AnalyserPlot.h"

#include "scope/plot/ScopePlot.h"
#include "scope/plot/PlotLayout.h"

#include <qcustomplot.h>
#include <spdlog/spdlog.h>

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <filesystem>
#include <limits>

namespace scope::analyser::ui {

namespace {
enum Col { ColVis = 0, ColName, ColAxis, ColCount };
}

AnalyserPlot::AnalyserPlot(scope::core::SignalStore& store, QWidget* parent)
    : QWidget(parent), store_(store) {
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

    auto* addAxisBtn = new QPushButton("+ Y axis", this);
    auto* delAxisBtn = new QPushButton("− Y axis", this);
    auto* saveBtn    = new QPushButton("Save layout…", this);
    auto* loadBtn    = new QPushButton("Load layout…", this);
    auto* redrawBtn  = new QPushButton("Redraw",   this);

    auto* axisBtnRow = new QHBoxLayout();
    axisBtnRow->addWidget(addAxisBtn);
    axisBtnRow->addWidget(delAxisBtn);
    auto* layoutBtnRow = new QHBoxLayout();
    layoutBtnRow->addWidget(saveBtn);
    layoutBtnRow->addWidget(loadBtn);

    auto* leftLayout = new QVBoxLayout();
    leftLayout->addWidget(new QLabel("Channels", this));
    leftLayout->addWidget(table_, /*stretch=*/1);
    leftLayout->addLayout(axisBtnRow);
    leftLayout->addLayout(layoutBtnRow);
    leftLayout->addWidget(redrawBtn);

    auto* root = new QHBoxLayout(this);
    root->addLayout(leftLayout);
    root->addWidget(scope_, /*stretch=*/1);

    connect(redrawBtn,  &QPushButton::clicked, this, &AnalyserPlot::redrawAll);
    connect(saveBtn,    &QPushButton::clicked, this, &AnalyserPlot::saveLayoutDialog);
    connect(loadBtn,    &QPushButton::clicked, this, &AnalyserPlot::loadLayoutDialog);
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

        // Diagnostic: dump the first/last few samples so we can tell whether
        // a "bars to 0" rendering comes from the *data* (alternating
        // value/0 pattern) or from the plot style. Logged via spdlog at
        // info level — remove once the speed-derivative bug is closed.
        if (view.count > 0) {
            const std::size_t n = std::min<std::size_t>(view.count, 8);
            QString first;
            for (std::size_t i = 0; i < n; ++i) {
                first += QString("(%1, %2) ").arg(xs[i], 0, 'g', 6)
                                              .arg(ys[i], 0, 'g', 6);
            }
            spdlog::info("AnalyserPlot draw '{}' count={} first={}",
                         name.toStdString(), view.count, first.toStdString());
        }
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

}  // namespace scope::analyser::ui
