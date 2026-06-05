#include "scope/converter/ConverterWidget.h"
#include "scope/converter/ConverterProfile.h"
#include "scope/converter/CsvSource.h"
#include "scope/converter/CsvWriter.h"
#include "scope/core/Hdf5Session.h"

#include "MappingPanel.h"
#include "CsvExportDialog.h"

#include <QAbstractItemModel>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QTableView>
#include <QVBoxLayout>

#include <filesystem>
#include <memory>

namespace scope::converter {

struct ConverterWidget::Impl {
    QTableView*           preview{nullptr};
    ui::MappingPanel*     mapping{nullptr};
    QLabel*               statusLabel{nullptr};
    std::unique_ptr<CsvSource>          csv;
    std::unique_ptr<QAbstractItemModel> previewModel;
    QString               currentFile;
};

ConverterWidget::ConverterWidget(scope::core::SignalStore& store, QWidget* parent)
    : QWidget(parent), store_(store), impl_(std::make_unique<Impl>()) {

    auto* openBtn    = new QPushButton("Open CSV…", this);
    auto* loadH5Btn  = new QPushButton("Load .h5…", this);
    auto* saveH5Btn  = new QPushButton("Save .h5…", this);
    auto* saveCsvBtn = new QPushButton("Save CSV…", this);

    impl_->preview = new QTableView(this);
    impl_->preview->setEditTriggers(QAbstractItemView::NoEditTriggers);
    impl_->preview->setSelectionMode(QAbstractItemView::NoSelection);
    impl_->preview->horizontalHeader()->setDefaultSectionSize(100);

    impl_->mapping     = new ui::MappingPanel(this);
    impl_->statusLabel = new QLabel("No file open", this);

    auto* topBar = new QHBoxLayout();
    topBar->addWidget(openBtn);
    topBar->addWidget(loadH5Btn);
    topBar->addSpacing(20);
    topBar->addWidget(saveH5Btn);
    topBar->addWidget(saveCsvBtn);
    topBar->addStretch();
    topBar->addWidget(impl_->statusLabel);

    auto* split = new QSplitter(Qt::Horizontal, this);
    split->addWidget(impl_->preview);
    split->addWidget(impl_->mapping);
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 1);

    auto* root = new QVBoxLayout(this);
    root->addLayout(topBar);
    root->addWidget(split, /*stretch=*/1);

    auto reparseCurrentFile = [this]{
        if (impl_->currentFile.isEmpty()) return;
        impl_->csv = std::make_unique<CsvSource>(
            std::filesystem::path(impl_->currentFile.toStdString()),
            impl_->mapping->columnDelimiter(),
            impl_->mapping->rowDelimiter());
        impl_->previewModel = impl_->csv->previewModel("file");
        impl_->preview->setModel(impl_->previewModel.get());
        impl_->statusLabel->setText(QString("%1: %2 rows × %3 cols")
                                        .arg(QFileInfo(impl_->currentFile).fileName())
                                        .arg(impl_->csv->rowCount())
                                        .arg(impl_->csv->columnCount()));
    };

    // Apply the current mappings to the currently-loaded CsvSource and push
    // the imported signals into the store. Returns the number of signals
    // produced. On error, writes the message to *errOut (if provided) and
    // returns 0. Shared by the Apply button and the auto-apply step when
    // switching CSV files.
    auto applyCurrentMappings = [this](QString* errOut) -> std::size_t {
        if (!impl_->csv) {
            if (errOut) *errOut = "No CSV file loaded.";
            return 0;
        }
        const auto profile = impl_->mapping->buildProfile("csv");
        QString err;
        auto sigs = impl_->csv->apply(profile, &err);
        if (sigs.empty()) {
            if (errOut) *errOut = err.isEmpty() ? "No signals produced." : err;
            return 0;
        }
        for (auto& s : sigs) store_.add(s);
        return sigs.size();
    };

