#include "scope/converter/ConverterWidget.h"
#include "scope/converter/BusyRunner.h"
#include "scope/converter/ConverterProfile.h"
#include "scope/converter/CsvSource.h"
#include "scope/converter/CsvWriter.h"
#include "scope/converter/SaveChartDialog.h"
#include "scope/converter/SignalIO.h"

#include "MappingPanel.h"

#include "scope/style/StyleKit.h"

#include <nlohmann/json.hpp>

#include <QAbstractItemModel>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableView>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

namespace scope::converter {

namespace {

enum class FileType { Csv, H5 };

struct OpenedFile {
    FileType type{FileType::Csv};
    QString path;
    QString displayName;

    // CSV-specific
    std::unique_ptr<CsvSource> csv;
    ConverterProfile profile;
    std::unique_ptr<QAbstractItemModel> previewModel;

    // H5-specific. Signals are loaded once on Open and kept in memory; the
    // user picks which subset to push to the store via the channel-selector
    // panel. selectedChannels / relativeChannels / offsetSec mirror the per-
    // channel state shown in the selector.
    std::vector<std::shared_ptr<core::Signal>> h5Signals;
    QStringList h5SelectedChannels;
    QStringList h5RelativeChannels;
    QHash<QString, double> h5OffsetSec;

    // Signals (by store-resolved name) that have been pushed to the store
    // from this file. Used by Remove to clean up the store and by workspace
    // save to record what's in flight.
    QStringList importedNames;
};

// HDF5 channel-selector widget. Plain QWidget (no Q_OBJECT) — the Apply
// button calls a callback registered by the parent. The mapping panel uses
// signals because it's reused across forms; this panel only ever has one
// consumer.
struct H5SelectorPanel : public QWidget {
    static constexpr int kColVis      = 0;
    static constexpr int kColName     = 1;
    static constexpr int kColUnit     = 2;
    static constexpr int kColSamples  = 3;
    static constexpr int kColDuration = 4;
    static constexpr int kColRelative = 5;
    static constexpr int kColOffset   = 6;
    static constexpr int kColCount    = 7;

    QTableWidget* table{nullptr};
    QCheckBox*    bulkRelative{nullptr};
    QDoubleSpinBox* bulkOffset{nullptr};
    std::function<void()> onApply;

    explicit H5SelectorPanel(QWidget* parent = nullptr) : QWidget(parent) {
        table = new QTableWidget(0, kColCount, this);
        table->setHorizontalHeaderLabels(
            {"", "Channel", "Unit", "Samples", "Duration", "Relative", "Offset [s]"});
        table->horizontalHeader()->setSectionResizeMode(kColName, QHeaderView::Stretch);
        table->verticalHeader()->setVisible(false);
        table->setSelectionMode(QAbstractItemView::NoSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setAlternatingRowColors(true);

        auto* allBtn   = new QPushButton("Select all", this);
        auto* noneBtn  = new QPushButton("Select none", this);
        auto* applyBtn = new QPushButton("Apply (import selected)", this);
        applyBtn->setProperty("accent", true);

        connect(allBtn,  &QPushButton::clicked, this, [this]{ setAllChecked(true); });
        connect(noneBtn, &QPushButton::clicked, this, [this]{ setAllChecked(false); });
        connect(applyBtn,&QPushButton::clicked, this, [this]{
            if (onApply) onApply();
        });

        // Bulk-set row: set Relative + Offset on every channel at once.
        bulkRelative = new QCheckBox("Relative", this);
        bulkOffset   = new QDoubleSpinBox(this);
        bulkOffset->setRange(-1e9, 1e9);
        bulkOffset->setDecimals(6);
        bulkOffset->setSuffix(" s");
        bulkOffset->setValue(0.0);
        auto* bulkBtn = new QPushButton("Apply to all", this);
        bulkBtn->setToolTip(
            "Bulk-set the Relative + Offset on every channel in the table.\n"
            "Per-channel values can still be edited individually afterwards.");
        connect(bulkBtn, &QPushButton::clicked, this, [this]{
            const bool rel = bulkRelative->isChecked();
            const double off = bulkOffset->value();
            for (int r = 0; r < table->rowCount(); ++r) {
                if (auto* cb = qobject_cast<QCheckBox*>(table->cellWidget(r, kColRelative)))
                    cb->setChecked(rel);
                if (auto* sb = qobject_cast<QDoubleSpinBox*>(table->cellWidget(r, kColOffset)))
                    sb->setValue(off);
            }
        });

        auto* btnRow = new QHBoxLayout();
        btnRow->addWidget(allBtn);
        btnRow->addWidget(noneBtn);
        btnRow->addStretch();

        auto* bulkRow = new QHBoxLayout();
        bulkRow->addWidget(new QLabel("All channels:", this));
        bulkRow->addWidget(bulkRelative);
        bulkRow->addWidget(new QLabel("Offset:", this));
        bulkRow->addWidget(bulkOffset);
        bulkRow->addWidget(bulkBtn);
        bulkRow->addStretch();

        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(4, 6, 8, 8);
        root->setSpacing(6);
        root->addWidget(scope::style::sectionLabel("HDF5 channels — tick to import", this));
        root->addWidget(table, /*stretch=*/1);
        root->addLayout(btnRow);
        root->addLayout(bulkRow);
        root->addWidget(applyBtn);
    }

    void setSignals(const std::vector<std::shared_ptr<core::Signal>>& sigs,
                    const QStringList& preChecked,
                    const QStringList& preRelative,
                    const QHash<QString, double>& preOffsets) {
        // Batch the rebuild — Qt would otherwise repaint after every
        // insertRow / setCellWidget. setUpdatesEnabled(false) defers
        // paints until we re-enable updates at the end.
        table->setUpdatesEnabled(false);
        table->setRowCount(0);
        // Pre-size the table so insertRow doesn't keep growing the
        // internal vector.
        int nValid = 0;
        for (const auto& s : sigs) if (s) ++nValid;
        table->setRowCount(nValid);
        int r = 0;
        for (const auto& s : sigs) {
            if (!s) continue;
            const auto meta = s->meta();
            const auto view = s->snapshotForRead();
            auto* cb = new QCheckBox();
            cb->setChecked(preChecked.isEmpty() || preChecked.contains(meta.name));
            table->setCellWidget(r, kColVis, cb);
            table->setItem(r, kColName, new QTableWidgetItem(meta.name));
            table->setItem(r, kColUnit, new QTableWidgetItem(meta.unit));
            table->setItem(r, kColSamples, new QTableWidgetItem(QString::number(view.count)));
            QString dur = "—";
            if (view.count >= 2) {
                const double secs =
                    (view.timestamps[view.count - 1] - view.timestamps[0]) / 1e9;
                dur = QString("%1 s").arg(secs, 0, 'g', 4);
            }
            table->setItem(r, kColDuration, new QTableWidgetItem(dur));

            auto* relCb = new QCheckBox();
            relCb->setChecked(preRelative.contains(meta.name));
            table->setCellWidget(r, kColRelative, relCb);

            auto* offSb = new QDoubleSpinBox();
            offSb->setRange(-1e9, 1e9);
            offSb->setDecimals(6);
            offSb->setValue(preOffsets.value(meta.name, 0.0));
            table->setCellWidget(r, kColOffset, offSb);
            ++r;
        }
        table->setUpdatesEnabled(true);
    }

    QStringList checkedNames() const {
        QStringList out;
        for (int r = 0; r < table->rowCount(); ++r) {
            if (auto* cb = qobject_cast<QCheckBox*>(table->cellWidget(r, kColVis))) {
                if (cb->isChecked()) {
                    if (auto* item = table->item(r, kColName)) out << item->text();
                }
            }
        }
        return out;
    }

    QStringList relativeNames() const {
        QStringList out;
        for (int r = 0; r < table->rowCount(); ++r) {
            if (auto* cb = qobject_cast<QCheckBox*>(table->cellWidget(r, kColRelative))) {
                if (cb->isChecked()) {
                    if (auto* item = table->item(r, kColName)) out << item->text();
                }
            }
        }
        return out;
    }

    QHash<QString, double> offsetMap() const {
        QHash<QString, double> out;
        for (int r = 0; r < table->rowCount(); ++r) {
            const auto* item = table->item(r, kColName);
            if (!item) continue;
            if (auto* sb = qobject_cast<QDoubleSpinBox*>(table->cellWidget(r, kColOffset))) {
                const double v = sb->value();
                if (v != 0.0) out.insert(item->text(), v);
            }
        }
        return out;
    }

private:
    void setAllChecked(bool on) {
        for (int r = 0; r < table->rowCount(); ++r) {
            if (auto* cb = qobject_cast<QCheckBox*>(table->cellWidget(r, kColVis)))
                cb->setChecked(on);
        }
    }
};

}  // namespace

struct ConverterWidget::Impl {
    // UI
    QListWidget*        fileList{nullptr};
    QPushButton*        removeFileBtn{nullptr};
    QTableView*         preview{nullptr};
    ui::MappingPanel*   mapping{nullptr};
    H5SelectorPanel*    h5Selector{nullptr};
    QStackedWidget*     rightStack{nullptr};
    QLabel*             statusLabel{nullptr};

