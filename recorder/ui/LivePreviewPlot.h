#pragma once

#include "scope/core/SignalStore.h"

#include <QHash>
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

public slots:
    void saveLayoutDialog();
    void loadLayoutDialog();

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
