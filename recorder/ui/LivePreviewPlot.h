#pragma once

#include "scope/core/SignalStore.h"

#include <QHash>
#include <QWidget>

#include <unordered_map>

class QCPGraph;

namespace scope::plot { class ScopePlot; }

namespace scope::recorder::ui {

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
    scope::core::SignalStore&    store_;
    scope::plot::ScopePlot*      scope_{nullptr};
    double                       windowSeconds_{5.0};
    std::unordered_map<QString, QCPGraph*> graphs_;
};

}  // namespace scope::recorder::ui