    // Per-file state
    std::vector<std::unique_ptr<OpenedFile>> files;
    int activeIndex{-1};

    // Block save-active-state-into-file from firing during setProfile()
    // inside a programmatic switch. Also gates the parseOptionsChanged
    // reparse handler so a programmatic load doesn't re-read the CSV
    // from disk 3-4 times in a row.
    bool suppressSave{false};

    // Identity of the OpenedFile whose H5 selector contents are
    // currently in the panel. When switching back to the same file
    // (e.g. A→B→A) we can skip the whole table rebuild.
    const OpenedFile* lastH5Source{nullptr};
};

// Forward decls of helper methods on ConverterWidget
ConverterWidget::~ConverterWidget() = default;

// ----------------------------- Helpers --------------------------------

namespace {

// Scan a freshly-imported set of channels (by name) for residual
// duplicate timestamps. Returns one human-readable warning line per
// channel whose duplicate ratio exceeds 10 %. Empty list means
// nothing of concern.
QStringList scanDuplicateTimestamps(const core::SignalStore& store,
                                    const QStringList& names) {
    QStringList warnings;
    for (const auto& name : names) {
        auto sig = store.get(name);
        if (!sig) continue;
        auto view = sig->snapshotForRead();
        if (view.count < 2) continue;
        std::size_t dups = 0;
        for (std::size_t i = 1; i < view.count; ++i)
            if (view.timestamps[i] == view.timestamps[i - 1]) ++dups;
        if (dups * 10 > view.count) {  // > 10 %
            warnings << QString("• %1: %2 of %3 samples share a timestamp")
                            .arg(name).arg(dups).arg(view.count);
        }
    }
    return warnings;
}

// Same idea for staircase / quantised data: count consecutive samples
// that share the same Y value despite having different timestamps.
// Threshold: 30 % (this pattern is rarer noise and more often a real
// data shape, so don't fire on trace amounts).
QStringList scanValuePlateaus(const core::SignalStore& store,
                              const QStringList& names) {
    QStringList warnings;
    for (const auto& name : names) {
        auto sig = store.get(name);
        if (!sig) continue;
        auto view = sig->snapshotForRead();
        if (view.count < 2) continue;
        auto vs = sig->readAsDouble();
        std::size_t same = 0;
        for (std::size_t i = 1; i < view.count && i < vs.size(); ++i)
            if (vs[i] == vs[i - 1]) ++same;
        if (same * 10 > view.count * 3) {  // > 30 %
            warnings << QString("• %1: %2 of %3 samples repeat the "
                                "previous value")
                            .arg(name).arg(same).arg(view.count);
        }
    }
    return warnings;
}

QString uniqueStoreName(const core::SignalStore& store, const QString& base) {
    if (!store.contains(base)) return base;
    for (int i = 2; i < 10000; ++i) {
        QString c = QString("%1 (%2)").arg(base).arg(i);
        if (!store.contains(c)) return c;
    }
    return base + " (?)";
}

}  // namespace

ConverterWidget::ConverterWidget(scope::core::SignalStore& store, QWidget* parent)
    : QWidget(parent), store_(store), impl_(std::make_unique<Impl>()) {

    // ---- Top action bar ----------------------------------------------
    auto* toolbar = new QWidget(this);
    scope::style::applyToolbarStrip(toolbar);
    auto* topBar = new QHBoxLayout(toolbar);
    topBar->setContentsMargins(8, 4, 8, 4);
    topBar->setSpacing(4);
    auto barBtn = [&](scope::style::Glyph glyph, const QString& text,
                      const QString& tip) {
        auto* b = new QToolButton(toolbar);
        b->setIcon(scope::style::icon(glyph));
        b->setText(text);
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        if (!tip.isEmpty()) b->setToolTip(tip);
        topBar->addWidget(b);
        return b;
    };
    auto* openChartBtn = barBtn(scope::style::Glyph::FolderOpen, "Open chart…",
        "Open .h5 / .mf4 / .csv / .txt. Recordings (.h5 / .mf4) go "
        "straight to the signal store; CSV files land in the mapping "
        "panel so you can review the column assignments before Apply. "
        "If the CSV carries a scope metadata header the mapping panel "
        "is pre-filled.");
    topBar->addSpacing(12);
    auto* saveWsBtn = barBtn(scope::style::Glyph::Save, "Save workspace…",
        "Save the list of opened files with their mapping profiles and "
        "channel selections to a .scaws file.");
    auto* loadWsBtn = barBtn(scope::style::Glyph::FolderOpen, "Load workspace…",
        "Reopen a saved set of files with their mapping profiles and "
        "channel selections.");
    topBar->addSpacing(12);
    auto* saveChartBtn = barBtn(scope::style::Glyph::Save, "Save chart…",
        "Unified save dialog: pick HDF5 / MDF4 / CSV, per-domain filters, "
        "time range, split-files toggle, CSV options.");
    topBar->addStretch();
    impl_->statusLabel = new QLabel("No file open", this);
    impl_->statusLabel->setProperty("scopeRole", "dim");
    topBar->addWidget(impl_->statusLabel);
    topBar->addSpacing(10);
    auto* applyAllBtn = new QPushButton("Apply all (import signals)", toolbar);
    applyAllBtn->setProperty("accent", true);
    applyAllBtn->setToolTip(
        "Import every opened file into the signal store using its current "
        "mapping / channel selection.");
    topBar->addWidget(applyAllBtn);

    // ---- Left: file list + remove button -----------------------------
    impl_->fileList = new QListWidget(this);
    impl_->fileList->setMinimumWidth(180);
    impl_->fileList->setMaximumWidth(280);
    scope::style::installEmptyHint(impl_->fileList,
        "No files open.\n\nUse “Open chart…” to load a recording or a "
        "CSV for mapping.");
    impl_->removeFileBtn = new QPushButton(
        scope::style::icon(scope::style::Glyph::Minus), "Remove file", this);
    impl_->removeFileBtn->setToolTip(
        "Close this file and remove the signals it imported from the store.");
    impl_->removeFileBtn->setEnabled(false);

    auto* leftPanel = new QWidget(this);
    auto* leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(8, 6, 4, 8);
    leftLayout->setSpacing(6);
    leftLayout->addWidget(scope::style::sectionLabel("Opened files", this));
    leftLayout->addWidget(impl_->fileList, /*stretch=*/1);
    leftLayout->addWidget(impl_->removeFileBtn);

    // ---- Middle: preview --------------------------------------------
    impl_->preview = new QTableView(this);
    impl_->preview->setEditTriggers(QAbstractItemView::NoEditTriggers);
    impl_->preview->setSelectionMode(QAbstractItemView::NoSelection);
    impl_->preview->horizontalHeader()->setDefaultSectionSize(100);

    auto* previewPanel = new QWidget(this);
    auto* previewLayout = new QVBoxLayout(previewPanel);
    previewLayout->setContentsMargins(4, 6, 4, 8);
    previewLayout->setSpacing(6);
    previewLayout->addWidget(scope::style::sectionLabel("File preview", this));
    previewLayout->addWidget(impl_->preview, /*stretch=*/1);

    // ---- Right: mapping panel (CSV) or H5 channel selector ----------
    impl_->mapping    = new ui::MappingPanel(this);
    impl_->h5Selector = new H5SelectorPanel(this);
    impl_->rightStack = new QStackedWidget(this);
    impl_->rightStack->addWidget(impl_->mapping);    // index 0 = CSV
    impl_->rightStack->addWidget(impl_->h5Selector); // index 1 = H5

    auto* split = new QSplitter(Qt::Horizontal, this);
    split->addWidget(leftPanel);
    split->addWidget(previewPanel);
    split->addWidget(impl_->rightStack);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 2);
    split->setStretchFactor(2, 1);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(toolbar);
    root->addWidget(split, /*stretch=*/1);

