#include "scope/recorder/RecorderWidget.h"

#include "SymbolBrowserWidget.h"
#include "ChannelTableWidget.h"
#include "LivePreviewPlot.h"

#include "scope/ads/MockAdsClient.h"
#include "scope/converter/BusyRunner.h"
#include "scope/converter/HtmlExport.h"
#include "scope/converter/CsvWriter.h"
#include "scope/converter/SaveChartDialog.h"
#include "scope/converter/SignalIO.h"
#include "scope/plot/PlotLayout.h"
#include "scope/style/StyleKit.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSize>
#include <QSpinBox>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>

#include <cstdint>
#include <filesystem>
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
    saveChartBtn_ = new QPushButton(
        scope::style::icon(scope::style::Glyph::Save), "Save chart…", this);
    saveChartBtn_->setEnabled(false);
    saveChartBtn_->setToolTip(
        "Save the recorded channels to HDF5 / MDF4 / CSV / JSON / HTML — the "
        "same dialog the Analyser uses. Recordings stream to a temp file; this "
        "is how you keep one.");
    onlySelectedCheck_ = new QCheckBox("only selected", this);
    onlySelectedCheck_->setEnabled(false);
    onlySelectedCheck_->setToolTip(
        "When ticked, “Send to Analyser” pushes only the channels checked in "
        "the live list; otherwise it sends all recorded channels.");
    sendAllBtn_ = new QPushButton(
        scope::style::icon(scope::style::Glyph::SendToAnalyser, Qt::white), "", this);
    sendAllBtn_->setProperty("accent", true);
    sendAllBtn_->setEnabled(false);
    sendAllBtn_->setIconSize(QSize(20, 20));
    sendAllBtn_->setToolTip(
        "Send recorded channels to the Analyser (and Converter). Re-sending a "
        "channel replaces its previous copy. Tick “only selected” to send just "
        "the checked channels.");
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

    // Save / Load layout live in this top toolbar, mirroring the Analyser's
    // action bar (grouped next to Save chart). They drive the full recorder
    // layout (connection + channel table + preview axes).
    auto* saveLayoutBtn = new QPushButton(
        scope::style::icon(scope::style::Glyph::Save), "Save layout…", this);
    saveLayoutBtn->setToolTip(
        "Save the recorder layout (connection, channel table, and preview "
        "axes) to a .scorec file.");
    auto* loadLayoutBtn = new QPushButton(
        scope::style::icon(scope::style::Glyph::FolderOpen), "Load layout…", this);
    loadLayoutBtn->setToolTip("Restore a recorder layout from a .scorec file.");
    connect(saveLayoutBtn, &QPushButton::clicked, this, &RecorderWidget::onSaveLayout);
    connect(loadLayoutBtn, &QPushButton::clicked, this, &RecorderWidget::onLoadLayout);

    // Undo / redo over the recorder document (connection + channel table +
    // preview axes). Icon-only to keep the toolbar compact.
    undoBtn_ = new QPushButton(scope::style::icon(scope::style::Glyph::Undo), "", this);
    undoBtn_->setToolTip("Undo the last change to the connection, channel table, "
                         "or preview (Ctrl+Z).");
    undoBtn_->setShortcut(QKeySequence::Undo);
    undoBtn_->setEnabled(false);
    redoBtn_ = new QPushButton(scope::style::icon(scope::style::Glyph::Redo), "", this);
    redoBtn_->setToolTip("Redo the change undone last (Ctrl+Y / Ctrl+Shift+Z).");
    redoBtn_->setShortcut(QKeySequence::Redo);
    redoBtn_->setEnabled(false);

    auto* captureRow = new QHBoxLayout();
    captureRow->setSpacing(6);
    captureRow->addWidget(startBtn_);
    captureRow->addWidget(stopBtn_);
    captureRow->addSpacing(8);
    captureRow->addWidget(saveChartBtn_);
    captureRow->addSpacing(12);
    captureRow->addWidget(undoBtn_);
    captureRow->addWidget(redoBtn_);
    captureRow->addSpacing(12);
    captureRow->addWidget(saveLayoutBtn);
    captureRow->addWidget(loadLayoutBtn);
    captureRow->addSpacing(10);
    captureRow->addWidget(statusLabel_);
    captureRow->addStretch();
    captureRow->addWidget(onlySelectedCheck_);
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
    connect(symbols_, &ui::SymbolBrowserWidget::addByNameRequested,
            this, &RecorderWidget::onAddSymbolByName);

    // One Send button; the "only selected" checkbox decides all vs checked.
    connect(sendAllBtn_, &QPushButton::clicked, this, [this]{
        if (onlySelectedCheck_->isChecked()) onSendSelected();
        else                                 onSendAll();
    });
    connect(saveChartBtn_, &QPushButton::clicked, this, &RecorderWidget::onSaveChart);

    // Send / Save chart are available once there's something captured.
    connect(&recordStore_, &scope::core::SignalStore::channelAdded,
            this, [this](const QString&){ updateActionButtons(); });
    connect(&recordStore_, &scope::core::SignalStore::channelRemoved,
            this, [this](const QString&){ updateActionButtons(); });
    // appliedMap_ remembers what each recorded channel became in the shared
    // store, so "send to Analyser" replaces rather than duplicates. A rename
    // there has to move with it or the next send adds a second copy.
    // Note: store_, not recordStore_ — appliedMap_'s VALUES are names in the
    // shared store, which is where the Analyser's rename happens.
    connect(&store_, &scope::core::SignalStore::channelRenamed, this,
            [this](const QString& from, const QString& to) {
        for (auto it = appliedMap_.begin(); it != appliedMap_.end(); ++it)
            if (it.value() == from) it.value() = to;
    });
    updateActionButtons();

    // ---- Undo / redo wiring ------------------------------------------
    commitTimer_ = new QTimer(this);
    commitTimer_->setSingleShot(true);
    commitTimer_->setInterval(0);
    undo_ = std::make_unique<scope::core::UndoStack<QJsonObject>>(
        [this]{ return captureConfig(); },
        [this](const QJsonObject& s){ applyConfig(s); },
        [this]{ refreshUndoButtons(); });
    connect(commitTimer_, &QTimer::timeout, this, [this]{ undo_->commit(); });
    connect(undoBtn_, &QPushButton::clicked, this, [this]{ undo_->undo(); });
    connect(redoBtn_, &QPushButton::clicked, this, [this]{ undo_->redo(); });

    // Commit points: a channel-table edit, or a settled connection-field edit.
    // (Restores set these programmatically; scheduleCommit() no-ops while the
    // undo stack is applying, and the line edits only fire on user edits.)
    connect(channels_, &ui::ChannelTableWidget::changed, this,
            [this]{ scheduleCommit(); });
    connect(hostEdit_,  &QLineEdit::editingFinished, this, [this]{ scheduleCommit(); });
    connect(netIdEdit_, &QLineEdit::editingFinished, this, [this]{ scheduleCommit(); });
    connect(portSpin_,  &QSpinBox::editingFinished,  this, [this]{ scheduleCommit(); });
    connect(sourceCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int){ scheduleCommit(); });

    // Seed the baseline now that every widget exists.
    undo_->reset();
    refreshUndoButtons();
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

    // Selecting a structure or array takes everything inside it, which can be
    // hundreds of channels from one click (an AXIS_REF is ~390). Confirm first
    // — the user may well have wanted only a few of them.
    const QStringList groups = symbols_->selectedGroupNames();
    if (!groups.isEmpty() && selected.size() > 1) {
        const QString what = groups.size() == 1
            ? QString("“%1”").arg(groups.first())
            : QString("%1 selected groups").arg(groups.size());
        const auto answer = QMessageBox::question(this, "Add all of these?",
            QString("%1 contains %2 recordable channels.\n\nAdd all of them?\n\n"
                    "To take only some, expand it in the list and select the "
                    "ones you want.")
                .arg(what).arg(selected.size()),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Yes) return;
    }

    QStringList rejected;
    for (const auto& sym : selected) {
        // A struct, function block, array or STRING has no single numeric
        // value. Recording one used to sample its first 8 bytes as a double —
        // a plausible-looking number that is not the data. Refuse instead.
        if (sym.unsupported) {
            rejected << QString("%1  (%2)").arg(sym.name, sym.typeName);
            continue;
        }
        const auto cycleUs = client_->taskCycleForSymbol(sym);
        channels_->addChannelFromSymbol(sym, cycleUs);
    }
    if (!rejected.isEmpty()) {
        QMessageBox::information(this, "Can't record these directly",
            QString("These symbols have no single numeric value, so they were "
                    "not added:\n\n%1\n\nRecord a structure's individual "
                    "members or an array's individual elements instead — those "
                    "appear as their own symbols once the PLC publishes them.")
                .arg(rejected.join("\n")));
    }
}

