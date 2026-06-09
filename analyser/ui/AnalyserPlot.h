#pragma once

#include "scope/core/SignalStore.h"
#include "scope/plot/PlotLayout.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QWidget>

class QCPAbstractPlottable;
class QComboBox;
class QTableWidget;
class QTimer;

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

    // Coalesced reaction to store activity: channelAdded (a new/replaced
    // source) may feed dependent formula channels, so re-evaluate them via a
    // short single-shot timer that collapses a burst (e.g. opening a
    // multi-channel file) into one recompute.
    void scheduleRecompute();
    QTimer* recomputeTimer_{nullptr};
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

    // Per-domain Y-axis machinery. Each domain (Time / Frequency)
    // remembers its own QList<PlotLayoutAxis> + per-channel vis+axis
    // map. When the user flips the View combo, if the new view's
    // effective domain differs from the current one we snapshot the
    // current Y axes + row state into stateByDomain_[oldDomain] and
    // restore stateByDomain_[newDomain]. XY borrows whichever domain
    // matches its chosen X channel's domain — there's no separate
    // "xy" entry.
    scope::core::Signal::Domain activeDomain() const;
    void snapshotIntoDomain(scope::core::Signal::Domain d);
    void applyFromDomain(scope::core::Signal::Domain d);

    // Pure builders / appliers used by both the dedicated layout dialogs
    // and the chart save/open path (which embeds the layout JSON inside
    // the scope-csv metadata block). currentLayout() snapshots the
    // active domain first so the result reflects what's on screen.
    // applyLayout() does everything loadLayoutDialog does after the
    // file has been parsed — hydrate stateByDomain_, re-eval formulas,
    // switch viewMode, rebuild the table.
    scope::plot::PlotLayout currentLayout();
    void applyLayout(const scope::plot::PlotLayout& layout);

    scope::core::SignalStore& store_;
    FormulaEngine&            engine_;
    QTableWidget*             table_{nullptr};
    scope::plot::ScopePlot*   scope_{nullptr};

    // The XY mode renders curves (non-monotonic X), Time / Frequency
    // render graphs. Track them via the common base so the same map
    // can hold either; the redraw branches cast as needed.
    QHash<QString, QCPAbstractPlottable*> plotted_;

    // View / XY UI handles kept so loadLayoutDialog can re-sync them
    // without re-triggering their slots.
    QComboBox* viewCombo_{nullptr};
    QWidget*   xyXRow_{nullptr};
    QComboBox* xyXCombo_{nullptr};

    // Channels referenced by a loaded layout that haven't appeared in the
    // store yet. When the SignalStore adds one of these, we apply the saved
    // axis assignment instead of the unit-match default.
    QHash<QString, int> pendingAssignments_;

    // Per-channel sticky table state (visibility + axis index). Persists
    // across rebuildTable() calls. Mirrors stateByDomain_[activeDomain].
    // rowState; kept here so the per-row table handlers don't have to
    // recompute the active domain every time.
    QHash<QString, std::pair<bool, int>> savedRowState_;

    // Domain → axes + row state. Switching domains snapshots into one
    // entry and restores from another. See activeDomain() for how a
    // ViewMode maps to a domain.
    struct PerDomainState {
        QList<scope::plot::PlotLayoutAxis>   axes;
        QHash<QString, std::pair<bool, int>> rowState;
    };
    QHash<int /* Signal::Domain */, PerDomainState> stateByDomain_;

    // Test access: the widget-level layout round-trip (currentLayout /
    // applyLayout) can't be driven through the modal Save/Open dialogs, so a
    // gtest fixture befriends us to call them directly.
    friend class AnalyserPlotLayoutTest;
};

}  // namespace scope::analyser::ui