    // ---- File-switching: save current → load clicked ----------------
    auto saveActiveState = [this]{
        if (impl_->suppressSave) return;
        if (impl_->activeIndex < 0
            || impl_->activeIndex >= (int)impl_->files.size()) return;
        auto& f = *impl_->files[impl_->activeIndex];
        if (f.type == FileType::Csv) {
            f.profile = impl_->mapping->buildProfile("csv");
        } else {
            f.h5SelectedChannels = impl_->h5Selector->checkedNames();
            f.h5RelativeChannels = impl_->h5Selector->relativeNames();
            f.h5OffsetSec        = impl_->h5Selector->offsetMap();
        }
    };

    auto loadActiveState = [this]{
        if (impl_->activeIndex < 0
            || impl_->activeIndex >= (int)impl_->files.size()) {
            impl_->preview->setModel(nullptr);
            impl_->rightStack->setCurrentIndex(0);
            impl_->statusLabel->setText("No file open");
            impl_->removeFileBtn->setEnabled(false);
            return;
        }
        auto& f = *impl_->files[impl_->activeIndex];
        impl_->suppressSave = true;
        if (f.type == FileType::Csv) {
            impl_->mapping->setProfile(f.profile);
            impl_->preview->setModel(f.previewModel.get());
            impl_->rightStack->setCurrentIndex(0);
            impl_->statusLabel->setText(
                QString("%1: %2 rows × %3 cols")
                    .arg(f.displayName)
                    .arg(f.csv ? f.csv->rowCount()    : 0)
                    .arg(f.csv ? f.csv->columnCount() : 0));
        } else {
            // Skip the table rebuild when switching back to the same
            // H5 file (A→B→A) — its rows are already in the panel.
            // The widget contents reflect what we left, so checkboxes
            // and offsets are right. setRowCount(0) + insertRow * N +
            // checkboxes + spinboxes can be 1000+ widget operations
            // for big recordings, so this is the main win on toggle.
            if (impl_->lastH5Source != &f) {
                impl_->h5Selector->setSignals(f.h5Signals,
                                              f.h5SelectedChannels,
                                              f.h5RelativeChannels,
                                              f.h5OffsetSec);
                impl_->lastH5Source = &f;
            }
            impl_->preview->setModel(nullptr);
            impl_->rightStack->setCurrentIndex(1);
            impl_->statusLabel->setText(
                QString("%1: %2 channel(s)")
                    .arg(f.displayName).arg(f.h5Signals.size()));
        }
        impl_->suppressSave = false;
        impl_->removeFileBtn->setEnabled(true);
    };

