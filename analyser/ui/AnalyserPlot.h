#pragma once

#include "scope/core/SignalStore.h"

#include <QHash>
#include <QWidget>

class QCustomPlot;
class QCPGraph;
class QListWidget;

namespace scope::analyser::ui {

// Multi-channel plot showing any subset of channels in the SignalStore. The
// user toggles channels via a checkable list to the left of the plot.
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
    QListWidget* list_;
    QCustomPlot* plot_;
    QHash<QString, QCPGraph*> graphs_;
};

}  // namespace scope::analyser::ui
