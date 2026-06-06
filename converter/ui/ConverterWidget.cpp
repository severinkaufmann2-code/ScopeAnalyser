#include "scope/converter/ConverterWidget.h"
#include "scope/converter/ConverterProfile.h"
#include "scope/converter/CsvSource.h"
#include "scope/converter/CsvWriter.h"
#include "scope/core/Hdf5Session.h"

#include "MappingPanel.h"
#include "CsvExportDialog.h"

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

        auto* allBtn   = new QPushButton("Select all", this);
        auto* noneBtn  = new QPushButton("Select none", this);
        auto* applyBtn = new QPushButton("Apply (import selected)", this);

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
        root->addWidget(new QLabel("HDF5 channels (tick to import):", this));
        root->addWidget(table, /*stretch=*/1);
        root->addLayout(btnRow);
        root->addLayout(bulkRow);
        root->addWidget(applyBtn);
    }

    void setSignals(const std::vector<std::shared_ptr<core::Signal>>& sigs,
                    const QStringList& preChecked,
                    const QStringList& preRelative,
                    const QHash<QString, double>& preOffsets) {
        table->setRowCount(0);
        for (const auto& s : sigs) {
            if (!s) continue;
            const auto meta = s->meta();
            const auto view = s->snapshotForRead();
            const int r = table->rowCount();
            table->insertRow(r);
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
        }
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
    // inside a programmatic switch.
    bool suppressSave{false};
};

// Forward decls of helper methods on ConverterWidget
ConverterWidget::~ConverterWidget() = default;

// ----------------------------- Helpers --------------------------------

namespace {

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

    auto* openCsvBtn  = new QPushButton("Open CSV…", this);
    auto* loadH5Btn   = new QPushButton("Open .h5…", this);
    auto* saveWsBtn   = new QPushButton("Save workspace…", this);
    auto* loadWsBtn   = new QPushButton("Load workspace…", this);
    auto* applyAllBtn = new QPushButton("Apply all (import signals)", this);
    auto* saveH5Btn   = new QPushButton("Save .h5…", this);
    auto* saveCsvBtn  = new QPushButton("Save CSV…", this);

    impl_->statusLabel = new QLabel("No file open", this);

    auto* topBar = new QHBoxLayout();
    topBar->addWidget(openCsvBtn);
    topBar->addWidget(loadH5Btn);
    topBar->addSpacing(20);
    topBar->addWidget(saveWsBtn);
    topBar->addWidget(loadWsBtn);
    topBar->addWidget(applyAllBtn);
    topBar->addSpacing(20);
    topBar->addWidget(saveH5Btn);
    topBar->addWidget(saveCsvBtn);
    topBar->addStretch();
    topBar->addWidget(impl_->statusLabel);

    // ---- Left: file list + remove button -----------------------------
    impl_->fileList = new QListWidget(this);
    impl_->fileList->setMinimumWidth(180);
    impl_->fileList->setMaximumWidth(260);
    impl_->removeFileBtn = new QPushButton("Remove file", this);
    impl_->removeFileBtn->setEnabled(false);

    auto* leftPanel = new QWidget(this);
    auto* leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->addWidget(new QLabel("Opened files:", this));
    leftLayout->addWidget(impl_->fileList, /*stretch=*/1);
    leftLayout->addWidget(impl_->removeFileBtn);

    // ---- Middle: preview --------------------------------------------
    impl_->preview = new QTableView(this);
    impl_->preview->setEditTriggers(QAbstractItemView::NoEditTriggers);
    impl_->preview->setSelectionMode(QAbstractItemView::NoSelection);
    impl_->preview->horizontalHeader()->setDefaultSectionSize(100);

    // ---- Right: mapping panel (CSV) or H5 channel selector ----------
    impl_->mapping    = new ui::MappingPanel(this);
    impl_->h5Selector = new H5SelectorPanel(this);
    impl_->rightStack = new QStackedWidget(this);
    impl_->rightStack->addWidget(impl_->mapping);    // index 0 = CSV
    impl_->rightStack->addWidget(impl_->h5Selector); // index 1 = H5