    auto switchTo = [this, saveActiveState, loadActiveState](int idx) {
        if (idx == impl_->activeIndex) return;
        saveActiveState();
        impl_->activeIndex = idx;
        loadActiveState();
        if (idx >= 0) {
            QSignalBlocker b(impl_->fileList);
            impl_->fileList->setCurrentRow(idx);
        }
    };

    connect(impl_->fileList, &QListWidget::currentRowChanged,
            this, [switchTo](int row){ switchTo(row); });

    auto addFileToList = [this](const OpenedFile& f) {
        const QString tag = (f.type == FileType::Csv) ? "[csv]" : "[h5]";
        impl_->fileList->addItem(QString("%1 %2").arg(tag, f.displayName));
    };

    // ---- Open CSV ----------------------------------------------------
    // ---- Open recording (.h5 / .mf4): loads all signals into memory,
    //      doesn't push to store until the user picks + Applies. ----
    auto openRecording = [this, saveActiveState, loadActiveState, addFileToList]
        (const QString& path) {
        // Read off the GUI thread behind a busy dialog (shared with the
        // Analyser) so a large .h5 / .mf4 doesn't look frozen.
        auto r = converter::ui::runWithBusyDialog(this, "Opening file…",
            [path] {
                return converter::loadFile(
                    std::filesystem::path(path.toStdString()));
            });
        if (!r.ok && r.channels.empty()) {
            QMessageBox::critical(this, "Open failed",
                r.error.isEmpty() ? QString("Unknown error") : r.error);
            return;
        }
        if (r.channels.empty()) {
            QMessageBox::warning(this, "Empty file",
                r.error.isEmpty() ? QString("No channels in this file.") : r.error);
            return;
        }

        auto f = std::make_unique<OpenedFile>();
        f->type = FileType::H5;
        f->path = path;
        f->displayName = QFileInfo(path).fileName();
        f->h5Signals = std::move(r.channels);
        for (const auto& s : f->h5Signals) {
            if (s) f->h5SelectedChannels << s->meta().name;
        }

        saveActiveState();
        impl_->files.push_back(std::move(f));
        addFileToList(*impl_->files.back());
        impl_->activeIndex = (int)impl_->files.size() - 1;
        {
            QSignalBlocker b(impl_->fileList);
            impl_->fileList->setCurrentRow(impl_->activeIndex);
        }
        loadActiveState();
    };

    // ---- Open CSV: lands in the mapping panel. If the file carries a
    //      "# scope-csv:" header the panel is pre-filled. ----
    auto openCsvForMapping = [this, saveActiveState, loadActiveState, addFileToList]
        (const QString& path) {
        auto f = std::make_unique<OpenedFile>();
        f->type = FileType::Csv;
        f->path = path;
        f->displayName = QFileInfo(path).fileName();
        // Pre-fill from scope metadata if present; otherwise the empty
        // default (user maps manually).
        ConverterProfile metaProfile;
        const bool hadMeta = converter::profileFromScopeMetadata(
            std::filesystem::path(path.toStdString()), &metaProfile);
        if (hadMeta) {
            f->profile = std::move(metaProfile);
        } else {
            f->profile = ConverterProfile{};
            f->profile.sourceType = "csv";
            f->profile.columnDelimiter = ",";
            f->profile.rowDelimiter    = "\n";
            f->profile.headerRow = 1;
            f->profile.decimalSeparator = ".";
        }
        try {
            f->csv = std::make_unique<CsvSource>(
                std::filesystem::path(path.toStdString()),
                f->profile.columnDelimiter,
                f->profile.rowDelimiter);
            f->previewModel = f->csv->previewModel("file");
        } catch (const std::exception& e) {
            QMessageBox::critical(this, "Open failed",
                QString("Couldn't open %1:\n%2").arg(path, e.what()));
            return;
        }

        saveActiveState();
        impl_->files.push_back(std::move(f));
        addFileToList(*impl_->files.back());
        impl_->activeIndex = (int)impl_->files.size() - 1;
        {
            QSignalBlocker b(impl_->fileList);
            impl_->fileList->setCurrentRow(impl_->activeIndex);
        }
        loadActiveState();
    };

    // ---- One Open chart button — extension dispatch. ----
    connect(openChartBtn, &QPushButton::clicked, this,
            [this, openRecording, openCsvForMapping]{
        const QString path = QFileDialog::getOpenFileName(
            this, "Open chart", QString(),
            "Scope files (*.h5 *.mf4 *.csv *.tsv *.txt);;"
            "Recordings (*.h5 *.mf4);;"
            "HDF5 (*.h5);;MDF4 (*.mf4);;"
            "CSV / text (*.csv *.tsv *.txt);;"
            "All files (*)");
        if (path.isEmpty()) return;
        const auto fmt = converter::detectFormatFromExtension(
            std::filesystem::path(path.toStdString()));
        if (fmt == converter::FileFormat::Hdf5
            || fmt == converter::FileFormat::Mdf4) {
            openRecording(path);
        } else {
            // .csv / .tsv / .txt / unknown → mapping panel.
            openCsvForMapping(path);
        }
    });

