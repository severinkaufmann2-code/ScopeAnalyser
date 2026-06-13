#include "scope/converter/SaveChartDialog.h"

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

namespace scope::converter::ui {

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
                                 QWidget* parent, bool offerMetadata)
    : QDialog(parent) {
    setWindowTitle("Save chart");
    resize(420, 480);

    // ---- Format ------------------------------------------------------
    h5Radio_  = new QRadioButton("HDF5 (.h5) — lossless recording format", this);
    mf4Radio_ = new QRadioButton("MDF4 (.mf4) — ASAM standard, opens in DiaDem / asammdf / MATLAB", this);
    csvRadio_ = new QRadioButton("CSV (.csv) — text, configurable below", this);
    htmlRadio_ = new QRadioButton(
        "HTML (.html) — interactive chart, opens in any browser (offline) and "
        "re-opens in ScopeAnalyser", this);
    h5Radio_->setChecked(true);
    auto* formatBox = new QGroupBox("Format", this);
    auto* formatLayout = new QVBoxLayout(formatBox);
    formatLayout->addWidget(h5Radio_);
    formatLayout->addWidget(mf4Radio_);
    formatLayout->addWidget(csvRadio_);
    formatLayout->addWidget(htmlRadio_);

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

    metadataCheck_ = new QCheckBox(
        "Include scope metadata header (# scope-csv: …)", csvGroup_);
    metadataCheck_->setChecked(true);
    metadataCheck_->setToolTip(
        "Embed a commented JSON line on top of the file describing each\n"
        "column's role (X-time / X-frequency / signal), unit and name.\n"
        "Lets the loader round-trip the file losslessly — frequency-domain\n"
        "channels go back into the Frequency view, etc. Most CSV consumers\n"
        "skip '#' comment lines; Excel does not.");

    auto* csvForm = new QFormLayout(csvGroup_);
    csvForm->addRow(headerCheck_);
    csvForm->addRow(metadataCheck_);
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

    // ---- Channel filters ---------------------------------------------
    filtersGroup_ = new QGroupBox("Channels to include", this);
    auto* filtersBox = filtersGroup_;
    includeTimeCheck_     = new QCheckBox("Time-domain channels", filtersBox);
    includeFreqCheck_     = new QCheckBox("Frequency-domain channels", filtersBox);
    includeDerivedCheck_  = new QCheckBox("Derived (formula) channels", filtersBox);
    splitFilesCheck_      = new QCheckBox(
        "Split time and frequency into separate files", filtersBox);
    includeTimeCheck_->setChecked(true);
    includeFreqCheck_->setChecked(true);
    includeDerivedCheck_->setChecked(true);
    splitFilesCheck_->setChecked(false);   // default off — single file
    splitFilesCheck_->setToolTip(
        "When on, time-domain channels go to <name>_time.<ext> and\n"
        "frequency-domain to <name>_frequency.<ext>. When off, both\n"
        "domains are written into one file. For CSV with the Shared\n"
        "Time mode, a mixed-domain single file gets one shared 't [s]'\n"
        "column plus one shared 'f [Hz]' column written side-by-side.");
    auto* filtersLayout = new QVBoxLayout(filtersBox);
    filtersLayout->addWidget(includeTimeCheck_);
    filtersLayout->addWidget(includeFreqCheck_);
    filtersLayout->addWidget(includeDerivedCheck_);
    filtersLayout->addWidget(splitFilesCheck_);

    // ---- Metadata (Analyser only) ------------------------------------
    // What gets embedded in the saved file so it can be re-opened: the math
    // formulas and/or the plot layout. Nested so a child is only choosable
    // once its parent is on.
    metadataGroup_ = new QGroupBox("Metadata", this);
    addMetadataCheck_ = new QCheckBox("Add metadata", metadataGroup_);
    addMetadataCheck_->setChecked(true);
    addMetadataCheck_->setToolTip(
        "Embed metadata so the file re-opens in ScopeAnalyser with its math\n"
        "channels and layout. Off → a plain data file (no layout / formulas).");
    mathFormulaCheck_ = new QCheckBox("Math formula", metadataGroup_);
    mathFormulaCheck_->setChecked(true);
    mathFormulaCheck_->setToolTip(
        "Embed each derived channel's formula so it can be edited / recomputed\n"
        "after re-opening. Off → math channels re-open as plain signals.");
    noMathDataCheck_ = new QCheckBox(
        "No data for math channels", metadataGroup_);
    noMathDataCheck_->setChecked(false);
    noMathDataCheck_->setToolTip(
        "Don't write the computed samples of math channels — keep only the\n"
        "formula. They're recomputed from the sources on re-open (smaller\n"
        "file; needs the sources present and 'Import formula' on re-open).");
    layoutCheck_ = new QCheckBox("Layout", metadataGroup_);
    layoutCheck_->setChecked(true);
    layoutCheck_->setToolTip(
        "Embed the Y axes, channel→axis assignments and view mode so the\n"
        "chart re-opens looking the same.");

    auto* metaLayout = new QVBoxLayout(metadataGroup_);
    metaLayout->addWidget(addMetadataCheck_);
    auto indent = [&](QCheckBox* cb, int px) {
        auto* row = new QHBoxLayout();
        row->addSpacing(px);
        row->addWidget(cb);
        row->addStretch();
        metaLayout->addLayout(row);
    };
    indent(mathFormulaCheck_, 20);
    indent(noMathDataCheck_, 40);
    indent(layoutCheck_, 20);
    metadataGroup_->setVisible(offerMetadata);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* root = new QVBoxLayout(this);
    root->addWidget(formatBox);
    root->addWidget(rangeBox);
    root->addWidget(filtersBox);
    root->addWidget(metadataGroup_);
    root->addWidget(csvGroup_);
    root->addStretch();
    root->addWidget(buttons);

    connect(h5Radio_,   &QRadioButton::toggled, this, [this](bool){ onFormatChanged(); });
    connect(mf4Radio_,  &QRadioButton::toggled, this, [this](bool){ onFormatChanged(); });
    connect(csvRadio_,  &QRadioButton::toggled, this, [this](bool){ onFormatChanged(); });
    connect(htmlRadio_, &QRadioButton::toggled, this, [this](bool){ onFormatChanged(); });
    connect(allRangeRadio_,    &QRadioButton::toggled, this, [this](bool){ onRangeModeChanged(); });
    connect(customRangeRadio_, &QRadioButton::toggled, this, [this](bool){ onRangeModeChanged(); });

    connect(colSepCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int i){ colSepCustom_->setVisible(i == kCustomColIdx); });
    connect(rowSepCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int i){ rowSepCustom_->setVisible(i == kCustomRowIdx); });

    connect(addMetadataCheck_, &QCheckBox::toggled, this, [this](bool){ onMetadataChanged(); });
    connect(mathFormulaCheck_, &QCheckBox::toggled, this, [this](bool){ onMetadataChanged(); });

    onFormatChanged();
    onRangeModeChanged();
    onMetadataChanged();
}

