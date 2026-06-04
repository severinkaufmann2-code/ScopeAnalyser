#include "scope/converter/ConverterWidget.h"
#include "scope/converter/ConverterProfile.h"
#include "scope/converter/CsvSource.h"

#include "MappingPanel.h"

#include <QAbstractItemModel>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QTableView>
#include <QVBoxLayout>

#include <filesystem>
#include <memory>
#include <set>

namespace scope::converter {

namespace {

QString colLabel(int section) {
    QString s;
    int n = section;
    while (true) {
        s.prepend(QChar('A' + (n % 26)));
        n = n / 26 - 1;
        if (n < 0) break;
    }
    return s;
}

struct Selection {
    std::vector<int> columns;   // sorted, unique
    int rowStart{-1};
    int rowEnd{-1};
    bool empty() const { return columns.empty(); }
};

Selection currentSelection(QTableView* view) {
    Selection out;
    auto idxs = view->selectionModel()->selectedIndexes();
    if (idxs.isEmpty()) return out;
    std::set<int> cols;
    int rLo = idxs.front().row();
    int rHi = rLo;
    for (const auto& idx : idxs) {
        cols.insert(idx.column());
        rLo = std::min(rLo, idx.row());
        rHi = std::max(rHi, idx.row());
    }
    out.columns.assign(cols.begin(), cols.end());
    out.rowStart = rLo;
    out.rowEnd   = rHi;
    return out;
}

}  // namespace

struct ConverterWidget::Impl {
    QTableView*           preview{nullptr};
    ui::MappingPanel*     mapping{nullptr};
    QLabel*               statusLabel{nullptr};
    QLineEdit*            yNameEdit{nullptr};
    QLineEdit*            yUnitEdit{nullptr};
    std::unique_ptr<CsvSource>          csv;
    std::unique_ptr<QAbstractItemModel> previewModel;
    QString               currentFile;

    QStringList currentColumnLabels() const {
        QStringList out;
        if (!csv) return out;
        for (int i = 0; i < csv->columnCount(); ++i) out << colLabel(i);
        return out;
    }
};

ConverterWidget::ConverterWidget(scope::core::SignalStore& store, QWidget* parent)
    : QWidget(parent), store_(store), impl_(std::make_unique<Impl>()) {

    auto* openBtn = new QPushButton("Open CSV…", this);

    impl_->preview = new QTableView(this);
    impl_->preview->setEditTriggers(QAbstractItemView::NoEditTriggers);
    impl_->preview->setSelectionMode(QAbstractItemView::ExtendedSelection);
    impl_->preview->setSelectionBehavior(QAbstractItemView::SelectItems);
    impl_->preview->horizontalHeader()->setDefaultSectionSize(100);

    impl_->mapping     = new ui::MappingPanel(this);
    impl_->statusLabel = new QLabel("No file open", this);
    impl_->yNameEdit   = new QLineEdit(this);
    impl_->yNameEdit->setPlaceholderText("Signal name");
    impl_->yNameEdit->setMaximumWidth(140);
    impl_->yUnitEdit   = new QLineEdit(this);
    impl_->yUnitEdit->setPlaceholderText("Unit");
    impl_->yUnitEdit->setMaximumWidth(80);

    auto* setXBtn = new QPushButton("Set selection as X-axis", this);
    auto* setYBtn = new QPushButton("Set selection as Y signal", this);
    auto* useRateBtn = new QPushButton("Use sample rate as X-axis", this);
    auto* clearXBtn = new QPushButton("Clear X-axis", this);

    auto* topBar = new QHBoxLayout();
    topBar->addWidget(openBtn);
    topBar->addStretch();
    topBar->addWidget(impl_->statusLabel);

    auto* selBar = new QHBoxLayout();
    selBar->addWidget(setXBtn);
    selBar->addSpacing(20);
    selBar->addWidget(impl_->yNameEdit);
    selBar->addWidget(impl_->yUnitEdit);
    selBar->addWidget(setYBtn);
    selBar->addSpacing(20);
    selBar->addWidget(useRateBtn);
    selBar->addWidget(clearXBtn);
    selBar->addStretch();

    auto* split = new QSplitter(Qt::Horizontal, this);
    split->addWidget(impl_->preview);
    split->addWidget(impl_->mapping);
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 1);

    auto* root = new QVBoxLayout(this);
    root->addLayout(topBar);
    root->addLayout(selBar);
    root->addWidget(split, /*stretch=*/1);