    // ---- Apply for the active file ----------------------------------
    // Lower-level: do the import using an explicit profile / selection,
    // without touching the active panel widgets. Used both by the per-file
    // Apply (which reads the visible panel state first) and by "Apply all"
    // (which reads each file's saved state directly).
    auto applyCsvWith = [this](OpenedFile& f,
                               const ConverterProfile& profile,
                               QString* errOut) -> int {
        if (!f.csv) {
            if (errOut) *errOut = "File isn't loaded.";
            return 0;
        }
        QString err;
        QStringList warnings;
        auto sigs = f.csv->apply(profile, &err, &warnings);
        if (!warnings.isEmpty()) {
            QMessageBox::warning(this, "Unit fallback",
                "The parser fell back to a default for the following "
                "X-axis unit(s):\n\n• " + warnings.join("\n• "));
        }
        if (sigs.empty()) {
            if (errOut) *errOut = err.isEmpty() ? "No signals produced." : err;
            return 0;
        }
        for (const auto& n : f.importedNames) store_.remove(n);
        f.importedNames.clear();
        for (auto& s : sigs) {
            auto meta = s->meta();
            const QString unique = uniqueStoreName(store_, meta.name);
            if (unique != meta.name) {
                meta.name = unique;
                s->setMeta(meta);
            }
            f.importedNames << unique;
            store_.add(s);
        }
        return static_cast<int>(sigs.size());
    };

    auto applyH5With = [this](OpenedFile& f,
                              const QStringList& checked,
                              const QStringList& relative,
                              const QHash<QString, double>& offsets) -> int {
        for (const auto& n : f.importedNames) store_.remove(n);
        f.importedNames.clear();
        int n = 0;
        for (const auto& s : f.h5Signals) {
            if (!s) continue;
            const auto meta0 = s->meta();
            if (!checked.contains(meta0.name)) continue;

            const bool isRel  = relative.contains(meta0.name);
            const double offS = offsets.value(meta0.name, 0.0);
            const auto view   = s->snapshotForRead();
            const auto values = s->readAsDouble();

            auto meta = meta0;
            const QString unique = uniqueStoreName(store_, meta.name);
            meta.name = unique;

            std::shared_ptr<core::Signal> toAdd;
            if ((isRel || offS != 0.0) && view.count > 0) {
                const core::TimestampNs t0  = isRel ? view.timestamps[0] : 0;
                const core::TimestampNs off =
                    static_cast<core::TimestampNs>(std::llround(offS * 1e9));
                std::vector<core::TimestampNs> ts(view.count);
                for (std::size_t i = 0; i < view.count; ++i) {
                    ts[i] = view.timestamps[i] - t0 + off;
                }
                toAdd = std::make_shared<core::Signal>(meta);
                toAdd->append(
                    ts.data(),
                    reinterpret_cast<const std::byte*>(values.data()),
                    view.count);
            } else {
                if (unique != meta0.name) s->setMeta(meta);
                toAdd = s;
            }
            f.importedNames << unique;
            store_.add(toAdd);
            ++n;
        }
        return n;
    };

    auto applyCsv = [this, applyCsvWith](OpenedFile& f) {
        if (!f.csv) {
            QMessageBox::information(this, "No file",
                "This file isn't loaded.");
            return;
        }
        const auto profile = impl_->mapping->buildProfile("csv");
        f.profile = profile;
        QString err;
        const int n = applyCsvWith(f, profile, &err);
        if (n == 0) {
            QMessageBox::critical(this, "Import failed", err);
            return;
        }
        impl_->statusLabel->setText(
            QString("Imported %1 signal(s) from %2")
                .arg(n).arg(f.displayName));
        const auto dupWarns  = scanDuplicateTimestamps(store_, f.importedNames);
        const auto platWarns = scanValuePlateaus(store_, f.importedNames);
        if (!dupWarns.isEmpty() || !platWarns.isEmpty()) {
            QString msg;
            if (!dupWarns.isEmpty()) {
                msg += QString("Duplicate timestamps in %1:\n\n%2\n\n"
                               "Edit the channel and set "
                               "Duplicate timestamps → first / last / mean "
                               "to collapse them.\n\n")
                           .arg(f.displayName).arg(dupWarns.join("\n"));
            }
            if (!platWarns.isEmpty()) {
                msg += QString("Value plateaus in %1:\n\n%2\n\n"
                               "This is a CSV logged faster than the "
                               "underlying value updates. Edit the channel "
                               "and set Value plateaus → first / last "
                               "timestamp to collapse them. Otherwise "
                               "Derivative on this signal will produce a "
                               "comb pattern.")
                           .arg(f.displayName).arg(platWarns.join("\n"));
            }
            QMessageBox::warning(this, "Import warnings", msg);
        }
    };

    auto applyH5 = [this, applyH5With](OpenedFile& f) {
        const auto checked  = impl_->h5Selector->checkedNames();
        const auto relative = impl_->h5Selector->relativeNames();
        const auto offsets  = impl_->h5Selector->offsetMap();
        f.h5SelectedChannels = checked;
        f.h5RelativeChannels = relative;
        f.h5OffsetSec        = offsets;
        const int n = applyH5With(f, checked, relative, offsets);
        impl_->statusLabel->setText(
            QString("Imported %1 channel(s) from %2")
                .arg(n).arg(f.displayName));
    };

    connect(impl_->mapping, &ui::MappingPanel::applyRequested, this,
            [this, applyCsv]{
        if (impl_->activeIndex < 0) return;
        auto& f = *impl_->files[impl_->activeIndex];
        if (f.type != FileType::Csv) return;
        applyCsv(f);
    });
    impl_->h5Selector->onApply = [this, applyH5]{
        if (impl_->activeIndex < 0) return;
        auto& f = *impl_->files[impl_->activeIndex];
        if (f.type != FileType::H5) return;
        applyH5(f);
    };