    connect(openBtn, &QPushButton::clicked, this,
            [this, reparseCurrentFile, applyCurrentMappings]{
        const QString path = QFileDialog::getOpenFileName(
            this, "Open CSV file", QString(),
            "CSV / text files (*.csv *.tsv *.txt);;All files (*)");
        if (path.isEmpty()) return;

        // Opening a *different* file with mappings in the table would
        // silently drop the previous file's data — there's no way to
        // re-apply mappings to a file that has already been closed.
        // Auto-apply the current mappings to the current file before
        // switching, so the previous file's signals land in the store.
        const bool isDifferentFile = !impl_->currentFile.isEmpty()
                                  && impl_->currentFile != path;
        if (isDifferentFile && impl_->csv
            && impl_->mapping->channelRowCount() > 0) {
            QString err;
            const QString prevName = QFileInfo(impl_->currentFile).fileName();
            const std::size_t n = applyCurrentMappings(&err);
            if (n > 0) {
                impl_->statusLabel->setText(
                    QString("Imported %1 signal(s) from %2 before opening %3")
                        .arg(n).arg(prevName)
                        .arg(QFileInfo(path).fileName()));
            } else {
                const auto resp = QMessageBox::warning(
                    this, "Previous mappings couldn't be applied",
                    QString("Couldn't apply the current mappings to %1:\n%2"
                            "\n\nOpening %3 will discard the unapplied "
                            "mappings. Continue?")
                        .arg(prevName)
                        .arg(err.isEmpty() ? "no signals produced" : err)
                        .arg(QFileInfo(path).fileName()),
                    QMessageBox::Yes | QMessageBox::Cancel);
                if (resp != QMessageBox::Yes) return;
            }
        }

        impl_->currentFile = path;
        reparseCurrentFile();
    });

    connect(impl_->mapping, &ui::MappingPanel::parseOptionsChanged,
            this, [this, reparseCurrentFile]{ reparseCurrentFile(); });

    connect(impl_->mapping, &ui::MappingPanel::applyRequested, this,
            [this, applyCurrentMappings]{
        if (!impl_->csv) {
            QMessageBox::information(this, "No file", "Open a CSV first.");
            return;
        }
        QString err;
        const std::size_t n = applyCurrentMappings(&err);
        if (n == 0) {
            QMessageBox::critical(this, "Import failed", err);
            return;
        }
        impl_->statusLabel->setText(QString("Imported %1 signal(s)").arg(n));
    });

    connect(impl_->mapping, &ui::MappingPanel::saveProfileRequested, this, [this]{
        // Use the non-static dialog API so we can set a default suffix.
        // With setDefaultSuffix, Qt appends ".scaconv" if the user didn't
        // type it AND runs the "file already exists?" check against the
        // full name — so an unrelated extensionless file with the same
        // base name doesn't trigger a bogus overwrite prompt.
        QFileDialog dlg(this, "Save profile");
        dlg.setAcceptMode(QFileDialog::AcceptSave);
        dlg.setNameFilters({"Scope conversion profile (*.scaconv)",
                            "All files (*)"});
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

    connect(impl_->mapping, &ui::MappingPanel::loadProfileRequested, this, [this, reparseCurrentFile]{
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
        impl_->mapping->setProfile(profile);
        reparseCurrentFile();
        impl_->statusLabel->setText(
            QString("Loaded profile %1 (%2 channel(s))")
                .arg(QFileInfo(path).fileName())
                .arg(profile.columns.size()));
    });

    // ---- Load HDF5: push every channel in the file into the store ------
    auto uniquify = [this](const QString& base) {
        if (!store_.contains(base)) return base;
        for (int i = 2; i < 1000; ++i) {
            QString c = QString("%1 (%2)").arg(base).arg(i);
            if (!store_.contains(c)) return c;
        }
        return base + " (?)";
    };

    connect(loadH5Btn, &QPushButton::clicked, this, [this, uniquify]{
        const QString path = QFileDialog::getOpenFileName(
            this, "Load recording (.h5)", QString(),
            "Scope sessions (*.h5);;All files (*)");
        if (path.isEmpty()) return;
        QString err;
        auto session = scope::core::Hdf5Session::openForRead(
            std::filesystem::path(path.toStdString()), &err);
        if (!session) {
            QMessageBox::critical(this, "Load failed",
                                  err.isEmpty() ? QString("Unknown error") : err);
            return;
        }
        auto loaded = session->loadAllSignals(&err);
        if (loaded.empty()) {
            QMessageBox::warning(this, "Empty file",
                err.isEmpty() ? QString("No channels in this file.") : err);
            return;
        }
        for (auto& s : loaded) {
            auto meta = s->meta();
            meta.name = uniquify(meta.name);
            s->setMeta(meta);
            store_.add(s);
        }
        impl_->statusLabel->setText(QString("Loaded %1: %2 channel(s)")
                                        .arg(QFileInfo(path).fileName())
                                        .arg(loaded.size()));
    });

    // ---- Save HDF5: dump the SignalStore to a recording file ----------
    connect(saveH5Btn, &QPushButton::clicked, this, [this]{
        if (store_.size() == 0) {
            QMessageBox::information(this, "Nothing to save",
                "The signal store is empty. Apply a CSV or Load an .h5 first.");
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
                "The signal store is empty. Apply a CSV or Load an .h5 first.");
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

ConverterWidget::~ConverterWidget() = default;

}  // namespace scope::converter
