#pragma once

#include "scope/core/SignalStore.h"

#include <QWidget>

#include <unordered_map>

class QCustomPlot;
class QCPGraph;

namespace scope::recorder::ui {

// Rolling preview of recorded channels. Subscribes to SignalStore::channelAdded
// and re-renders on a 20 Hz timer using the most recent `windowSeconds_` of
// each channel's samples.
class LivePreviewPlot : public QWidget {
    Q_OBJECT
public:
    explicit LivePreviewPlot(scope::core::SignalStore& store, QWidget* parent = nullptr);

    void setWindowSeconds(double seconds);

private slots:
    void onChannelAdded(QString name);
    void onChannelRemoved(QString name);
    void onTick();

private:
    scope::core::SignalStore& store_;
    QCustomPlot* plot_;
    double windowSeconds_{5.0};
    std::unordered_map<QString, QCPGraph*> graphs_;
};

}  // namespace scope::recorder::ui