    // ---- Parse-options change: rebuild CsvSource + preview ----------
    connect(impl_->mapping, &ui::MappingPanel::parseOptionsChanged, this, [this]{
        // While loadActiveState() / loadProfile / load-workspace are
        // programmatically setting MappingPanel widgets, the panel emits
        // parseOptionsChanged 3-4 times. Each emission would otherwise
        // re-read the whole CSV from disk and rebuild the preview model.
        // The file's csv / previewModel were already built with the
        // right delimiters when it was opened, so during a programmatic
        // load we just suppress.
        if (impl_->suppressSave) return;
        if (impl_->activeIndex < 0) return;
        auto& f = *impl_->files[impl_->activeIndex];
        if (f.type != FileType::Csv) return;
        // If the delimiters / parse options match what's already cached
        // on the CsvSource, nothing changed — skip the disk round-trip.
        if (f.csv
            && f.profile.columnDelimiter == impl_->mapping->columnDelimiter()
            && f.profile.rowDelimiter    == impl_->mapping->rowDelimiter()) {
            return;
        }
        try {
            f.csv = std::make_unique<CsvSource>(
                std::filesystem::path(f.path.toStdString()),
                impl_->mapping->columnDelimiter(),
                impl_->mapping->rowDelimiter());
            f.previewModel = f.csv->previewModel("file");
            impl_->preview->setModel(f.previewModel.get());
            impl_->statusLabel->setText(
                QString("%1: %2 rows × %3 cols")
                    .arg(f.displayName)
                    .arg(f.csv->rowCount())
                    .arg(f.csv->columnCount()));
        } catch (const std::exception& e) {
            QMessageBox::warning(this, "Reparse failed",
                QString::fromUtf8(e.what()));
        }
    });

    // ---- Auto-detect mapping (dropdown + Apply in the mapping panel) ----
    connect(impl_->mapping, &ui::MappingPanel::autoDetectRequested, this,
            [this](const QString& detectorId) {
        if (impl_->activeIndex < 0) {
            QMessageBox::information(this, "No file", "Open a CSV first.");
            return;
        }
        auto& f = *impl_->files[impl_->activeIndex];
        if (f.type != FileType::Csv) return;

        ConverterProfile prof;
        QString err;
        bool ok = false;
        if (detectorId == "twincat") {
            ok = converter::profileFromTwinCatScope(
                std::filesystem::path(f.path.toStdString()), &prof, &err);
        }
        if (!ok) {
            QMessageBox::information(this, "Auto-detect",
                QString("No recognised structure for the selected detector "
                        "was found in this file.%1")
                    .arg(err.isEmpty() ? QString() : "\n\n" + err));
            return;
        }

        // Rebuild the CsvSource with the detected delimiter so the preview
        // and a subsequent Apply parse the right columns, then push the
        // profile into the panel (suppress the parse-options churn).
        try {
            f.csv = std::make_unique<CsvSource>(
                std::filesystem::path(f.path.toStdString()),
                prof.columnDelimiter, prof.rowDelimiter);
            f.previewModel = f.csv->previewModel("file");
        } catch (const std::exception& e) {
            QMessageBox::warning(this, "Auto-detect failed",
                QString::fromUtf8(e.what()));
            return;
        }
        f.profile = prof;
        impl_->suppressSave = true;
        impl_->mapping->setProfile(prof);
        impl_->preview->setModel(f.previewModel.get());
        impl_->suppressSave = false;
        impl_->statusLabel->setText(
            QString("%1: auto-detected %2 channel(s) — review, then Apply")
                .arg(f.displayName)
                .arg(static_cast<int>(prof.columns.size()) / 2));
    });

    // ---- Save / Load profile (per-file scaconv) ---------------------
    connect(impl_->mapping, &ui::MappingPanel::saveProfileRequested, this, [this]{
        if (impl_->activeIndex < 0) {
            QMessageBox::information(this, "No file", "Open a CSV first.");
            return;
        }
        QFileDialog dlg(this, "Save profile");
        dlg.setAcceptMode(QFileDialog::AcceptSave);
        dlg.setNameFilters({"Scope conversion profile (*.scaconv)", "All files (*)"});
        dlg.setDefaultSuffix("scaconv");
        if (dlg.exec() != QDialog::Accepted) return;
        const QStringList sel = dlg.selectedFiles();
        if (sel.isEmpty()) return;
        QString path = sel.first();
        if (!path.endsWith(".scaconv", Qt::CaseInsensitive)) path += ".scaconv";

        const auto profile = impl_->mapping->buildProfile("csv");
        QString err;
        if (!profile.saveToFile(std::filesystem::path(path.toStdString()), &err)) {
            QMessageBox::critical(this, "Save failed", err);
            return;
        }
        impl_->statusLabel->setText(QString("Profile saved to %1")
                                        .arg(QFileInfo(path).fileName()));
    });

    connect(impl_->mapping, &ui::MappingPanel::loadProfileRequested, this, [this]{
        if (impl_->activeIndex < 0) {
            QMessageBox::information(this, "No file",
                "Open a CSV first, then load a profile.");
            return;
        }
        const QString path = QFileDialog::getOpenFileName(
            this, "Load profile", QString(),
            "Scope conversion profile (*.scaconv);;All files (*)");
        if (path.isEmpty()) return;
        QString err;
        const auto profile = ConverterProfile::loadFromFile(
            std::filesystem::path(path.toStdString()), &err);
        if (!err.isEmpty()) {
            QMessageBox::critical(this, "Load failed", err);
            return;
        }
        auto& f = *impl_->files[impl_->activeIndex];
        f.profile = profile;
        impl_->suppressSave = true;
        impl_->mapping->setProfile(profile);
        impl_->suppressSave = false;
        // Re-parse with the loaded delimiters.
        try {
            f.csv = std::make_unique<CsvSource>(
                std::filesystem::path(f.path.toStdString()),
                impl_->mapping->columnDelimiter(),
                impl_->mapping->rowDelimiter());
            f.previewModel = f.csv->previewModel("file");
            impl_->preview->setModel(f.previewModel.get());
        } catch (const std::exception& e) {
            QMessageBox::warning(this, "Reparse failed",
                QString::fromUtf8(e.what()));
        }
        impl_->statusLabel->setText(
            QString("Loaded profile %1 (%2 channel(s))")
                .arg(QFileInfo(path).fileName())
                .arg(profile.columns.size()));
    });

