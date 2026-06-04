#pragma once

#include "scope/core/SignalStore.h"

#include <QHash>
#include <QWidget>

class QCPGraph;
class QTableWidget;

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
    void saveLayoutDialog();
    void loadLayoutDialog();

private:
    void rebuildTable();
    void redrawForActiveChannels();
    void rebuildAxisCombos();
    void onAxisComboChanged(int row, int axisIndex);
    void onVisibilityChanged(int row);
    void recolorChannels();
    QSet<QString> activeChannels() const;
    int  pickAxisForUnit(const QString& unit) const;
    int  axisIndexForRow(int row) const;
    void setAxisIndexForRow(int row, int axisIndex);

    scope::core::SignalStore& store_;
    QTableWidget*             table_{nullptr};
    scope::plot::ScopePlot*   scope_{nullptr};
    QHash<QString, QCPGraph*> graphs_;

    // Channels referenced by a loaded layout that haven't appeared in the
    // store yet. When the SignalStore adds one of these, we apply the saved
    // axis assignment instead of the unit-match default.
    QHash<QString, int> pendingAssignments_;

    // When true, each channel renders with its own first sample at x = 0
    // instead of using the shared min-first-sample base. Useful when
    // comparing recordings with very different absolute timestamps.
    bool alignStarts_{false};
};

}  // namespace scope::analyser::ui
