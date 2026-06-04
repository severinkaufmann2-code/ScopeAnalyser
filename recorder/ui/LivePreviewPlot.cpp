#include "LivePreviewPlot.h"

#include "scope/plot/ScopePlot.h"

#include <qcustomplot.h>

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

#include <array>
#include <limits>

namespace scope::recorder::ui {

namespace {
enum Col { ColVis = 0, ColName, ColAxis, ColCount };
constexpr int kTickIntervalMs = 50;

const std::array<QColor, 8> kPalette = {
    QColor(  0,114,189), QColor(217, 83, 25), QColor(237,177, 32),
    QColor(126, 47,142), QColor(119,172, 48), QColor( 77,190,238),
    QColor(162, 20, 47), QColor(  0,  0,  0),
};
}

LivePreviewPlot::LivePreviewPlot(scope::core::SignalStore& store, QWidget* parent)
    : QWidget(parent), store_(store) {
    scope_ = new scope::plot::ScopePlot(this);
    scope_->plot()->xAxis->setLabel("seconds before now");
    scope_->setPauseSupported(true);

    table_ = new QTableWidget(0, ColCount, this);
    table_->setHorizontalHeaderLabels({"", "Channel", "Axis"});
    table_->horizontalHeader()->setSectionResizeMode(ColName, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(ColVis,  QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(ColAxis, QHeaderView::ResizeToContents);
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setMaximumWidth(280);

    auto* addAxisBtn = new QPushButton("+ Y axis", this);
    auto* delAxisBtn = new QPushButton("− Y axis", this);

    auto* axisBtnRow = new QHBoxLayout();
    axisBtnRow->addWidget(addAxisBtn);
    axisBtnRow->addWidget(delAxisBtn);

    auto* sidebar = new QVBoxLayout();
    sidebar->addWidget(new QLabel("Channels", this));
    sidebar->addWidget(table_, /*stretch=*/1);
    sidebar->addLayout(axisBtnRow);

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0,0,0,0);
    root->addLayout(sidebar);
    root->addWidget(scope_, /*stretch=*/1);

    connect(&store_, &scope::core::SignalStore::channelAdded,
            this, &LivePreviewPlot::onChannelAdded);
    connect(&store_, &scope::core::SignalStore::channelRemoved,
            this, &LivePreviewPlot::onChannelRemoved);
    connect(scope_, &scope::plot::ScopePlot::yAxesChanged,
            this, &LivePreviewPlot::rebuildAxisCombos);
    connect(addAxisBtn, &QPushButton::clicked, this, [this]{
        scope_->addYAxis();
    });
    connect(delAxisBtn, &QPushButton::clicked, this, [this]{
        const int last = scope_->yAxisCount() - 1;
        if (last <= 0) return;
        QString err;
        if (!scope_->removeYAxis(last, &err)) {
            QMessageBox::information(this, "Can't remove", err);
        }
    });

    auto* tick = new QTimer(this);
    connect(tick, &QTimer::timeout, this, &LivePreviewPlot::onTick);
    tick->start(kTickIntervalMs);
}

void LivePreviewPlot::setWindowSeconds(double seconds) { windowSeconds_ = seconds; }

int LivePreviewPlot::pickAxisForUnit(const QString& unit) const {
    if (unit.isEmpty()) return 0;
    for (int i = 0; i < scope_->yAxisCount(); ++i) {
        auto* ax = scope_->yAxis(i);
        if (ax && ax->label().contains(unit, Qt::CaseInsensitive)) return i;
    }
    return 0;
}

QSet<QString> LivePreviewPlot::activeChannels() const {
    QSet<QString> out;
    for (int r = 0; r < table_->rowCount(); ++r) {
        if (auto* cb = qobject_cast<QCheckBox*>(table_->cellWidget(r, ColVis))) {
            if (cb->isChecked()) out.insert(table_->item(r, ColName)->text());
        }
    }
    return out;
}

void LivePreviewPlot::rebuildTable() {
    QHash<QString, std::pair<bool,int>> prev;
    for (int r = 0; r < table_->rowCount(); ++r) {
        const QString name = table_->item(r, ColName) ? table_->item(r, ColName)->text() : QString();
        if (name.isEmpty()) continue;
        bool vis = true; int axis = 0;
        if (auto* cb = qobject_cast<QCheckBox*>(table_->cellWidget(r, ColVis))) vis = cb->isChecked();
        if (auto* co = qobject_cast<QComboBox*>(table_->cellWidget(r, ColAxis))) axis = co->currentIndex();
        prev[name] = {vis, axis};
    }

    table_->setRowCount(0);
    for (const auto& name : store_.channelNames()) {
        const int r = table_->rowCount();
        table_->insertRow(r);

        auto* vis = new QCheckBox();
        bool defaultVis = true; int defaultAxis = 0;
        if (prev.contains(name)) {
            defaultVis  = prev[name].first;
            defaultAxis = prev[name].second;
        } else {
            auto s = store_.get(name);
            if (s) defaultAxis = pickAxisForUnit(s->meta().unit);
        }
        vis->setChecked(defaultVis);
        table_->setCellWidget(r, ColVis, vis);
        connect(vis, &QCheckBox::toggled, this, [this](bool){ onVisibilityChanged(); });

        auto* nameItem = new QTableWidgetItem(name);
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
        table_->setItem(r, ColName, nameItem);

        auto* combo = new QComboBox();
        for (int a = 0; a < scope_->yAxisCount(); ++a)
            combo->addItem(scope_->yAxis(a)->label());
        combo->setCurrentIndex(std::min(defaultAxis, combo->count() - 1));
        table_->setCellWidget(r, ColAxis, combo);
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this, r](int idx){ onAxisComboChanged(r, idx); });
    }
}

