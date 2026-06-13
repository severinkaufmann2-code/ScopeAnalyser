#include "scope/recorder/RecorderWidget.h"

#include "SymbolBrowserWidget.h"
#include "ChannelTableWidget.h"
#include "LivePreviewPlot.h"

#include "scope/ads/MockAdsClient.h"
#include "scope/plot/PlotLayout.h"
#include "scope/style/StyleKit.h"

#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QVBoxLayout>

#include <cstdint>
#include <memory>
#include <vector>

namespace scope::recorder {

namespace {
// A name not yet present in `store`, suffixing " (2)", " (3)", … on clash.
// Matches the Converter's import naming so recorder sends never clobber a
// channel the Analyser / Converter already owns.
QString uniqueStoreName(const scope::core::SignalStore& store, const QString& base) {
    if (!store.contains(base)) return base;
    for (int i = 2; i < 10000; ++i) {
        const QString c = QString("%1 (%2)").arg(base).arg(i);
        if (!store.contains(c)) return c;
    }
    return base + " (?)";
}
}  // namespace

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
    sendSelectedBtn_ = new QPushButton(
        scope::style::icon(scope::style::Glyph::AnalyseTab),
        "Send checked to Analyser", this);
    sendSelectedBtn_->setEnabled(false);
    sendSelectedBtn_->setToolTip(
        "Send only the channels ticked in the live list to the Analyser "
        "(and Converter). Nothing is sent automatically.");
    sendAllBtn_ = new QPushButton(
        scope::style::icon(scope::style::Glyph::AnalyseTab, Qt::white),
        "Send to Analyser", this);
    sendAllBtn_->setProperty("accent", true);
    sendAllBtn_->setEnabled(false);
    sendAllBtn_->setToolTip(
        "Send all recorded channels to the Analyser (and Converter). "
        "Re-sending a channel replaces its previous copy.");
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
    captureRow->addWidget(sendSelectedBtn_);
    captureRow->addWidget(sendAllBtn_);

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
    preview_   = new ui::LivePreviewPlot(recordStore_, this);

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

    connect(sendAllBtn_,      &QPushButton::clicked, this, &RecorderWidget::onSendAll);
    connect(sendSelectedBtn_, &QPushButton::clicked, this, &RecorderWidget::onSendSelected);

    // The preview's sidebar buttons only request — the full recorder layout
    // (connection + channel table + plot) is saved/loaded here.
    connect(preview_, &ui::LivePreviewPlot::saveLayoutRequested,
            this, &RecorderWidget::onSaveLayout);
    connect(preview_, &ui::LivePreviewPlot::loadLayoutRequested,
            this, &RecorderWidget::onLoadLayout);

    // Send is available once there's something captured in the private store.
    connect(&recordStore_, &scope::core::SignalStore::channelAdded,
            this, [this](const QString&){ updateSendButtons(); });
    connect(&recordStore_, &scope::core::SignalStore::channelRemoved,
            this, [this](const QString&){ updateSendButtons(); });
    updateSendButtons();
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

