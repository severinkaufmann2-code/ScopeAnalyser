#include "scope/recorder/RecorderWidget.h"

#include "SymbolBrowserWidget.h"
#include "ChannelTableWidget.h"
#include "LivePreviewPlot.h"

#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QVBoxLayout>

namespace scope::recorder {

RecorderWidget::RecorderWidget(scope::core::SignalStore& store, QWidget* parent)
    : QWidget(parent), store_(store) {
    client_ = scope::core::makeDefaultAdsClient();

    netIdEdit_  = new QLineEdit("127.0.0.1.1.1", this);
    netIdEdit_->setMaximumWidth(180);
    portSpin_   = new QSpinBox(this);
    portSpin_->setRange(1, 65535);
    portSpin_->setValue(851);
    connectBtn_    = new QPushButton("Connect", this);
    disconnectBtn_ = new QPushButton("Disconnect", this);
    disconnectBtn_->setEnabled(false);
    startBtn_   = new QPushButton("Record", this);
    stopBtn_    = new QPushButton("Stop", this);
    stopBtn_->setEnabled(false);
    statusLabel_ = new QLabel("Disconnected", this);

    auto* topBar = new QHBoxLayout();
    topBar->addWidget(new QLabel("AMS NetId:", this));
    topBar->addWidget(netIdEdit_);
    topBar->addWidget(new QLabel("Port:", this));
    topBar->addWidget(portSpin_);
    topBar->addWidget(connectBtn_);
    topBar->addWidget(disconnectBtn_);
    topBar->addStretch();
    topBar->addWidget(startBtn_);
    topBar->addWidget(stopBtn_);
    topBar->addWidget(statusLabel_);

    symbols_   = new ui::SymbolBrowserWidget(this);
    channels_  = new ui::ChannelTableWidget(this);
    preview_   = new ui::LivePreviewPlot(store_, this);

    auto* leftRight = new QSplitter(Qt::Horizontal, this);
    leftRight->addWidget(symbols_);
    leftRight->addWidget(channels_);
    leftRight->setStretchFactor(0, 1);
    leftRight->setStretchFactor(1, 1);

    auto* topBottom = new QSplitter(Qt::Vertical, this);
    topBottom->addWidget(leftRight);
    topBottom->addWidget(preview_);
    topBottom->setStretchFactor(0, 1);
    topBottom->setStretchFactor(1, 2);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(topBar);
    layout->addWidget(topBottom);

    connect(connectBtn_,    &QPushButton::clicked, this, &RecorderWidget::onConnectClicked);
    connect(disconnectBtn_, &QPushButton::clicked, this, &RecorderWidget::onDisconnectClicked);
    connect(startBtn_,      &QPushButton::clicked, this, &RecorderWidget::onStartClicked);
    connect(stopBtn_,       &QPushButton::clicked, this, &RecorderWidget::onStopClicked);
    connect(symbols_, &ui::SymbolBrowserWidget::refreshRequested,    this, &RecorderWidget::onRefreshSymbols);
    connect(symbols_, &ui::SymbolBrowserWidget::addSelectedRequested, this, &RecorderWidget::onAddSelectedSymbols);
}

RecorderWidget::~RecorderWidget() = default;

void RecorderWidget::onConnectClicked() {
    scope::core::AdsRoute route;
    route.netId = netIdEdit_->text().trimmed();
    route.port  = static_cast<std::uint16_t>(portSpin_->value());

    QString err;
    if (!client_->connect(route, &err)) {
        QMessageBox::critical(this, "ADS connect failed", err);
        return;
    }
    statusLabel_->setText("Connected to " + route.netId);
    connectBtn_->setEnabled(false);
    disconnectBtn_->setEnabled(true);
    onRefreshSymbols();
}

void RecorderWidget::onDisconnectClicked() {
    onStopClicked();
    client_->disconnect();
    statusLabel_->setText("Disconnected");
    connectBtn_->setEnabled(true);
    disconnectBtn_->setEnabled(false);
    symbols_->setSymbols({});
}

void RecorderWidget::onRefreshSymbols() {
    if (!client_->isConnected()) return;
    QString err;
    auto syms = client_->listSymbols(&err);
    if (syms.empty() && !err.isEmpty()) {
        QMessageBox::warning(this, "listSymbols", err);
        return;
    }
    symbols_->setSymbols(std::move(syms));
}

void RecorderWidget::onAddSelectedSymbols() {
    if (!client_->isConnected()) return;
    auto selected = symbols_->selectedSymbols();
    for (const auto& sym : selected) {
        const auto cycleUs = client_->taskCycleForSymbol(sym);
        channels_->addChannelFromSymbol(sym, cycleUs);
    }
}

void RecorderWidget::onStartClicked() {
    if (!client_->isConnected()) {
        QMessageBox::information(this, "Not connected", "Connect to a PLC first.");
        return;
    }
    const auto file = QFileDialog::getSaveFileName(
        this, "Save recording session", QString(), "Scope sessions (*.h5)");
    if (file.isEmpty()) return;

    session_ = std::make_unique<RecordingSession>(store_, *client_, this);
    connect(session_.get(), &RecordingSession::statsChanged,
            this, &RecorderWidget::onStats);
    connect(session_.get(), &RecordingSession::channelOverrun,
            this, &RecorderWidget::onChannelOverrun);

    QString err;
    auto cfgs = channels_->buildConfigs();
    if (!session_->addNotifyChannels(cfgs, &err)) {
        QMessageBox::critical(this, "Failed to add channels", err);
        session_.reset();
        return;
    }
    if (!session_->start(file.toStdString(), &err)) {
        QMessageBox::critical(this, "Failed to start", err);
        session_.reset();
        return;
    }
    startBtn_->setEnabled(false);
    stopBtn_->setEnabled(true);
}

void RecorderWidget::onStopClicked() {
    if (!session_) return;
    session_->stop();
    session_.reset();
    startBtn_->setEnabled(true);
    stopBtn_->setEnabled(false);
}

void RecorderWidget::onChannelOverrun(QString name, std::uint64_t total) {
    statusLabel_->setText(QString("Overrun on '%1' (total %2)").arg(name).arg(total));
}

void RecorderWidget::onStats(RecordingSession::Stats s) {
    statusLabel_->setText(QString("Recording: %1 channels, %2 samples, %3 overruns")
                              .arg(s.channels)
                              .arg(s.samplesReceived)
                              .arg(s.overruns));
}

}  // namespace scope::recorder
