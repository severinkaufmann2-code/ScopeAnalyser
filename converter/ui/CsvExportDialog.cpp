#include "CsvExportDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QVBoxLayout>

#include <array>

namespace scope::converter::ui {

namespace {

constexpr int kCustomColIdx = 4;
constexpr int kCustomRowIdx = 2;

struct SepOption { const char* label; const char* value; };
const std::array<SepOption, 5> kColSepOptions = {{
    {"Comma  ,",      ","},
    {"Semicolon  ;",  ";"},
    {"Tab  \\t",      "\t"},
    {"Pipe  |",       "|"},
    {"Custom...",     ""}
}};
const std::array<SepOption, 3> kRowSepOptions = {{
    {"Newline (LF)",  "\n"},
    {"CRLF (\\r\\n)", "\r\n"},
    {"Custom...",     ""}
}};

}  // namespace

CsvExportDialog::CsvExportDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("Save CSV — options");

    headerCheck_ = new QCheckBox("Include header row", this);
    headerCheck_->setChecked(true);

    metadataCheck_ = new QCheckBox(
        "Include scope metadata header (# scope-csv: …)", this);
    metadataCheck_->setChecked(true);
    metadataCheck_->setToolTip(
        "Embed a commented JSON line on top of the file describing each\n"
        "column's role, unit and signal name. Lets the loader round-trip\n"
        "the file losslessly (frequency-domain channels go back into the\n"
        "Frequency view, etc.). Most consumers skip '#' lines; Excel does\n"
        "not.");

    colSepCombo_ = new QComboBox(this);
    for (const auto& opt : kColSepOptions)
        colSepCombo_->addItem(opt.label, QString::fromUtf8(opt.value));
    colSepCustom_ = new QLineEdit(this);
    colSepCustom_->setMaximumWidth(80);
    colSepCustom_->setPlaceholderText("single char");
    colSepCustom_->setVisible(false);

    rowSepCombo_ = new QComboBox(this);
    for (const auto& opt : kRowSepOptions)
        rowSepCombo_->addItem(opt.label, QString::fromUtf8(opt.value));
    rowSepCustom_ = new QLineEdit(this);
    rowSepCustom_->setMaximumWidth(120);
    rowSepCustom_->setVisible(false);

    decimalEdit_ = new QLineEdit(".", this);
    decimalEdit_->setMaximumWidth(40);

    timeModeCombo_ = new QComboBox(this);
    timeModeCombo_->addItem("Shared time column (resampled)",
                            static_cast<int>(CsvExportOptions::TimeMode::Shared));
    timeModeCombo_->addItem("Per-signal time (no interpolation)",
                            static_cast<int>(CsvExportOptions::TimeMode::PerSignal));

    auto* form = new QFormLayout();
    form->addRow(headerCheck_);
    form->addRow(metadataCheck_);

    auto* colSepRow = new QHBoxLayout();
    colSepRow->addWidget(colSepCombo_);
    colSepRow->addWidget(colSepCustom_);
    colSepRow->addStretch();
    form->addRow("Column separator:", colSepRow);

    auto* rowSepRow = new QHBoxLayout();
    rowSepRow->addWidget(rowSepCombo_);
    rowSepRow->addWidget(rowSepCustom_);
    rowSepRow->addStretch();
    form->addRow("Row separator:", rowSepRow);

    form->addRow("Decimal separator:", decimalEdit_);
    form->addRow("Time column:", timeModeCombo_);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* root = new QVBoxLayout(this);
    root->addLayout(form);
    root->addWidget(buttons);

    auto sync = [this]{
        colSepCustom_->setVisible(colSepCombo_->currentIndex() == kCustomColIdx);
        rowSepCustom_->setVisible(rowSepCombo_->currentIndex() == kCustomRowIdx);
    };
    sync();
    connect(colSepCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [sync](int){ sync(); });
    connect(rowSepCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [sync](int){ sync(); });
}

CsvExportOptions CsvExportDialog::options() const {
    CsvExportOptions o;
    o.includeHeader   = headerCheck_->isChecked();
    o.includeMetadata = metadataCheck_->isChecked();

    o.columnDelimiter = (colSepCombo_->currentIndex() == kCustomColIdx)
        ? (colSepCustom_->text().isEmpty() ? QString(",") : colSepCustom_->text())
        : colSepCombo_->currentData().toString();
    o.rowDelimiter = (rowSepCombo_->currentIndex() == kCustomRowIdx)
        ? (rowSepCustom_->text().isEmpty() ? QString("\n") : rowSepCustom_->text())
        : rowSepCombo_->currentData().toString();

    o.decimalSeparator = decimalEdit_->text().isEmpty()
        ? QString(".") : decimalEdit_->text();
    o.timeMode = static_cast<CsvExportOptions::TimeMode>(
        timeModeCombo_->currentData().toInt());
    return o;
}

}  // namespace scope::converter::ui
