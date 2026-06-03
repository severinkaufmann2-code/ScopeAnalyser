#pragma once

#include "scope/core/SignalStore.h"
#include "scope/core/IAdsClient.h"
#include "scope/recorder/RecordingSession.h"

#include <QWidget>

#include <memory>

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
    void onStartClicked();
    void onStopClicked();
    void onChannelOverrun(QString name, std::uint64_t total);
    void onStats(RecordingSession::Stats s);

private:
    scope::core::SignalStore& store_;
    std::unique_ptr<scope::core::IAdsClient> client_;
    std::unique_ptr<RecordingSession> session_;

    class QLineEdit*       netIdEdit_;
    class QSpinBox*        portSpin_;
    class QPushButton*     connectBtn_;
    class QPushButton*     disconnectBtn_;
    class QPushButton*     startBtn_;
    class QPushButton*     stopBtn_;
    class QLabel*          statusLabel_;

    ui::SymbolBrowserWidget* symbols_;
    ui::ChannelTableWidget*  channels_;
    ui::LivePreviewPlot*     preview_;
};

}  // namespace scope::recorder