    auto* split = new QSplitter(Qt::Horizontal, this);
    split->addWidget(leftPanel);
    split->addWidget(impl_->preview);
    split->addWidget(impl_->rightStack);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 2);
    split->setStretchFactor(2, 1);

    auto* root = new QVBoxLayout(this);
    root->addLayout(topBar);
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
            impl_->h5Selector->setSignals(f.h5Signals,
                                          f.h5SelectedChannels,
                                          f.h5RelativeChannels,
                                          f.h5OffsetSec);
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
    connect(openCsvBtn, &QPushButton::clicked, this,
            [this, saveActiveState, loadActiveState, addFileToList]{
        const QString path = QFileDialog::getOpenFileName(
            this, "Open CSV file", QString(),
            "CSV / text files (*.csv *.tsv *.txt);;All files (*)");
        if (path.isEmpty()) return;

        auto f = std::make_unique<OpenedFile>();
        f->type = FileType::Csv;
        f->path = path;
        f->displayName = QFileInfo(path).fileName();
        f->profile = ConverterProfile{};
        f->profile.sourceType = "csv";
        f->profile.columnDelimiter = ",";
        f->profile.rowDelimiter    = "\n";
        f->profile.headerRow = 1;
        f->profile.decimalSeparator = ".";
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
    });

    // ---- Open .h5 (loads all signals into memory, doesn't push to store
    //               until user selects + Applies) ----------------------
    connect(loadH5Btn, &QPushButton::clicked, this,
            [this, saveActiveState, loadActiveState, addFileToList]{
        const QString path = QFileDialog::getOpenFileName(
            this, "Open recording (.h5)", QString(),
            "Scope sessions (*.h5);;All files (*)");
        if (path.isEmpty()) return;

        QString err;
        auto session = core::Hdf5Session::openForRead(
            std::filesystem::path(path.toStdString()), &err);
        if (!session) {
            QMessageBox::critical(this, "Open failed",
                err.isEmpty() ? QString("Unknown error") : err);
            return;
        }
        auto loaded = session->loadAllSignals(&err);
        if (loaded.empty()) {
            QMessageBox::warning(this, "Empty file",
                err.isEmpty() ? QString("No channels in this file.") : err);
            return;
        }

        auto f = std::make_unique<OpenedFile>();
        f->type = FileType::H5;
        f->path = path;
        f->displayName = QFileInfo(path).fileName();
        f->h5Signals = std::move(loaded);
        // Default: all channels checked (so the user just clicks Apply
        // to get the old behaviour).
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
        auto sigs = f.csv->apply(profile, &err);
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
        if (impl_->activeIndex < 0) return;
        auto& f = *impl_->files[impl_->activeIndex];
        if (f.type != FileType::Csv) return;
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
            if (n > 0) { totalSignals += n; ++okFiles; }
            else {
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
                QString h5err;
                auto session = core::Hdf5Session::openForRead(
                    std::filesystem::path(filePath.toStdString()), &h5err);
                if (!session) {
                    QMessageBox::warning(this, "H5 reopen failed",
                        QString("%1:\n%2").arg(filePath, h5err));
                    continue;
                }
                f->h5Signals = session->loadAllSignals(&h5err);
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

    // ---- Save HDF5: dump the SignalStore to a recording file ----------
    connect(saveH5Btn, &QPushButton::clicked, this, [this]{
        if (store_.size() == 0) {
            QMessageBox::information(this, "Nothing to save",
                "The signal store is empty. Apply a file first.");
            return;
        }
        QFileDialog dlg(this, "Save recording (.h5)");
        dlg.setAcceptMode(QFileDialog::AcceptSave);
        dlg.setNameFilters({"Scope sessions (*.h5)", "All files (*)"});
        dlg.setDefaultSuffix("h5");
        if (dlg.exec() != QDialog::Accepted) return;
        const auto sel = dlg.selectedFiles();
        if (sel.isEmpty()) return;
        QString path = sel.first();
        if (!path.endsWith(".h5", Qt::CaseInsensitive)) path += ".h5";

        QString err;
        auto session = scope::core::Hdf5Session::create(
            std::filesystem::path(path.toStdString()), &err);
        if (!session) {
            QMessageBox::critical(this, "Save failed", err);
            return;
        }
        int written = 0;
        for (const auto& name : store_.channelNames()) {
            auto sig = store_.get(name);
            if (!sig) continue;
            if (!session->addChannel(sig->meta(), &err)) continue;
            auto view = sig->snapshotForRead();
            if (view.count > 0) {
                session->appendSamples(name, view.timestamps, view.values,
                                       view.count, &err);
            }
            ++written;
        }
        session->flush();
        impl_->statusLabel->setText(QString("Saved %1: %2 channel(s)")
                                        .arg(QFileInfo(path).fileName())
                                        .arg(written));
    });

    // ---- Save CSV: dialog + writer ------------------------------------
    connect(saveCsvBtn, &QPushButton::clicked, this, [this]{
        if (store_.size() == 0) {
            QMessageBox::information(this, "Nothing to save",
                "The signal store is empty. Apply a file first.");
            return;
        }
        ui::CsvExportDialog optionsDlg(this);
        if (optionsDlg.exec() != QDialog::Accepted) return;
        const auto opts = optionsDlg.options();

        QFileDialog dlg(this, "Save CSV");
        dlg.setAcceptMode(QFileDialog::AcceptSave);
        dlg.setNameFilters({"CSV (*.csv)", "Text (*.txt)", "All files (*)"});
        dlg.setDefaultSuffix("csv");
        if (dlg.exec() != QDialog::Accepted) return;
        const auto sel = dlg.selectedFiles();
        if (sel.isEmpty()) return;
        QString path = sel.first();
        if (!path.contains('.')) path += ".csv";

        std::vector<std::shared_ptr<scope::core::Signal>> chans;
        for (const auto& name : store_.channelNames()) {
            if (auto s = store_.get(name)) chans.push_back(std::move(s));
        }
        QString err;
        if (!writeCsv(std::filesystem::path(path.toStdString()), chans, opts, &err)) {
            QMessageBox::critical(this, "Save failed", err);
            return;
        }
        impl_->statusLabel->setText(QString("Saved %1: %2 channel(s)")
                                        .arg(QFileInfo(path).fileName())
                                        .arg(chans.size()));
    });
}

}  // namespace scope::converter
