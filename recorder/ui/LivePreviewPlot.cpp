#include "LivePreviewPlot.h"

#include <qcustomplot.h>

#include <QVBoxLayout>
#include <QTimer>

namespace scope::recorder::ui {

namespace {
constexpr int kTickIntervalMs = 50;  // 20 Hz
const std::array<QColor, 8> kPalette = {
    QColor(  0,114,189), QColor(217, 83, 25), QColor(237,177, 32),
    QColor(126, 47,142), QColor(119,172, 48), QColor( 77,190,238),
    QColor(162, 20, 47), QColor(  0,  0,  0),
};
}

LivePreviewPlot::LivePreviewPlot(scope::core::SignalStore& store, QWidget* parent)
    : QWidget(parent), store_(store) {
    plot_ = new QCustomPlot(this);
    plot_->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    plot_->xAxis->setLabel("t [s]");

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0,0,0,0);
    layout->addWidget(plot_);

    connect(&store_, &scope::core::SignalStore::channelAdded,
            this, &LivePreviewPlot::onChannelAdded);
    connect(&store_, &scope::core::SignalStore::channelRemoved,
            this, &LivePreviewPlot::onChannelRemoved);

    auto* tick = new QTimer(this);
    connect(tick, &QTimer::timeout, this, &LivePreviewPlot::onTick);
    tick->start(kTickIntervalMs);
}

void LivePreviewPlot::setWindowSeconds(double seconds) { windowSeconds_ = seconds; }

void LivePreviewPlot::onChannelAdded(QString name) {
    if (graphs_.contains(name)) return;
    auto* g = plot_->addGraph();
    const auto color = kPalette[graphs_.size() % kPalette.size()];
    g->setPen(QPen(color));
    g->setName(name);
    graphs_[name] = g;
}

void LivePreviewPlot::onChannelRemoved(QString name) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return;
    plot_->removeGraph(it->second);
    graphs_.erase(it);
}

void LivePreviewPlot::onTick() {
    if (graphs_.empty()) return;

    const auto nowNs = scope::core::nowNs();
    const auto windowNs = static_cast<scope::core::TimestampNs>(windowSeconds_ * 1e9);
    const auto cutoffNs = nowNs - windowNs;

    double xMin = 0.0, xMax = 1.0;
    double yMin =  std::numeric_limits<double>::infinity();
    double yMax = -std::numeric_limits<double>::infinity();

    for (auto& [name, graph] : graphs_) {
        auto sig = store_.get(name);
        if (!sig) continue;
        auto view = sig->snapshotForRead();
        auto values = sig->readAsDouble();  // simple v1; optimize later

        QVector<double> xs, ys;
        xs.reserve(static_cast<int>(view.count));
        ys.reserve(static_cast<int>(view.count));
        for (std::size_t i = 0; i < view.count; ++i) {
            if (view.timestamps[i] < cutoffNs) continue;
            xs.push_back(view.timestamps[i] / 1e9);
            const double v = (i < values.size()) ? values[i] : 0.0;
            ys.push_back(v);
            yMin = std::min(yMin, v);
            yMax = std::max(yMax, v);
        }
        if (!xs.isEmpty()) {
            xMin = std::min(xMin, xs.first());
            xMax = std::max(xMax, xs.last());
        }
        graph->setData(xs, ys, /*alreadySorted=*/true);
    }

    if (yMin == std::numeric_limits<double>::infinity()) { yMin = -1; yMax = 1; }
    plot_->xAxis->setRange(xMin, xMax);
    plot_->yAxis->setRange(yMin - 0.05*(yMax-yMin) - 1e-9, yMax + 0.05*(yMax-yMin) + 1e-9);
    plot_->replot(QCustomPlot::rpQueuedReplot);
}

}  // namespace scope::recorder::ui