    session_ = std::make_unique<RecordingSession>(recordStore_, *client_, this);
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

void RecorderWidget::updateSendButtons() {
    const bool any = recordStore_.size() > 0;
    sendAllBtn_->setEnabled(any);
    sendSelectedBtn_->setEnabled(any);
}

void RecorderWidget::onSendAll() {
    sendChannels(recordStore_.channelNames());
}

void RecorderWidget::onSendSelected() {
    const auto names = preview_->checkedChannelNames();
    if (names.isEmpty()) {
        QMessageBox::information(this, "Nothing ticked",
            "Tick one or more channels in the live list first, then "
            "“Send checked to Analyser”.");
        return;
    }
    sendChannels(names);
}

void RecorderWidget::sendChannels(const QStringList& names) {
    int n = 0;
    for (const auto& name : names) {
        auto src = recordStore_.get(name);
        if (!src) continue;

        // Re-send replaces the copy a previous Send pushed for this channel.
        if (auto it = appliedMap_.find(name); it != appliedMap_.end()) {
            store_.remove(it.value());
            appliedMap_.erase(it);
        }

        // Frozen snapshot — a later recording into recordStore_ won't mutate
        // what's now in the Analyser. append() copies the bytes immediately.
        const auto view = src->snapshotForRead();
        auto meta = src->meta();
        meta.name = uniqueStoreName(store_, meta.name);
        auto copy = std::make_shared<scope::core::Signal>(meta);
        copy->append(view.timestamps, view.values, view.count);
        store_.add(copy);
        appliedMap_.insert(name, meta.name);
        ++n;
    }
    statusLabel_->setText(n == 0
        ? QString("Nothing to send")
        : QString("Sent %1 channel(s) to the Analyser").arg(n));
}

void RecorderWidget::onSaveLayout() {
    QFileDialog dlg(this, "Save recorder layout");
    dlg.setAcceptMode(QFileDialog::AcceptSave);
    dlg.setNameFilters({"Scope recorder layout (*.scorec)", "All files (*)"});
    dlg.setDefaultSuffix("scorec");
    if (dlg.exec() != QDialog::Accepted) return;
    const auto sel = dlg.selectedFiles();
    if (sel.isEmpty()) return;
    QString path = sel.first();
    if (!path.endsWith(".scorec", Qt::CaseInsensitive)) path += ".scorec";

    QJsonObject conn;
    conn["source"] = sourceCombo_->currentText();
    conn["host"]   = hostEdit_->text();
    conn["netId"]  = netIdEdit_->text();
    conn["port"]   = portSpin_->value();

    QJsonArray chans;
    for (const auto& r : channels_->rows()) {
        QJsonObject jc;
        jc["name"]        = r.name;
        jc["type"]        = r.type;
        jc["mode"]        = r.mode;
        jc["unit"]        = r.unit;
        jc["cycleUs"]     = static_cast<double>(r.cycleUs);
        jc["indexGroup"]  = static_cast<double>(r.indexGroup);
        jc["indexOffset"] = static_cast<double>(r.indexOffset);
        chans.append(jc);
    }

    QJsonObject root;
    root["version"]    = 1;
    root["connection"] = conn;
    root["channels"]   = chans;
    // Reuse the PlotLayout serialiser, embedded as a string — the recorder
    // layout is its own file; it never reads/writes the Analyser's .scoana.
    root["plotLayout"] = preview_->currentPlotLayout().toJsonString();

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::critical(this, "Save failed",
            QString("Couldn't open %1 for writing.").arg(path));
        return;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    f.close();
    statusLabel_->setText(
        QString("Recorder layout saved: %1").arg(QFileInfo(path).fileName()));
}

void RecorderWidget::onLoadLayout() {
    const QString path = QFileDialog::getOpenFileName(
        this, "Load recorder layout", QString(),
        "Scope recorder layout (*.scorec);;All files (*)");
    if (path.isEmpty()) return;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::critical(this, "Load failed",
            QString("Couldn't open %1.").arg(path));
        return;
    }
    QJsonParseError perr;
    const auto doc = QJsonDocument::fromJson(f.readAll(), &perr);
    f.close();

    // Reject anything that isn't a recorder-layout wrapper (e.g. an Analyser
    // .scoana plot layout) — the two formats are deliberately not
    // interchangeable.
    const auto root = doc.object();
    if (perr.error != QJsonParseError::NoError || !doc.isObject()
        || !root.contains("version") || !root.contains("connection")
        || !root.contains("channels")) {
        QMessageBox::critical(this, "Load failed",
            "This isn't a recorder layout. Recorder layouts are .scorec files "
            "(the Analyser uses .scoana — they aren't interchangeable).");
        return;
    }

    const auto conn = root.value("connection").toObject();
    sourceCombo_->setCurrentText(conn.value("source").toString());
    hostEdit_->setText(conn.value("host").toString());
    netIdEdit_->setText(conn.value("netId").toString());
    portSpin_->setValue(conn.value("port").toInt(portSpin_->value()));

    std::vector<ui::ChannelTableWidget::Row> rows;
    for (const auto& v : root.value("channels").toArray()) {
        const auto jc = v.toObject();
        ui::ChannelTableWidget::Row r;
        r.name        = jc.value("name").toString();
        r.type        = jc.value("type").toString();
        r.mode        = jc.value("mode").toString();
        r.unit        = jc.value("unit").toString();
        r.cycleUs     = static_cast<std::uint32_t>(jc.value("cycleUs").toDouble());
        r.indexGroup  = static_cast<std::uint32_t>(jc.value("indexGroup").toDouble());
        r.indexOffset = static_cast<std::uint32_t>(jc.value("indexOffset").toDouble());
        rows.push_back(std::move(r));
    }
    channels_->setRows(rows);

    preview_->applyPlotLayout(scope::plot::PlotLayout::fromJsonString(
        root.value("plotLayout").toString()));

    statusLabel_->setText(
        QString("Recorder layout loaded: %1").arg(QFileInfo(path).fileName()));
}

}  // namespace scope::recorder
