#pragma once

#include "scope/core/SignalStore.h"
#include "scope/core/IAdsClient.h"
#include "scope/core/UndoStack.h"
#include "scope/recorder/RecordingSession.h"

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <memory>

class QLineEdit;
class QSpinBox;
class QPushButton;
class QLabel;
class QComboBox;
class QCheckBox;
class QTimer;

namespace scope::recorder::ui {
class SymbolBrowserWidget;
class ChannelTableWidget;
class LivePreviewPlot;
}

namespace scope::recorder {

// Top-level Recorder UI: connection bar, symbol browser, channel table, live
// preview, and the start/stop/save controls.
class RecorderWidget : public QWidget {
    Q_OBJECT
public:
    explicit RecorderWidget(scope::core::SignalStore& store, QWidget* parent = nullptr);
    ~RecorderWidget();

private slots:
    void onConnectClicked();
    void onDisconnectClicked();
    void onRefreshSymbols();
    void onAddSelectedSymbols();
    void onAddSymbolByName(const QString& name);
    void onStartClicked();
    void onStopClicked();
    void onChannelOverrun(QString name, std::uint64_t total);
    void onStats(RecordingSession::Stats s);
    void onSendAll();
    void onSendSelected();
    void onSaveChart();
    void onSaveLayout();
    void onLoadLayout();

private:
    // Copy the named recorded channels from recordStore_ into the shared
    // store_ (the Analyser / Converter bus). Snapshots a frozen copy and
    // replaces any copy a previous Send pushed for the same channel.
    void sendChannels(const QStringList& names);
    // Enable/disable the Send + Save-chart buttons based on whether the
    // private capture store has anything in it.
    void updateActionButtons();

    // ---- Undo / redo --------------------------------------------------
    // The editable recorder "document" is the connection params + channel
    // table + preview axes. captureConfig() serialises that to JSON (the same
    // payload onSaveLayout writes); applyConfig() restores it. The undo stack
    // snapshots after each settled edit. Live actions (connect, record,
    // Send-to-Analyser) are NOT part of the document and never snapshot.
    QJsonObject captureConfig();
    void        applyConfig(const QJsonObject& root);
    void        scheduleCommit();      // coalesce a burst of edits into one snapshot
    void        refreshUndoButtons();  // enable/disable per stack state

    QPushButton* undoBtn_{nullptr};
    QPushButton* redoBtn_{nullptr};
    QTimer*      commitTimer_{nullptr};
    std::unique_ptr<scope::core::UndoStack<QJsonObject>> undo_;

    // The shared store is the Analyser / Converter bus. The Recorder keeps
    // its captures in a PRIVATE store so nothing reaches the Analyser until
    // the user presses a Send button.
    scope::core::SignalStore& store_;
    scope::core::SignalStore  recordStore_;
    // recorder channel name → the (unique) name it was last sent under in
    // store_. Lets a re-Send replace its earlier copy instead of duplicating.
    QHash<QString, QString>   appliedMap_;

    std::unique_ptr<scope::core::IAdsClient> client_;
    std::unique_ptr<RecordingSession> session_;

    QComboBox*       sourceCombo_;
    QLabel*          hostLabel_;
    QLineEdit*       hostEdit_;
    QLabel*          netIdLabel_;
    QLineEdit*       netIdEdit_;
    QLabel*          portLabel_;
    QSpinBox*        portSpin_;
    QPushButton*     connectBtn_;
    QPushButton*     disconnectBtn_;
    QPushButton*     startBtn_;
    QPushButton*     stopBtn_;
    QPushButton*     saveChartBtn_;     // export captures (HDF5/MDF4/CSV/JSON/HTML)
    QPushButton*     sendAllBtn_;       // icon-only "Send to Analyser" (all, or — with
                                        // onlySelectedCheck_ ticked — only checked)
    QCheckBox*       onlySelectedCheck_; // gate Send to only the checked channels
    QLabel*          statusLabel_;   // live recording stats / warnings
    QLabel*          connPill_;      // connection state chip
    QString          connectedLabel_;  // pill text while connected

    ui::SymbolBrowserWidget* symbols_;
    ui::ChannelTableWidget*  channels_;
    ui::LivePreviewPlot*     preview_;
};

}  // namespace scope::recorder