void RecorderWidget::onAddSymbolByName(const QString& name) {
    if (!client_ || !client_->isConnected()) {
        QMessageBox::information(this, "Not connected",
            "Connect to a source before looking a symbol up.");
        return;
    }
    // Ask the PLC directly. This is the only route to a structure member: the
    // symbol upload has one entry per declared variable, so members are in no
    // list we could search.
    QString err;
    const auto sym = client_->resolveSymbol(name, &err);
    if (!sym) {
        QMessageBox::information(this, "Symbol not added",
            err.isEmpty() ? QString("Couldn't resolve '%1'.").arg(name) : err);
        return;
    }
    const auto cycleUs = client_->taskCycleForSymbol(*sym);
    channels_->addChannelFromSymbol(*sym, cycleUs);
}

void RecorderWidget::onStartClicked() {
    if (!client_ || !client_->isConnected()) {
        QMessageBox::information(this, "Not connected", "Connect to a source first.");
        return;
    }
    // Record straight into a temp .h5 (overwritten each Record) — no save
    // prompt. The user persists a chosen file/format later via "Save chart…".
    const auto tempFile = std::filesystem::temp_directory_path()
                        / "ScopeAnalyser_recording.h5";

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
    // Channels now exist in recordStore_ → their preview graphs were created.
    // Put each on the axis a loaded layout asked for (the layout was applied
    // before any channel existed, so the assignment was deferred).
    preview_->reapplyChannelAssignments();

    if (!session_->start(tempFile, &err)) {
        QMessageBox::critical(this, "Failed to start", err);
        session_.reset();
        return;
    }
    preview_->setRecording(true);
    startBtn_->setEnabled(false);
    stopBtn_->setEnabled(true);
    scope::style::setPill(connPill_, scope::style::PillTone::Rec, "● Recording");
}

