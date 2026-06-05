#include "SaveChartDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QRadioButton>
#include <QVBoxLayout>

#include <array>

namespace scope::analyser::ui {

namespace {

struct SepOption { const char* label; const char* value; };
const std::array<SepOption, 5> kColSepOptions = {{
    {"Comma  ,",      ","},
    {"Semicolon  ;",  ";"},
    {"Tab  \\t",      "\t"},
    {"Pipe  |",       "|"},
    {"Custom…",       ""},
}};
const std::array<SepOption, 3> kRowSepOptions = {{
    {"LF  \\n",       "\n"},
    {"CRLF  \\r\\n",  "\r\n"},
    {"Custom…",       ""},
}};
constexpr int kCustomColIdx = 4;
constexpr int kCustomRowIdx = 2;

}  // namespace

SaveChartDialog::SaveChartDialog(double currentXMinSec, double currentXMaxSec,
                                 QWidget* parent)
    : QDialog(parent) {
    setWindowTitle("Save chart");
    resize(420, 480);

    // ---- Format ------------------------------------------------------
    h5Radio_  = new QRadioButton("HDF5 (.h5) — lossless recording format", this);
    csvRadio_ = new QRadioButton("CSV (.csv) — text, configurable below", this);
    h5Radio_->setChecked(true);
    auto* formatBox = new QGroupBox("Format", this);
    auto* formatLayout = new QVBoxLayout(formatBox);
    formatLayout->addWidget(h5Radio_);
    formatLayout->addWidget(csvRadio_);

    // ---- Time range --------------------------------------------------
    allRangeRadio_    = new QRadioButton("All data", this);
    customRangeRadio_ = new QRadioButton("Custom range:", this);
    allRangeRadio_->setChecked(true);
    fromSpin_ = new QDoubleSpinBox(this);
    fromSpin_->setRange(-1e12, 1e12);
    fromSpin_->setDecimals(6);
    fromSpin_->setSuffix(" s");
    fromSpin_->setValue(currentXMinSec);
    toSpin_ = new QDoubleSpinBox(this);
    toSpin_->setRange(-1e12, 1e12);
    toSpin_->setDecimals(6);
    toSpin_->setSuffix(" s");
    toSpin_->setValue(currentXMaxSec);

    auto* rangeBox = new QGroupBox("Time range", this);
    auto* rangeLayout = new QVBoxLayout(rangeBox);
    rangeLayout->addWidget(allRangeRadio_);
    auto* customRow = new QHBoxLayout();
    customRow->addWidget(customRangeRadio_);
    customRow->addWidget(new QLabel("from", this));
    customRow->addWidget(fromSpin_);
    customRow->addWidget(new QLabel("to", this));
    customRow->addWidget(toSpin_);
    customRow->addStretch();
    rangeLayout->addLayout(customRow);

    // ---- CSV options (only relevant when CSV selected) --------------
    csvGroup_ = new QGroupBox("CSV options", this);
    headerCheck_ = new QCheckBox("Include header row", csvGroup_);
    headerCheck_->setChecked(true);

    colSepCombo_ = new QComboBox(csvGroup_);
    for (const auto& o : kColSepOptions)
        colSepCombo_->addItem(o.label, QString::fromUtf8(o.value));
    colSepCustom_ = new QLineEdit(csvGroup_);
    colSepCustom_->setMaximumWidth(80);
    colSepCustom_->setPlaceholderText("single char");
    colSepCustom_->setVisible(false);

    rowSepCombo_ = new QComboBox(csvGroup_);
    for (const auto& o : kRowSepOptions)
        rowSepCombo_->addItem(o.label, QString::fromUtf8(o.value));
    rowSepCustom_ = new QLineEdit(csvGroup_);
    rowSepCustom_->setMaximumWidth(120);
    rowSepCustom_->setVisible(false);

    decimalEdit_ = new QLineEdit(".", csvGroup_);
    decimalEdit_->setMaximumWidth(40);

    timeModeCombo_ = new QComboBox(csvGroup_);
    timeModeCombo_->addItem("Shared time column (interpolated)");
    timeModeCombo_->addItem("Per-signal time columns (exact)");

    auto* csvForm = new QFormLayout(csvGroup_);
    csvForm->addRow(headerCheck_);
    auto* colSepRow = new QHBoxLayout();
    colSepRow->addWidget(colSepCombo_); colSepRow->addWidget(colSepCustom_); colSepRow->addStretch();
    auto* colSepWrap = new QWidget(csvGroup_);
    colSepWrap->setLayout(colSepRow);
    csvForm->addRow("Column separator:", colSepWrap);
    auto* rowSepRow = new QHBoxLayout();
    rowSepRow->addWidget(rowSepCombo_); rowSepRow->addWidget(rowSepCustom_); rowSepRow->addStretch();
    auto* rowSepWrap = new QWidget(csvGroup_);
    rowSepWrap->setLayout(rowSepRow);
    csvForm->addRow("Row separator:", rowSepWrap);
    csvForm->addRow("Decimal separator:", decimalEdit_);
    csvForm->addRow("Time mode:", timeModeCombo_);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* root = new QVBoxLayout(this);
    root->addWidget(formatBox);
    root->addWidget(rangeBox);
    root->addWidget(csvGroup_);
    root->addStretch();
    root->addWidget(buttons);

    connect(h5Radio_,  &QRadioButton::toggled, this, [this](bool){ onFormatChanged(); });
    connect(csvRadio_, &QRadioButton::toggled, this, [this](bool){ onFormatChanged(); });
    connect(allRangeRadio_,    &QRadioButton::toggled, this, [this](bool){ onRangeModeChanged(); });
    connect(customRangeRadio_, &QRadioButton::toggled, this, [this](bool){ onRangeModeChanged(); });

    connect(colSepCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int i){ colSepCustom_->setVisible(i == kCustomColIdx); });
    connect(rowSepCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int i){ rowSepCustom_->setVisible(i == kCustomRowIdx); });

    onFormatChanged();
    onRangeModeChanged();
}

