#pragma once

#include "scope/core/SignalStore.h"

#include <QHash>
#include <QString>
#include <QWidget>

class QCPAbstractPlottable;
class QComboBox;
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

    // What's currently visible in the table + chart.
    //   Time      — time-domain channels, X = timestamps (s)
    //   Frequency — frequency-domain channels, X = Hz
    //   XY        — the user picks an X channel; other same-domain
    //               channels become Y candidates. Curve uses
    //               (xChannel.values, yChannel.values) paired by index
    //               when the timestamp grids match, else linearly
    //               interpolated onto X's grid. Not persisted.
    enum class ViewMode { Time, Frequency, XY };
    ViewMode viewMode_{ViewMode::Time};
    QString  xyChannel_;            // valid only when viewMode_ == XY

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

    // XY: rebuild the X-channel combo from the current store and set
    // it visible only when viewMode_ == XY. Same-domain filter on the
    // table follows from whatever the combo currently points at.
    void rebuildXyXCombo();
    // Plottables for the current view mode are the wrong type after a
    // switch (Graph vs Curve), so wipe them all when the user picks a
    // new mode.
    void clearAllPlottables();

    scope::core::SignalStore& store_;
    FormulaEngine&            engine_;
    QTableWidget*             table_{nullptr};
    scope::plot::ScopePlot*   scope_{nullptr};

    // The XY mode renders curves (non-monotonic X), Time / Frequency
    // render graphs. Track them via the common base so the same map
    // can hold either; the redraw branches cast as needed.
    QHash<QString, QCPAbstractPlottable*> plotted_;

    // XY-only X-axis selector row, hidden in other modes.
    QWidget*   xyXRow_{nullptr};
    QComboBox* xyXCombo_{nullptr};

    // Channels referenced by a loaded layout that haven't appeared in the
    // store yet. When the SignalStore adds one of these, we apply the saved
    // axis assignment instead of the unit-match default.
    QHash<QString, int> pendingAssignments_;

    // Per-channel sticky table state (visibility + axis index). Persists
    // across rebuildTable() calls so that toggling the View combo
    // (Time / Frequency / XY) doesn't lose the user's per-channel choices.
    QHash<QString, std::pair<bool, int>> savedRowState_;
};

}  // namespace scope::analyser::ui
