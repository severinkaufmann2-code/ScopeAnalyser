#pragma once

#include "scope/core/SignalStore.h"
#include "scope/plot/PlotLayout.h"

#include <QHash>
#include <QStringList>
#include <QWidget>

class QCPGraph;
class QTableWidget;

namespace scope::plot { class ScopePlot; }

namespace scope::recorder::ui {

class LivePreviewPlot : public QWidget {
    Q_OBJECT
public:
    explicit LivePreviewPlot(scope::core::SignalStore& store, QWidget* parent = nullptr);

    void setWindowSeconds(double seconds);

    // Channels whose visibility box is ticked in the live-channels table.
    // Used by the Recorder's "Send checked to Analyser" action.
    QStringList checkedChannelNames() const;

    // The preview plot's own layout (Y axes + channel→axis), as a pure value.
    // The Recorder embeds this inside its .scorec file — file IO lives in the
    // Recorder, not here, so this plot has no link to the Analyser's layout
    // files. Reuses scope::plot::PlotLayout purely as a serialiser.
    scope::plot::PlotLayout currentPlotLayout() const;
    void applyPlotLayout(const scope::plot::PlotLayout& layout);

signals:
    // The sidebar Save/Load buttons only request; the Recorder performs the
    // full save/load (connection + channel table + this plot layout).
    void saveLayoutRequested();
    void loadLayoutRequested();

private slots:
    void onChannelAdded(QString name);
    void onChannelRemoved(QString name);
    void onTick();
    void onAxisComboChanged(int row, int axisIndex);
    void onVisibilityChanged();
    void rebuildAxisCombos();

private:
    void rebuildTable();
    void recolorChannels();
    int  pickAxisForUnit(const QString& unit) const;
    int  axisIndexForRow(int row) const;
    void setAxisIndexForRow(int row, int axisIndex);
    QSet<QString> activeChannels() const;

    scope::core::SignalStore&    store_;
    scope::plot::ScopePlot*      scope_{nullptr};
    QTableWidget*                table_{nullptr};
    double                       windowSeconds_{5.0};
    QHash<QString, QCPGraph*>    graphs_;
    QHash<QString, int>          pendingAssignments_;
};

}  // namespace scope::recorder::ui
