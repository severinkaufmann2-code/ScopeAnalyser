#include "scope/recorder/RecorderWidget.h"

#include "SymbolBrowserWidget.h"
#include "ChannelTableWidget.h"
#include "LivePreviewPlot.h"

#include "scope/ads/MockAdsClient.h"
#include "scope/style/StyleKit.h"

#include <QComboBox>
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

    sourceCombo_ = new QComboBox(this);
    sourceCombo_->addItem("Demo (Mock)");
    sourceCombo_->addItem("ADS over TCP");
    hostLabel_ = new QLabel("Target IP/host:", this);
    hostEdit_  = new QLineEdit("127.0.0.1", this);
    hostEdit_->setMaximumWidth(140);
    hostEdit_->setToolTip("IP or hostname where the PLC's ADS router listens "
                          "(TCP 48898). For a local TwinCAT use 127.0.0.1.");
    netIdLabel_ = new QLabel("AMS NetId:", this);
    netIdEdit_  = new QLineEdit(this);
    netIdEdit_->setMaximumWidth(160);
    netIdEdit_->setPlaceholderText("local PLC");
    netIdEdit_->setToolTip("Target PLC's AMS NetId (NOT its IP), e.g. "
                           "5.123.45.67.1.1. Leave blank to use the local "
                           "TwinCAT on the host above.");
    portLabel_  = new QLabel("Port:", this);
    portSpin_   = new QSpinBox(this);
    portSpin_->setRange(1, 65535);
    portSpin_->setValue(851);
    connectBtn_ = new QPushButton(
        scope::style::icon(scope::style::Glyph::Plug, Qt::white), "Connect", this);
    connectBtn_->setProperty("accent", true);
    disconnectBtn_ = new QPushButton(
        scope::style::icon(scope::style::Glyph::Unplug), "Disconnect", this);
    disconnectBtn_->setEnabled(false);
    startBtn_ = new QPushButton(
        scope::style::icon(scope::style::Glyph::RecordDot, Qt::white), "Record", this);
    startBtn_->setProperty("danger", true);
    startBtn_->setEnabled(false);
    startBtn_->setToolTip(
        "Record the channels in the table into an .h5 session file.\n"
        "Enabled once a source is connected.");
    stopBtn_ = new QPushButton(
        scope::style::icon(scope::style::Glyph::Stop), "Stop", this);
    stopBtn_->setEnabled(false);
    statusLabel_ = new QLabel(this);
    statusLabel_->setProperty("scopeRole", "dim");
    connPill_ = scope::style::makePill(this);
    scope::style::setPill(connPill_, scope::style::PillTone::Neutral, "Disconnected");

    auto* srcLabel = new QLabel("Source:", this);
    srcLabel->setProperty("scopeRole", "dim");
    auto* connRow = new QHBoxLayout();
    connRow->setSpacing(6);
    connRow->addWidget(srcLabel);
    connRow->addWidget(sourceCombo_);
    connRow->addSpacing(8);
    connRow->addWidget(hostLabel_);
    connRow->addWidget(hostEdit_);
    connRow->addWidget(netIdLabel_);
    connRow->addWidget(netIdEdit_);
    connRow->addWidget(portLabel_);
    connRow->addWidget(portSpin_);
    connRow->addSpacing(8);
    connRow->addWidget(connectBtn_);
    connRow->addWidget(disconnectBtn_);
    connRow->addStretch();
    connRow->addWidget(connPill_);

    auto* captureRow = new QHBoxLayout();
    captureRow->setSpacing(6);
    captureRow->addWidget(startBtn_);
    captureRow->addWidget(stopBtn_);
    captureRow->addSpacing(10);
    captureRow->addWidget(statusLabel_);
    captureRow->addStretch();

    auto* header = new QWidget(this);
    scope::style::applyToolbarStrip(header);
    auto* headerLayout = new QVBoxLayout(header);
    headerLayout->setContentsMargins(8, 6, 8, 6);
    headerLayout->setSpacing(4);
    headerLayout->addLayout(connRow);
    headerLayout->addLayout(captureRow);

    auto applySourceVisibility = [this]{
        const bool isAds = sourceCombo_->currentText() == "ADS over TCP";
        hostLabel_->setVisible(isAds);
        hostEdit_->setVisible(isAds);
        netIdLabel_->setVisible(isAds);
        netIdEdit_->setVisible(isAds);
        portLabel_->setVisible(isAds);
        portSpin_->setVisible(isAds);
    };
    applySourceVisibility();
    connect(sourceCombo_, &QComboBox::currentTextChanged,
            this, [applySourceVisibility](const QString&){ applySourceVisibility(); });

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
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(header);
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
    if (sourceCombo_->currentText() == "Demo (Mock)") {
        client_ = scope::ads::makeMockAdsClient();
    } else {
        client_ = scope::core::makeDefaultAdsClient();
    }

    scope::core::AdsRoute route;
    route.host  = hostEdit_->text().trimmed();
    route.netId = netIdEdit_->text().trimmed();
    route.port  = static_cast<std::uint16_t>(portSpin_->value());

    QString err;
    if (!client_->connect(route, &err)) {
        QMessageBox::critical(this, "Connect failed", err);
        client_.reset();
        return;
    }
    if (sourceCombo_->currentText() == "Demo (Mock)") {
        connectedLabel_ = "Connected — Demo";
    } else {
        connectedLabel_ = QString("Connected — %1")
            .arg(route.netId.isEmpty() ? route.host : route.netId);
    }
    scope::style::setPill(connPill_, scope::style::PillTone::Ok, connectedLabel_);
    statusLabel_->clear();
    connectBtn_->setEnabled(false);
    disconnectBtn_->setEnabled(true);
    startBtn_->setEnabled(true);
    onRefreshSymbols();
}

