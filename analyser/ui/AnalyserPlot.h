#pragma once

#include "scope/core/SignalStore.h"

#include <QHash>
#include <QWidget>

class QCPGraph;
class QListWidget;

namespace scope::plot { class ScopePlot; }

namespace scope::analyser::ui {

class AnalyserPlot : public QWidget {
    Q_OBJECT
public:
    explicit AnalyserPlot(scope::core::SignalStore& store, QWidget* parent = nullptr);

public slots:
    void redrawAll();
    void onChannelAdded(QString name);
    void onChannelRemoved(QString name);

private:
    void rebuildList();
    void redrawForActiveChannels();
    QSet<QString> activeChannels() const;

    scope::core::SignalStore& store_;
    QListWidget*              list_{nullptr};
    scope::plot::ScopePlot*   scope_{nullptr};
    QHash<QString, QCPGraph*> graphs_;
};

}  // namespace scope::analyser::ui