void SaveChartDialog::onFormatChanged() {
    csvGroup_->setEnabled(csvRadio_->isChecked());
}

void SaveChartDialog::onRangeModeChanged() {
    const bool custom = customRangeRadio_->isChecked();
    fromSpin_->setEnabled(custom);
    toSpin_->setEnabled(custom);
}

SaveChartDialog::Format SaveChartDialog::format() const {
    return csvRadio_->isChecked() ? Format::Csv : Format::Hdf5;
}

bool   SaveChartDialog::useCustomRange() const { return customRangeRadio_->isChecked(); }
double SaveChartDialog::fromSec()        const { return fromSpin_->value(); }
double SaveChartDialog::toSec()          const { return toSpin_->value(); }

scope::converter::CsvExportOptions SaveChartDialog::csvOptions() const {
    scope::converter::CsvExportOptions o;
    o.includeHeader = headerCheck_->isChecked();
    o.columnDelimiter = (colSepCombo_->currentIndex() == kCustomColIdx)
        ? (colSepCustom_->text().isEmpty() ? QString(",") : colSepCustom_->text())
        : colSepCombo_->currentData().toString();
    o.rowDelimiter = (rowSepCombo_->currentIndex() == kCustomRowIdx)
        ? (rowSepCustom_->text().isEmpty() ? QString("\n") : rowSepCustom_->text())
        : rowSepCombo_->currentData().toString();
    o.decimalSeparator = decimalEdit_->text().isEmpty() ? QString(".") : decimalEdit_->text();
    o.timeMode = (timeModeCombo_->currentIndex() == 1)
        ? scope::converter::CsvExportOptions::TimeMode::PerSignal
        : scope::converter::CsvExportOptions::TimeMode::Shared;
    return o;
}

}  // namespace scope::analyser::ui