void RecorderWidget::onDisconnectClicked() {
    onStopClicked();
    if (client_) client_->disconnect();
    client_.reset();
    scope::style::setPill(connPill_, scope::style::PillTone::Neutral, "Disconnected");
    statusLabel_->clear();
    connectBtn_->setEnabled(true);
    disconnectBtn_->setEnabled(false);
    startBtn_->setEnabled(false);
    symbols_->setSymbols({});
}

void RecorderWidget::onRefreshSymbols() {
    if (!client_ || !client_->isConnected()) return;
    QString err;
    auto syms = client_->listSymbols(&err);
    if (syms.empty() && !err.isEmpty()) {
        QMessageBox::warning(this, "listSymbols", err);
        return;
    }
    symbols_->setSymbols(std::move(syms));
}

void RecorderWidget::onAddSelectedSymbols() {
    if (!client_ || !client_->isConnected()) return;
    auto selected = symbols_->selectedSymbols();
    for (const auto& sym : selected) {
        const auto cycleUs = client_->taskCycleForSymbol(sym);
        channels_->addChannelFromSymbol(sym, cycleUs);
    }
}

void RecorderWidget::onStartClicked() {
    if (!client_ || !client_->isConnected()) {
        QMessageBox::information(this, "Not connected", "Connect to a source first.");
        return;
    }
    // Non-static QFileDialog + setDefaultSuffix("h5") so the existence-
    // check runs against the real .h5 target and ".h5" is appended if
    // the user didn't type it. Same trick as Save converter profile /
    // Save plot layout.
    QFileDialog dlg(this, "Save recording session");
    dlg.setAcceptMode(QFileDialog::AcceptSave);
    dlg.setNameFilters({"Scope sessions (*.h5)", "All files (*)"});
    dlg.setDefaultSuffix("h5");
    if (dlg.exec() != QDialog::Accepted) return;
    const auto sel = dlg.selectedFiles();
    if (sel.isEmpty()) return;
    QString file = sel.first();
    if (!file.endsWith(".h5", Qt::CaseInsensitive)) file += ".h5";

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
    scope::style::setPill(connPill_, scope::style::PillTone::Rec, "● Recording");
}

void RecorderWidget::onStopClicked() {
    if (!session_) return;
    session_->stop();
    session_.reset();
    const bool connected = client_ && client_->isConnected();
    startBtn_->setEnabled(connected);
    stopBtn_->setEnabled(false);
    if (connected) {
        scope::style::setPill(connPill_, scope::style::PillTone::Ok, connectedLabel_);
    } else {
        scope::style::setPill(connPill_, scope::style::PillTone::Neutral, "Disconnected");
    }
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
