#pragma once

#include "scope/core/SignalStore.h"

#include <QHash>
#include <QWidget>

class QCPGraph;
class QTableWidget;

namespace scope::plot { class ScopePlot; }

namespace scope::analyser::ui {

// Multi-channel plot with per-channel visibility and Y-axis assignment.
class AnalyserPlot : public QWidget {
    Q_OBJECT
public:
    explicit AnalyserPlot(scope::core::SignalStore& store, QWidget* parent = nullptr);

public slots:
    void redrawAll();
    void onChannelAdded(QString name);
    void onChannelRemoved(QString name);

private:
    void rebuildTable();
    void redrawForActiveChannels();
    void rebuildAxisCombos();
    void onAxisComboChanged(int row, int axisIndex);
    void onVisibilityChanged(int row);
    QSet<QString> activeChannels() const;
    int  pickAxisForUnit(const QString& unit) const;

    scope::core::SignalStore& store_;
    QTableWidget*             table_{nullptr};
    scope::plot::ScopePlot*   scope_{nullptr};
    QHash<QString, QCPGraph*> graphs_;
};

}  // namespace scope::analyser::ui
