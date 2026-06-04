#include "AnalyserPlot.h"

#include "scope/plot/ScopePlot.h"

#include <qcustomplot.h>

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <array>
#include <limits>

namespace scope::analyser::ui {

namespace {
enum Col { ColVis = 0, ColName, ColAxis, ColCount };

const std::array<QColor, 10> kPalette = {
    QColor(  0,114,189), QColor(217, 83, 25), QColor(237,177, 32),
    QColor(126, 47,142), QColor(119,172, 48), QColor( 77,190,238),
    QColor(162, 20, 47), QColor(  0,128,  0), QColor(255,128,  0),
    QColor(128,  0,128),
};
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
    auto* redrawBtn  = new QPushButton("Redraw",   this);

    auto* axisBtnRow = new QHBoxLayout();
    axisBtnRow->addWidget(addAxisBtn);
    axisBtnRow->addWidget(delAxisBtn);

    auto* leftLayout = new QVBoxLayout();
    leftLayout->addWidget(new QLabel("Channels", this));
    leftLayout->addWidget(table_, /*stretch=*/1);
    leftLayout->addLayout(axisBtnRow);
    leftLayout->addWidget(redrawBtn);

    auto* root = new QHBoxLayout(this);
    root->addLayout(leftLayout);
    root->addWidget(scope_, /*stretch=*/1);

    connect(redrawBtn, &QPushButton::clicked, this, &AnalyserPlot::redrawAll);
    connect(addAxisBtn, &QPushButton::clicked, this, [this]{
        scope_->addYAxis();
        rebuildAxisCombos();
    });
    connect(delAxisBtn, &QPushButton::clicked, this, [this]{
        const int last = scope_->yAxisCount() - 1;
        if (last <= 0) return;  // can't remove Y1
        QString err;
        if (!scope_->removeYAxis(last, &err)) {
            QMessageBox::information(this, "Can't remove", err);
            return;
        }
        rebuildAxisCombos();
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
    QHash<QString, std::pair<bool,int>> prev;  // name → (visible, axis)
    for (int r = 0; r < table_->rowCount(); ++r) {
        const QString name = table_->item(r, ColName) ? table_->item(r, ColName)->text() : QString();
        if (name.isEmpty()) continue;
        bool vis = false;
        if (auto* cb = qobject_cast<QCheckBox*>(table_->cellWidget(r, ColVis)))
            vis = cb->isChecked();
        int axis = 0;
        if (auto* co = qobject_cast<QComboBox*>(table_->cellWidget(r, ColAxis)))
            axis = co->currentIndex();
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
        } else {
            // New channel: auto-assign by unit if a matching axis exists.
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
    scope_->plot()->replot(QCustomPlot::rpQueuedReplot);
}

void AnalyserPlot::onVisibilityChanged(int /*row*/) {
    redrawForActiveChannels();
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

    int colorIdx = 0;
    for (int r = 0; r < table_->rowCount(); ++r) {
        const QString name = table_->item(r, ColName)->text();
        if (!active.contains(name)) continue;
        auto sig = store_.get(name);
        if (!sig) continue;

        int axisIndex = 0;
        if (auto* co = qobject_cast<QComboBox*>(table_->cellWidget(r, ColAxis)))
            axisIndex = co->currentIndex();

        QCPGraph* graph = graphs_.value(name, nullptr);
        if (!graph) {
            graph = plot->addGraph(plot->xAxis, scope_->yAxis(axisIndex));
            graph->setPen(QPen(kPalette[colorIdx % kPalette.size()]));
            graph->setName(name);
            graphs_[name] = graph;
        } else if (graph->valueAxis() != scope_->yAxis(axisIndex)) {
            scope_->setGraphYAxis(graph, axisIndex);
        }
        ++colorIdx;

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

    if (active.isEmpty() || xMin == std::numeric_limits<double>::infinity()) {
        xMin = 0; xMax = 1;
    }
    plot->xAxis->setRange(xMin, xMax);
    scope_->rescaleAllYAxes();
    plot->replot(QCustomPlot::rpQueuedReplot);
}

}  // namespace scope::analyser::ui
