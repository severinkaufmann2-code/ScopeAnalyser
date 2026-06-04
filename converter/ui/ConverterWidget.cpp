#include "scope/converter/ConverterWidget.h"
#include "scope/converter/ConverterProfile.h"
#include "scope/converter/CsvSource.h"

#include "MappingPanel.h"

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

    auto* openBtn = new QPushButton("Open CSV…", this);

    impl_->preview = new QTableView(this);
    impl_->preview->setEditTriggers(QAbstractItemView::NoEditTriggers);
    impl_->preview->setSelectionMode(QAbstractItemView::NoSelection);
    impl_->preview->horizontalHeader()->setDefaultSectionSize(100);

    impl_->mapping     = new ui::MappingPanel(this);
    impl_->statusLabel = new QLabel("No file open", this);

    auto* topBar = new QHBoxLayout();
    topBar->addWidget(openBtn);
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

    connect(openBtn, &QPushButton::clicked, this, [this, reparseCurrentFile]{
        const QString path = QFileDialog::getOpenFileName(
            this, "Open CSV file", QString(),
            "CSV / text files (*.csv *.tsv *.txt);;All files (*)");
        if (path.isEmpty()) return;
        impl_->currentFile = path;
        reparseCurrentFile();
    });

    connect(impl_->mapping, &ui::MappingPanel::parseOptionsChanged,
            this, [this, reparseCurrentFile]{ reparseCurrentFile(); });

    connect(impl_->mapping, &ui::MappingPanel::applyRequested, this, [this]{
        if (!impl_->csv) { QMessageBox::information(this, "No file", "Open a CSV first."); return; }
        const auto profile = impl_->mapping->buildProfile("csv");
        QString err;
        auto sigs = impl_->csv->apply(profile, &err);
        if (sigs.empty()) {
            QMessageBox::critical(this, "Import failed",
                                  err.isEmpty() ? "No signals produced." : err);
            return;
        }
        for (auto& s : sigs) store_.add(s);
        impl_->statusLabel->setText(QString("Imported %1 signal(s)").arg(sigs.size()));
    });

    connect(impl_->mapping, &ui::MappingPanel::saveProfileRequested, this, [this]{
        QString path = QFileDialog::getSaveFileName(
            this, "Save profile", QString(),
            "Scope conversion profile (*.scaconv);;All files (*)");
        if (path.isEmpty()) return;
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
}

ConverterWidget::~ConverterWidget() = default;

}  // namespace scope::converter