    // ---- Remove file: drop from list AND remove its signals --------
    connect(impl_->removeFileBtn, &QPushButton::clicked, this, [this]{
        if (impl_->activeIndex < 0) return;
        auto& f = *impl_->files[impl_->activeIndex];
        const auto resp = QMessageBox::question(
            this, "Remove file?",
            QString("Remove %1 from the workspace?\n\nThis also removes "
                    "%2 signal(s) it imported from the store.")
                .arg(f.displayName).arg(f.importedNames.size()),
            QMessageBox::Yes | QMessageBox::Cancel);
        if (resp != QMessageBox::Yes) return;

        for (const auto& n : f.importedNames) store_.remove(n);

        const int idx = impl_->activeIndex;
        {
            QSignalBlocker b(impl_->fileList);
            delete impl_->fileList->takeItem(idx);
        }
        impl_->files.erase(impl_->files.begin() + idx);
        impl_->lastH5Source = nullptr;   // erased file may have been it
        impl_->activeIndex = -1;
        const int newIdx = impl_->files.empty()
                              ? -1
                              : std::min(idx, (int)impl_->files.size() - 1);
        if (newIdx >= 0) {
            impl_->fileList->setCurrentRow(newIdx);  // triggers load
        } else {
            impl_->preview->setModel(nullptr);
            impl_->rightStack->setCurrentIndex(0);
            impl_->statusLabel->setText("No file open");
            impl_->removeFileBtn->setEnabled(false);
        }
    });

    // ---- Apply all: apply every file in the workspace to the store --
    connect(applyAllBtn, &QPushButton::clicked, this,
            [this, saveActiveState, applyCsvWith, applyH5With]{
        if (impl_->files.empty()) {
            QMessageBox::information(this, "Nothing to apply",
                "Open at least one file first.");
            return;
        }
        // Flush the active panel's edits into its file before iterating.
        saveActiveState();
        int totalSignals = 0, okFiles = 0;
        QStringList failures;
        QStringList dupWarnings;
        QStringList platWarnings;
        for (auto& fp : impl_->files) {
            auto& f = *fp;
            int n = 0;
            QString err;
            if (f.type == FileType::Csv) {
                n = applyCsvWith(f, f.profile, &err);
            } else {
                n = applyH5With(f, f.h5SelectedChannels,
                                f.h5RelativeChannels, f.h5OffsetSec);
            }
            if (n > 0) {
                totalSignals += n; ++okFiles;
                for (const auto& w : scanDuplicateTimestamps(store_, f.importedNames))
                    dupWarnings << QString("%1 — %2").arg(f.displayName, w);
                for (const auto& w : scanValuePlateaus(store_, f.importedNames))
                    platWarnings << QString("%1 — %2").arg(f.displayName, w);
            } else {
                failures << QString("%1%2")
                                .arg(f.displayName)
                                .arg(err.isEmpty() ? QString()
                                                   : QString(" (%1)").arg(err));
            }
        }
        QString msg = QString("Applied %1 file(s), %2 signal(s) imported")
                          .arg(okFiles).arg(totalSignals);
        if (!failures.isEmpty()) {
            msg += QString(" — %1 skipped").arg(failures.size());
            QMessageBox::warning(this, "Some files were skipped",
                "These files produced no signals:\n" + failures.join("\n"));
        }
        if (!dupWarnings.isEmpty() || !platWarnings.isEmpty()) {
            QString w;
            if (!dupWarnings.isEmpty()) {
                w += "Duplicate timestamps:\n\n"
                   + dupWarnings.join("\n")
                   + "\n\nEdit the channel and set Duplicate timestamps "
                     "→ first / last / mean to collapse.\n\n";
            }
            if (!platWarnings.isEmpty()) {
                w += "Value plateaus (CSV logged faster than the value "
                     "updates):\n\n"
                   + platWarnings.join("\n")
                   + "\n\nEdit the channel and set Value plateaus → "
                     "first / last timestamp to collapse. Derivative "
                     "on a staircase signal produces a comb pattern.";
            }
            QMessageBox::warning(this, "Import warnings", w);
        }
        impl_->statusLabel->setText(msg);
    });

    // ---- Save workspace (.scaws) -----------------------------------
    connect(saveWsBtn, &QPushButton::clicked, this, [this, saveActiveState]{
        if (impl_->files.empty()) {
            QMessageBox::information(this, "Nothing to save",
                "Open at least one file first.");
            return;
        }
        saveActiveState();
        QFileDialog dlg(this, "Save workspace");
        dlg.setAcceptMode(QFileDialog::AcceptSave);
        dlg.setNameFilters({"Scope workspace (*.scaws)", "All files (*)"});
        dlg.setDefaultSuffix("scaws");
        if (dlg.exec() != QDialog::Accepted) return;
        const QStringList sel = dlg.selectedFiles();
        if (sel.isEmpty()) return;
        QString path = sel.first();
        if (!path.endsWith(".scaws", Qt::CaseInsensitive)) path += ".scaws";

        nlohmann::json j;
        j["version"] = 1;
        j["files"] = nlohmann::json::array();
        for (const auto& fp : impl_->files) {
            const auto& f = *fp;
            nlohmann::json jf;
            jf["path"] = f.path.toStdString();
            jf["displayName"] = f.displayName.toStdString();
            if (f.type == FileType::Csv) {
                jf["type"] = "csv";
                QString perr;
                auto tmp = std::filesystem::temp_directory_path()
                         / "ScopeAnalyser_ws_tmp.scaconv";
                // Round-trip the profile through its own save/load so the
                // workspace embeds the canonical JSON form.
                if (f.profile.saveToFile(tmp, &perr)) {
                    std::ifstream is(tmp);
                    nlohmann::json jp; is >> jp;
                    jf["profile"] = jp;
                    std::error_code ec; std::filesystem::remove(tmp, ec);
                }
            } else {
                jf["type"] = "h5";
                jf["selectedChannels"] = nlohmann::json::array();
                for (const auto& n : f.h5SelectedChannels)
                    jf["selectedChannels"].push_back(n.toStdString());
                jf["relativeChannels"] = nlohmann::json::array();
                for (const auto& n : f.h5RelativeChannels)
                    jf["relativeChannels"].push_back(n.toStdString());
                nlohmann::json jOff = nlohmann::json::object();
                for (auto it = f.h5OffsetSec.constBegin();
                     it != f.h5OffsetSec.constEnd(); ++it) {
                    jOff[it.key().toStdString()] = it.value();
                }
                jf["offsets"] = jOff;
            }
            j["files"].push_back(std::move(jf));
        }

        try {
            std::ofstream os(path.toStdString());
            os << j.dump(2);
        } catch (const std::exception& e) {
            QMessageBox::critical(this, "Save failed", e.what());
            return;
        }
        impl_->statusLabel->setText(
            QString("Workspace saved: %1 file(s)").arg(impl_->files.size()));
    });