void RecorderWidget::onStopClicked() {
    if (!session_) return;
    session_->stop();
    session_.reset();
    preview_->setRecording(false);
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

void RecorderWidget::updateActionButtons() {
    const bool any = recordStore_.size() > 0;
    sendAllBtn_->setEnabled(any);
    onlySelectedCheck_->setEnabled(any);
    saveChartBtn_->setEnabled(any);
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

void RecorderWidget::onSaveChart() {
    if (recordStore_.size() == 0) {
        QMessageBox::information(this, "Nothing to save", "Record something first.");
        return;
    }
    converter::ui::SaveChartDialog dlg(0.0, 0.0, this);
    // The recorder is time-domain only with no math channels: drop the
    // "Channels to include" group and show only the Layout metadata choice.
    dlg.setChannelFiltersVisible(false);
    dlg.setMetadataLayoutOnly(true);
    dlg.setChannels(recordStore_);
    dlg.setRepeatedTimestamps(converter::scanRepeatedTimestamps(recordStore_));
    if (dlg.exec() != QDialog::Accepted) return;

    const auto fmtChoice = dlg.format();
    const converter::FileFormat fmt =
          (fmtChoice == converter::ui::SaveChartDialog::Format::Csv)  ? converter::FileFormat::Csv
        : (fmtChoice == converter::ui::SaveChartDialog::Format::Mdf4) ? converter::FileFormat::Mdf4
        : (fmtChoice == converter::ui::SaveChartDialog::Format::Html) ? converter::FileFormat::Html
        : (fmtChoice == converter::ui::SaveChartDialog::Format::Json) ? converter::FileFormat::Json
                                                                      : converter::FileFormat::Hdf5;

    QFileDialog fileDlg(this,
        QString("Save chart (.%1)").arg(converter::defaultSuffix(fmt)));
    fileDlg.setAcceptMode(QFileDialog::AcceptSave);
    fileDlg.setNameFilters(converter::nameFilters(fmt));
    fileDlg.setDefaultSuffix(converter::defaultSuffix(fmt));
    if (fileDlg.exec() != QDialog::Accepted) return;
    const auto sel = fileDlg.selectedFiles();
    if (sel.isEmpty()) return;

    converter::ChartSaveFilters filters;
    filters.includeTime              = dlg.includeTimeDomain();
    filters.includeFrequency         = dlg.includeFrequencyDomain();
    filters.includeDerived           = dlg.includeDerivedChannels();
    filters.selectedChannels         = dlg.selectedChannels();
    filters.splitDomainsIntoTwoFiles = dlg.splitDomainsIntoTwoFiles();
    filters.useCustomRange           = dlg.useCustomRange();
    filters.fromSec                  = dlg.fromSec();
    filters.toSec                    = dlg.toSec();

    converter::SaveOptions opts;
    opts.csv = dlg.csvOptions();
    opts.htmlViewOnly = dlg.htmlViewOnly();
    // Embed only the Analyser-relevant plot layout (axes + channel→axis) so a
    // re-opened file restores the view. No recorder-specific data is written.
    if (dlg.includeLayout()) {
        opts.layoutJson = preview_->currentPlotLayout().toJsonString();
    }
    // Storable HTML mirrors the preview (axes / colours / assignments).
    if (fmt == converter::FileFormat::Html) {
        opts.htmlView = preview_->htmlExportView();
    }

    struct SaveOutcome {
        converter::ChartSaveResult result{converter::ChartSaveResult::Failed};
        QStringList messages;
        QString error;
    };
    const QString target = sel.first();
    const auto outcome = converter::ui::runWithBusyDialog(this, "Saving chart…",
        [&]() -> SaveOutcome {
            SaveOutcome o;
            QStringList msgs;
            QString err;
            o.result   = converter::saveChartFromStore(
                target, recordStore_, fmt, filters, opts, &msgs, &err);
            o.messages = std::move(msgs);
            o.error    = std::move(err);
            return o;
        });
    switch (outcome.result) {
        case converter::ChartSaveResult::NothingMatchedFilters:
            QMessageBox::warning(this, "Nothing to save",
                "No channels matched the current filters / time range.");
            return;
        case converter::ChartSaveResult::Failed:
            QMessageBox::critical(this, "Save failed", outcome.error);
            return;
        case converter::ChartSaveResult::Ok:
            statusLabel_->setText("Saved: " + outcome.messages.join("; "));
            QMessageBox::information(this, "Saved", outcome.messages.join("\n"));
            return;
    }
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

    const QJsonObject root = captureConfig();

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

    applyConfig(root);
    // Loading a layout is one undo step.
    scheduleCommit();

    statusLabel_->setText(
        QString("Recorder layout loaded: %1").arg(QFileInfo(path).fileName()));
}

QJsonObject RecorderWidget::captureConfig() {
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
    return root;
}

void RecorderWidget::applyConfig(const QJsonObject& root) {
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
}

void RecorderWidget::scheduleCommit() {
    if (!undo_ || undo_->applying()) return;
    commitTimer_->start();
}

void RecorderWidget::refreshUndoButtons() {
    if (!undo_) return;
    undoBtn_->setEnabled(undo_->canUndo());
    redoBtn_->setEnabled(undo_->canRedo());
}

}  // namespace scope::recorder