void LivePreviewPlot::rebuildAxisCombos() {
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

void LivePreviewPlot::onAxisComboChanged(int row, int axisIndex) {
    if (row < 0 || row >= table_->rowCount()) return;
    const QString name = table_->item(row, ColName)->text();
    if (!graphs_.contains(name)) return;
    scope_->setGraphYAxis(graphs_[name], axisIndex);
    scope_->plot()->replot(QCustomPlot::rpQueuedReplot);
}

void LivePreviewPlot::onVisibilityChanged() {
    const auto active = activeChannels();
    for (auto it = graphs_.begin(); it != graphs_.end(); ++it) {
        it.value()->setVisible(active.contains(it.key()));
    }
    scope_->plot()->replot(QCustomPlot::rpQueuedReplot);
}

void LivePreviewPlot::onChannelAdded(QString name) {
    if (graphs_.contains(name)) return;
    auto* plot = scope_->plot();
    auto sig = store_.get(name);
    const int axisIdx = sig ? pickAxisForUnit(sig->meta().unit) : 0;
    auto* g = plot->addGraph(plot->xAxis, scope_->yAxis(axisIdx));
    const auto color = kPalette[graphs_.size() % kPalette.size()];
    g->setPen(QPen(color));
    g->setName(name);
    graphs_[name] = g;
    rebuildTable();
}

void LivePreviewPlot::onChannelRemoved(QString name) {
    auto it = graphs_.find(name);
    if (it != graphs_.end()) {
        scope_->plot()->removeGraph(it.value());
        graphs_.erase(it);
    }
    rebuildTable();
}

void LivePreviewPlot::onTick() {
    if (scope_->isPaused()) return;
    if (graphs_.empty()) return;

    const auto nowNs    = scope::core::nowNs();
    const auto windowNs = static_cast<scope::core::TimestampNs>(windowSeconds_ * 1e9);
    const auto cutoffNs = nowNs - windowNs;

    const auto active = activeChannels();

    for (auto it = graphs_.begin(); it != graphs_.end(); ++it) {
        const QString& name = it.key();
        QCPGraph*      graph = it.value();
        if (!active.contains(name)) continue;
        auto sig = store_.get(name);
        if (!sig) continue;
        auto view = sig->snapshotForRead();
        auto values = sig->readAsDouble();

        QVector<double> xs, ys;
        xs.reserve(static_cast<int>(view.count));
        ys.reserve(static_cast<int>(view.count));
        for (std::size_t i = 0; i < view.count; ++i) {
            if (view.timestamps[i] < cutoffNs) continue;
            xs.push_back((view.timestamps[i] - nowNs) / 1e9);
            const double v = (i < values.size()) ? values[i] : 0.0;
            ys.push_back(v);
        }
        graph->setData(xs, ys, /*alreadySorted=*/true);
    }

    auto* plot = scope_->plot();
    plot->xAxis->setRange(-windowSeconds_, 0.0);
    scope_->rescaleAllYAxes();
    plot->replot(QCustomPlot::rpQueuedReplot);
}

}  // namespace scope::recorder::ui
