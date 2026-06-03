#include "AnalyserPlot.h"

#include <qcustomplot.h>

#include <QHBoxLayout>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QVBoxLayout>

namespace scope::analyser::ui {

namespace {
const std::array<QColor, 10> kPalette = {
    QColor(  0,114,189), QColor(217, 83, 25), QColor(237,177, 32),
    QColor(126, 47,142), QColor(119,172, 48), QColor( 77,190,238),
    QColor(162, 20, 47), QColor(  0,128,  0), QColor(255,128,  0),
    QColor(128,  0,128),
};
}

AnalyserPlot::AnalyserPlot(scope::core::SignalStore& store, QWidget* parent)
    : QWidget(parent), store_(store) {
    plot_ = new QCustomPlot(this);
    plot_->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom | QCP::iSelectPlottables);
    plot_->xAxis->setLabel("t [s]");
    plot_->legend->setVisible(true);

    list_ = new QListWidget(this);
    list_->setMaximumWidth(260);
    auto* redraw = new QPushButton("Redraw", this);

    auto* leftLayout = new QVBoxLayout();
    leftLayout->addWidget(list_, /*stretch=*/1);
    leftLayout->addWidget(redraw);

    auto* root = new QHBoxLayout(this);
    root->addLayout(leftLayout);
    root->addWidget(plot_, /*stretch=*/1);

    connect(redraw, &QPushButton::clicked, this, &AnalyserPlot::redrawAll);
    connect(list_,  &QListWidget::itemChanged,
            this, [this](QListWidgetItem*){ redrawForActiveChannels(); });
    connect(&store_, &scope::core::SignalStore::channelAdded,
            this, &AnalyserPlot::onChannelAdded);
    connect(&store_, &scope::core::SignalStore::channelRemoved,
            this, &AnalyserPlot::onChannelRemoved);

    rebuildList();
}

void AnalyserPlot::rebuildList() {
    const auto active = activeChannels();
    list_->clear();
    for (const auto& name : store_.channelNames()) {
        auto* item = new QListWidgetItem(name);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(active.contains(name) ? Qt::Checked : Qt::Unchecked);
        list_->addItem(item);
    }
}

QSet<QString> AnalyserPlot::activeChannels() const {
    QSet<QString> out;
    for (int i = 0; i < list_->count(); ++i) {
        auto* item = list_->item(i);
        if (item->checkState() == Qt::Checked) out.insert(item->text());
    }
    return out;
}

void AnalyserPlot::onChannelAdded(QString name) {
    bool exists = false;
    for (int i = 0; i < list_->count(); ++i)
        if (list_->item(i)->text() == name) { exists = true; break; }
    if (exists) return;
    auto* item = new QListWidgetItem(name);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(Qt::Checked);  // newly added → show by default
    list_->addItem(item);
    redrawForActiveChannels();
}

void AnalyserPlot::onChannelRemoved(QString name) {
    for (int i = 0; i < list_->count(); ++i) {
        if (list_->item(i)->text() == name) { delete list_->takeItem(i); break; }
    }
    if (graphs_.contains(name)) {
        plot_->removeGraph(graphs_[name]);
        graphs_.remove(name);
    }
    redrawForActiveChannels();
}

void AnalyserPlot::redrawAll() {
    redrawForActiveChannels();
}

void AnalyserPlot::redrawForActiveChannels() {
    const auto active = activeChannels();

    // Remove graphs no longer active
    for (auto it = graphs_.begin(); it != graphs_.end(); ) {
        if (!active.contains(it.key())) {
            plot_->removeGraph(it.value());
            it = graphs_.erase(it);
        } else { ++it; }
    }

    double xMin = std::numeric_limits<double>::infinity();
    double xMax = -std::numeric_limits<double>::infinity();
    double yMin = std::numeric_limits<double>::infinity();
    double yMax = -std::numeric_limits<double>::infinity();

    int colorIdx = 0;
    for (const auto& name : active) {
        auto sig = store_.get(name);
        if (!sig) continue;

        QCPGraph* graph = graphs_.value(name, nullptr);
        if (!graph) {
            graph = plot_->addGraph();
            graph->setPen(QPen(kPalette[colorIdx % kPalette.size()]));
            graph->setName(name);
            graphs_[name] = graph;
        }
        ++colorIdx;

        auto view = sig->snapshotForRead();
        auto values = sig->readAsDouble();
        QVector<double> xs, ys;
        xs.reserve(static_cast<int>(view.count));
        ys.reserve(static_cast<int>(view.count));

        // Anchor X at first sample to keep the axis values small.
        const double base = view.count > 0 ? view.timestamps[0] / 1e9 : 0.0;
        for (std::size_t i = 0; i < view.count; ++i) {
            const double x = view.timestamps[i] / 1e9 - base;
            xs.push_back(x);
            const double v = (i < values.size()) ? values[i] : 0.0;
            ys.push_back(v);
            xMin = std::min(xMin, x);
            xMax = std::max(xMax, x);
            yMin = std::min(yMin, v);
            yMax = std::max(yMax, v);
        }
        graph->setData(xs, ys, /*alreadySorted=*/true);
    }

    if (active.isEmpty() ||
        xMin == std::numeric_limits<double>::infinity()) {
        xMin = 0; xMax = 1; yMin = -1; yMax = 1;
    }
    if (yMin == yMax) { yMin -= 0.5; yMax += 0.5; }
    const double yMargin = 0.05 * (yMax - yMin);
    plot_->xAxis->setRange(xMin, xMax);
    plot_->yAxis->setRange(yMin - yMargin, yMax + yMargin);
    plot_->replot(QCustomPlot::rpQueuedReplot);
}

}  // namespace scope::analyser::ui
