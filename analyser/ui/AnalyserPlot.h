#pragma once

#include "scope/core/SignalStore.h"

#include <QHash>
#include <QWidget>

class QCPGraph;
class QTableWidget;

namespace scope::plot { class ScopePlot; }

namespace scope::analyser {
class FormulaEngine;
}

namespace scope::analyser::ui {

class AnalyserPlot : public QWidget {
    Q_OBJECT
public:
    AnalyserPlot(scope::core::SignalStore& store,
                 FormulaEngine&            engine,
                 QWidget*                  parent = nullptr);

public slots:
    void redrawAll();
    void onChannelAdded(QString name);
    void onChannelRemoved(QString name);
    void saveLayoutDialog();
    void loadLayoutDialog();
    void addChannelDialog();
    void editChannelDialog();
    void saveChartDialog();
    void openChartDialog();

private:
    // The very first call to redrawForActiveChannels with data does an
    // X-range fit so the user isn't stuck on [0, 1]. Subsequent
    // redraws (visibility toggles, axis combo changes, new channels)
    // leave X alone so manual zoom isn't yanked back.
    bool hasDrawnYet_{false};

    // Which signal domain is currently visible in the table + chart.
    scope::core::Signal::Domain viewDomain_{scope::core::Signal::Domain::Time};

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
    FormulaEngine&            engine_;
    QTableWidget*             table_{nullptr};
    scope::plot::ScopePlot*   scope_{nullptr};
    QHash<QString, QCPGraph*> graphs_;

    // Channels referenced by a loaded layout that haven't appeared in the
    // store yet. When the SignalStore adds one of these, we apply the saved
    // axis assignment instead of the unit-match default.
    QHash<QString, int> pendingAssignments_;

    // Per-channel sticky table state (visibility + axis index). Persists
    // across rebuildTable() calls so that toggling the View combo
    // (Time / Frequency) doesn't lose the user's per-channel choices.
    QHash<QString, std::pair<bool, int>> savedRowState_;
};

}  // namespace scope::analyser::ui