void SaveChartDialog::onMetadataChanged() {
    const bool meta    = addMetadataCheck_->isChecked();
    const bool formula = meta && mathFormulaCheck_->isChecked();
    mathFormulaCheck_->setEnabled(meta);
    layoutCheck_->setEnabled(meta);
    noMathDataCheck_->setEnabled(formula);
}

bool SaveChartDialog::addMetadata() const {
    return addMetadataCheck_->isChecked();
}
bool SaveChartDialog::includeMathFormula() const {
    return addMetadata() && mathFormulaCheck_->isChecked();
}
bool SaveChartDialog::noDataForMathChannels() const {
    return includeMathFormula() && noMathDataCheck_->isChecked();
}
bool SaveChartDialog::includeLayout() const {
    return addMetadata() && layoutCheck_->isChecked();
}

void SaveChartDialog::setChannelFiltersVisible(bool on) {
    filtersGroup_->setVisible(on);
}

void SaveChartDialog::setMetadataLayoutOnly(bool layoutOnly) {
    if (!layoutOnly) return;
    // Keep the master checked (hidden) so includeLayout() stays honoured,
    // then hide everything in the group except the Layout checkbox.
    addMetadataCheck_->setChecked(true);
    addMetadataCheck_->setVisible(false);
    mathFormulaCheck_->setVisible(false);
    noMathDataCheck_->setVisible(false);
}

void SaveChartDialog::onFormatChanged() {
    csvGroup_->setEnabled(csvRadio_->isChecked());
    // HTML is always a single file with its own Time / Frequency / XY view
    // selector, so the split-files option doesn't apply.
    const bool html = htmlRadio_->isChecked();
    splitFilesCheck_->setEnabled(!html);
    if (html) splitFilesCheck_->setChecked(false);
}

void SaveChartDialog::onRangeModeChanged() {
    const bool custom = customRangeRadio_->isChecked();
    fromSpin_->setEnabled(custom);
    toSpin_->setEnabled(custom);
}

SaveChartDialog::Format SaveChartDialog::format() const {
    if (csvRadio_->isChecked())  return Format::Csv;
    if (mf4Radio_->isChecked())  return Format::Mdf4;
    if (htmlRadio_->isChecked()) return Format::Html;
    return Format::Hdf5;
}

bool   SaveChartDialog::useCustomRange() const { return customRangeRadio_->isChecked(); }
double SaveChartDialog::fromSec()        const { return fromSpin_->value(); }
double SaveChartDialog::toSec()          const { return toSpin_->value(); }

bool SaveChartDialog::includeTimeDomain()        const { return includeTimeCheck_->isChecked(); }
bool SaveChartDialog::includeFrequencyDomain()   const { return includeFreqCheck_->isChecked(); }
bool SaveChartDialog::includeDerivedChannels()   const { return includeDerivedCheck_->isChecked(); }
bool SaveChartDialog::splitDomainsIntoTwoFiles() const { return splitFilesCheck_->isChecked(); }

scope::converter::CsvExportOptions SaveChartDialog::csvOptions() const {
    scope::converter::CsvExportOptions o;
    o.includeHeader = headerCheck_->isChecked();
    o.includeMetadata = metadataCheck_->isChecked();
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

}  // namespace scope::converter::ui
