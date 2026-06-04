#include "LivePreviewPlot.h"

#include "scope/plot/ScopePlot.h"

#include <qcustomplot.h>

#include <QTimer>
#include <QVBoxLayout>

#include <array>
#include <limits>

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
    scope_ = new scope::plot::ScopePlot(this);
    scope_->plot()->xAxis->setLabel("seconds before now");
    scope_->setPauseSupported(true);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0,0,0,0);
    layout->addWidget(scope_);

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
    auto* g = scope_->plot()->addGraph();
    const auto color = kPalette[graphs_.size() % kPalette.size()];
    g->setPen(QPen(color));
    g->setName(name);
    graphs_[name] = g;
}

void LivePreviewPlot::onChannelRemoved(QString name) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return;
    scope_->plot()->removeGraph(it->second);
    graphs_.erase(it);
}

void LivePreviewPlot::onTick() {
    if (scope_->isPaused()) return;
    if (graphs_.empty()) return;

    const auto nowNs    = scope::core::nowNs();
    const auto windowNs = static_cast<scope::core::TimestampNs>(windowSeconds_ * 1e9);
    const auto cutoffNs = nowNs - windowNs;

    double yMin =  std::numeric_limits<double>::infinity();
    double yMax = -std::numeric_limits<double>::infinity();
    bool   any  = false;

    for (auto& [name, graph] : graphs_) {
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
            yMin = std::min(yMin, v);
            yMax = std::max(yMax, v);
            any = true;
        }
        graph->setData(xs, ys, /*alreadySorted=*/true);
    }

    auto* plot = scope_->plot();
    plot->xAxis->setRange(-windowSeconds_, 0.0);
    if (!any) { yMin = -1; yMax = 1; }
    if (yMin == yMax) { yMin -= 0.5; yMax += 0.5; }
    const double margin = 0.05 * (yMax - yMin);
    plot->yAxis->setRange(yMin - margin, yMax + margin);
    plot->replot(QCustomPlot::rpQueuedReplot);
}

}  // namespace scope::recorder::ui
