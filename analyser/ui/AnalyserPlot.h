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

public:
    enum class DisplayMode { Line, Points, Both };

private:
    void applyDisplayMode();
    void applyDisplayModeTo(QCPGraph* graph) const;

    DisplayMode displayMode_{DisplayMode::Line};


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
};

}  // namespace scope::analyser::ui
