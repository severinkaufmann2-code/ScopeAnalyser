#pragma once

#include "scope/converter/HtmlExport.h"
#include "scope/core/SignalStore.h"
#include "scope/core/Types.h"
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

    // Re-apply the last-loaded layout's channel→axis assignments to the
    // channels present now. A layout is usually loaded before any channel
    // exists (no recording yet), so the assignment is deferred; the Recorder
    // calls this once recording has started and the channels appear.
    void reapplyChannelAssignments();

    // Recording lifecycle. While recording, the preview follows the latest
    // window (newest on the right) unless the user zooms/pans or pauses; on
    // stop it fits the whole capture. While not recording the tick leaves the
    // view untouched so the user can inspect the finished signal.
    void setRecording(bool on);

    // Time-domain HTML export view mirroring the preview (axes / colours /
    // channel→axis), for the Recorder's "Save chart… → HTML".
    scope::converter::HtmlExportView htmlExportView() const;

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
    // Load the full signal of every graph into the plot (x = t − originNs_).
    void refreshData();

    scope::core::SignalStore&    store_;
    scope::plot::ScopePlot*      scope_{nullptr};
    QTableWidget*                table_{nullptr};
    double                       windowSeconds_{5.0};
    QHash<QString, QCPGraph*>    graphs_;
    QHash<QString, int>          pendingAssignments_;

    // Live-capture state. originNs_ is a fixed time reference captured from
    // the first samples so x doesn't drift between ticks; recording_ gates
    // the follow/refresh in onTick. loadedLayout_ is the last layout applied,
    // re-applied to channels once they appear (reapplyChannelAssignments).
    bool                         recording_{false};
    bool                         originSet_{false};
    scope::core::TimestampNs     originNs_{0};
    scope::plot::PlotLayout      loadedLayout_;
};

}  // namespace scope::recorder::ui