    auto reparseCurrentFile = [this]{
        if (impl_->currentFile.isEmpty()) return;
        impl_->csv = std::make_unique<CsvSource>(
            std::filesystem::path(impl_->currentFile.toStdString()),
            impl_->mapping->columnDelimiter(),
            impl_->mapping->rowDelimiter());
        impl_->previewModel = impl_->csv->previewModel("file");
        impl_->preview->setModel(impl_->previewModel.get());
        impl_->mapping->setColumns(impl_->currentColumnLabels());
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

    connect(setXBtn, &QPushButton::clicked, this, [this]{
        const auto sel = currentSelection(impl_->preview);
        if (sel.empty()) {
            QMessageBox::information(this, "No selection",
                "Select one or more cells in the X-axis column first.");
            return;
        }
        // Clear any existing X-axis column.
        for (const auto& label : impl_->currentColumnLabels()) {
            // We don't know which was X without re-reading; cheapest is to leave
            // others alone — MappingPanel will overwrite via setColumnMapping
            // for the new X. (The CSV importer only uses the LAST X-time
            // column in profile order.)
            Q_UNUSED(label);
        }
        const QString xLabel = colLabel(sel.columns.front());
        impl_->mapping->setColumnMapping(
            xLabel, ColumnMapping::Role::XTime,
            sel.rowStart, sel.rowEnd,
            QString(), impl_->yUnitEdit->text());
        impl_->mapping->setUseSampleRate(false, 0, "Hz");
    });

    connect(setYBtn, &QPushButton::clicked, this, [this]{
        const auto sel = currentSelection(impl_->preview);
        if (sel.empty()) {
            QMessageBox::information(this, "No selection",
                "Select one or more cells in the column(s) you want as Y signal(s).");
            return;
        }
        const QString baseName = impl_->yNameEdit->text();
        const QString unit     = impl_->yUnitEdit->text();
        for (std::size_t i = 0; i < sel.columns.size(); ++i) {
            const QString label = colLabel(sel.columns[i]);
            const QString name  = baseName.isEmpty()
                                    ? QString("Col%1").arg(label)
                                    : (sel.columns.size() == 1
                                         ? baseName
                                         : QString("%1_%2").arg(baseName, label));
            impl_->mapping->setColumnMapping(
                label, ColumnMapping::Role::Signal,
                sel.rowStart, sel.rowEnd, name, unit);
        }
    });

    connect(useRateBtn, &QPushButton::clicked, this, [this]{
        impl_->mapping->setUseSampleRate(true,
            impl_->mapping->sampleRateHz(),
            impl_->mapping->sampleRateDisplayUnit());
        // Clear any column with role==XTime.
        for (const auto& label : impl_->currentColumnLabels()) {
            // We don't know current role of each; safest is to leave them.
            // The CSV importer ignores XTime columns when useSampleRate is on.
            Q_UNUSED(label);
        }
        impl_->statusLabel->setText(QString("X-axis = sample rate %1 %2 (= %3 Hz)")
            .arg(impl_->mapping->sampleRateHz() > 0
                   ? QString::number(impl_->mapping->sampleRateHz(), 'g', 6)
                   : QString("0"))
            .arg("Hz")
            .arg(impl_->mapping->sampleRateHz(), 0, 'g', 6));
    });

    connect(clearXBtn, &QPushButton::clicked, this, [this]{
        for (const auto& label : impl_->currentColumnLabels())
            impl_->mapping->clearColumnRole(label);
        impl_->mapping->setUseSampleRate(false, 0, "Hz");
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
        const QString path = QFileDialog::getSaveFileName(
            this, "Save profile", QString(), "Scope conversion profile (*.scaconv)");
        if (path.isEmpty()) return;
        const auto profile = impl_->mapping->buildProfile("csv");
        QString err;
        if (!profile.saveToFile(std::filesystem::path(path.toStdString()), &err))
            QMessageBox::critical(this, "Save failed", err);
    });

    connect(impl_->mapping, &ui::MappingPanel::loadProfileRequested, this, [this, reparseCurrentFile]{
        const QString path = QFileDialog::getOpenFileName(
            this, "Load profile", QString(), "Scope conversion profile (*.scaconv)");
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
    });
}

ConverterWidget::~ConverterWidget() = default;

}  // namespace scope::converter