    // ---- Load workspace --------------------------------------------
    connect(loadWsBtn, &QPushButton::clicked, this,
            [this, addFileToList, loadActiveState]{
        const QString path = QFileDialog::getOpenFileName(
            this, "Load workspace", QString(),
            "Scope workspace (*.scaws);;All files (*)");
        if (path.isEmpty()) return;

        nlohmann::json j;
        try {
            std::ifstream is(path.toStdString());
            is >> j;
        } catch (const std::exception& e) {
            QMessageBox::critical(this, "Load failed", e.what());
            return;
        }

        // Drop everything (and remove all signals these files imported).
        for (auto& fp : impl_->files)
            for (const auto& n : fp->importedNames) store_.remove(n);
        impl_->files.clear();
        impl_->fileList->clear();
        impl_->lastH5Source = nullptr;
        impl_->activeIndex = -1;

        QStringList notFound;
        for (const auto& jf : j.value("files", nlohmann::json::array())) {
            const QString filePath = QString::fromStdString(
                jf.value("path", std::string{}));
            const QString type = QString::fromStdString(
                jf.value("type", std::string{}));
            if (!std::filesystem::exists(filePath.toStdString())) {
                notFound << filePath;
                continue;
            }
            auto f = std::make_unique<OpenedFile>();
            f->path = filePath;
            f->displayName = QString::fromStdString(
                jf.value("displayName", QFileInfo(filePath).fileName().toStdString()));
            if (type == "csv") {
                f->type = FileType::Csv;
                if (jf.contains("profile")) {
                    auto tmp = std::filesystem::temp_directory_path()
                             / "ScopeAnalyser_ws_load_tmp.scaconv";
                    {
                        std::ofstream os(tmp);
                        os << jf["profile"].dump(2);
                    }
                    QString perr;
                    f->profile = ConverterProfile::loadFromFile(tmp, &perr);
                    std::error_code ec; std::filesystem::remove(tmp, ec);
                }
                try {
                    f->csv = std::make_unique<CsvSource>(
                        std::filesystem::path(filePath.toStdString()),
                        f->profile.columnDelimiter.isEmpty()
                            ? QString(",") : f->profile.columnDelimiter,
                        f->profile.rowDelimiter.isEmpty()
                            ? QString("\n") : f->profile.rowDelimiter);
                    f->previewModel = f->csv->previewModel("file");
                } catch (const std::exception& e) {
                    QMessageBox::warning(this, "Reparse failed",
                        QString("%1:\n%2").arg(filePath, e.what()));
                    continue;
                }
            } else if (type == "h5") {
                f->type = FileType::H5;
                auto rr = converter::loadFile(
                    std::filesystem::path(filePath.toStdString()));
                if (!rr.ok && rr.channels.empty()) {
                    QMessageBox::warning(this, "H5 reopen failed",
                        QString("%1:\n%2").arg(filePath, rr.error));
                    continue;
                }
                f->h5Signals = std::move(rr.channels);
                for (const auto& jn : jf.value("selectedChannels",
                                               nlohmann::json::array()))
                    f->h5SelectedChannels << QString::fromStdString(
                        jn.get<std::string>());
                for (const auto& jn : jf.value("relativeChannels",
                                               nlohmann::json::array()))
                    f->h5RelativeChannels << QString::fromStdString(
                        jn.get<std::string>());
                if (jf.contains("offsets") && jf["offsets"].is_object()) {
                    for (const auto& kv : jf["offsets"].items()) {
                        f->h5OffsetSec.insert(
                            QString::fromStdString(kv.key()),
                            kv.value().get<double>());
                    }
                }
            } else {
                continue;
            }
            impl_->files.push_back(std::move(f));
            addFileToList(*impl_->files.back());
        }

        if (!impl_->files.empty()) {
            impl_->fileList->setCurrentRow(0);  // triggers load
        } else {
            loadActiveState();
        }
        QString msg = QString("Workspace loaded: %1 file(s)")
                          .arg(impl_->files.size());
        if (!notFound.isEmpty()) {
            msg += QString(" (missing: %1)").arg(notFound.size());
        }
        impl_->statusLabel->setText(msg);
    });

    // ---- Save chart: same dialog the Analyser uses ----
    connect(saveChartBtn, &QPushButton::clicked, this, [this]{
        if (store_.size() == 0) {
            QMessageBox::information(this, "Nothing to save",
                "The signal store is empty. Apply a file first.");
            return;
        }
        // No plot here → custom range box opens at 0…0, but it's
        // disabled unless the user picks "Custom range" anyway.
        converter::ui::SaveChartDialog dlg(0.0, 0.0, this);
        if (dlg.exec() != QDialog::Accepted) return;

        const auto fmtChoice = dlg.format();
        const converter::FileFormat fmt =
              (fmtChoice == converter::ui::SaveChartDialog::Format::Csv)  ? converter::FileFormat::Csv
            : (fmtChoice == converter::ui::SaveChartDialog::Format::Mdf4) ? converter::FileFormat::Mdf4
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
        filters.splitDomainsIntoTwoFiles = dlg.splitDomainsIntoTwoFiles();
        filters.useCustomRange           = dlg.useCustomRange();
        filters.fromSec                  = dlg.fromSec();
        filters.toSec                    = dlg.toSec();

        converter::SaveOptions opts;
        opts.csv = dlg.csvOptions();

        // Write off the GUI thread behind a busy dialog (shared with the
        // Analyser); saveChartFromStore reads the store via its
        // concurrent-read path, so this is safe.
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
                    target, store_, fmt, filters, opts, &msgs, &err);
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
                impl_->statusLabel->setText("Saved: " + outcome.messages.join("; "));
                QMessageBox::information(this, "Saved", outcome.messages.join("\n"));
                return;
        }
    });
}

}  // namespace scope::converter
