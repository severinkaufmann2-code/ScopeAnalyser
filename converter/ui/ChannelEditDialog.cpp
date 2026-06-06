#include "ChannelEditDialog.h"

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

bool parseRowRange(const QString& text, int* startOut, int* endOut, QString* errOut) {
    const QString s = text.trimmed();
    if (s.isEmpty() || s.compare("all", Qt::CaseInsensitive) == 0) {
        *startOut = -1; *endOut = -1; return true;
    }
    int sep = s.indexOf(':');
    if (sep < 0) sep = s.indexOf('-');
    QString lhs, rhs;
    if (sep < 0) { lhs = s; rhs = s; }
    else {
        lhs = s.left(sep).trimmed();
        rhs = s.mid(sep + 1).trimmed();
    }
    auto parseEnd = [&](const QString& t, int* out) -> bool {
        if (t.isEmpty() || t == "*") { *out = -1; return true; }
        bool ok = false;
        const int n = t.toInt(&ok);
        if (!ok || n < 1) return false;
        *out = n - 1;
        return true;
    };
    if (!parseEnd(lhs, startOut) || !parseEnd(rhs, endOut)) {
        if (errOut) *errOut = QString("Row range '%1' is not a number, '*', 'all', or e.g. '5:100'").arg(s);
        return false;
    }
    if (*startOut >= 0 && *endOut >= 0 && *startOut > *endOut) {
        if (errOut) *errOut = QString("Row range '%1': start must be ≤ end").arg(s);
        return false;
    }
    return true;
}

QString formatRowRange(int start, int end) {
    if (start < 0 && end < 0) return "all";
    const QString lhs = (start < 0) ? QString("*") : QString::number(start + 1);
    const QString rhs = (end   < 0) ? QString("*") : QString::number(end + 1);
    return lhs + ":" + rhs;
}

int indexFromRole(ColumnMapping::Role r) {
    switch (r) {
        case ColumnMapping::Role::XTime:  return 1;
        default: return 0;  // signal (default)
    }
}
ColumnMapping::Role roleFromIndex(int i) {
    return (i == 1) ? ColumnMapping::Role::XTime : ColumnMapping::Role::Signal;
}

struct RateUnit { const char* label; double m; };
const std::array<RateUnit, 7> kRateUnits = {{
    {"s",  -1.0},
    {"ms", -1e-3},   // default
    {"µs", -1e-6},
    {"ns", -1e-9},
    {"Hz",  1.0},
    {"kHz", 1e3},
    {"MHz", 1e6},
}};
constexpr int kDefaultRateUnitIndex = 1;

double rateValueToHz(double value, int unitIndex) {
    if (unitIndex < 0 || unitIndex >= (int)kRateUnits.size() || value == 0) return 0;
    const auto& u = kRateUnits[unitIndex];
    if (u.m > 0) return value * u.m;
    return 1.0 / (value * -u.m);
}

int unitIndexFromName(const QString& name) {
    for (int i = 0; i < (int)kRateUnits.size(); ++i)
        if (name == QString::fromUtf8(kRateUnits[i].label)) return i;
    return kDefaultRateUnitIndex;
}

void setRateFromHz(double hz, const QString& displayUnit,
                   QDoubleSpinBox* valueBox, QComboBox* unitBox) {
    if (hz <= 0) return;
    const int idx = unitIndexFromName(displayUnit);
    unitBox->setCurrentIndex(idx);
    const auto& u = kRateUnits[idx];
    const double v = (u.m > 0) ? hz / u.m : 1.0 / (hz * -u.m);
    valueBox->setValue(v);
}

}  // namespace

ChannelEditDialog::ChannelEditDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("Add channel");

    colEdit_  = new QLineEdit(this);
    colEdit_->setPlaceholderText("e.g. A");
    colEdit_->setMaximumWidth(60);

    rowsEdit_ = new QLineEdit(this);
    rowsEdit_->setText("all");
    rowsEdit_->setPlaceholderText("all  |  5:100  |  5:  |  :100");

    roleCombo_ = new QComboBox(this);
    roleCombo_->addItem("Y signal");
    roleCombo_->addItem("X-axis (time)");
    connect(roleCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int){ onRoleChanged(); });

    nameEdit_ = new QLineEdit(this);
    nameEdit_->setPlaceholderText("Signal name (Y only)");

    unitEdit_ = new QLineEdit(this);
    unitEdit_->setPlaceholderText("Unit (e.g. V, rpm, s)");

    auto* form = new QFormLayout();
    form->addRow("Column:", colEdit_);
    form->addRow("Rows:",   rowsEdit_);
    form->addRow("Role:",   roleCombo_);
    form->addRow("Name:",   nameEdit_);
    form->addRow("Unit:",   unitEdit_);

    // X-source group: only shown for Y signals.
    xSourceGroup_   = new QGroupBox("X-axis source (for this Y signal)", this);
    useColumnRadio_ = new QRadioButton("From X-axis column:", xSourceGroup_);
    useColumnRadio_->setChecked(true);
    xColumnCombo_   = new QComboBox(xSourceGroup_);
    xColumnCombo_->setMinimumWidth(120);
    useRateRadio_   = new QRadioButton("From sample rate / time:", xSourceGroup_);
    rateValue_      = new QDoubleSpinBox(xSourceGroup_);
    rateValue_->setRange(0.0, 1e12);
    rateValue_->setDecimals(6);
    rateValue_->setValue(1.0);
    rateUnit_       = new QComboBox(xSourceGroup_);
    for (const auto& u : kRateUnits) rateUnit_->addItem(QString::fromUtf8(u.label));
    rateUnit_->setCurrentIndex(kDefaultRateUnitIndex);

    resetToZeroCheck_ = new QCheckBox(
        "Reset timestamps so signal starts at 0", xSourceGroup_);
    resetToZeroCheck_->setToolTip(
        "Subtract this channel's first timestamp on import. Only meaningful\n"
        "when X comes from a column (sample-rate mode already starts at 0).");

    offsetSpin_ = new QDoubleSpinBox(xSourceGroup_);
    offsetSpin_->setRange(-1e9, 1e9);
    offsetSpin_->setDecimals(6);
    offsetSpin_->setSuffix(" s");
    offsetSpin_->setValue(0.0);
    offsetSpin_->setToolTip(
        "Constant offset added to every timestamp after the reset step.\n"
        "Reset + offset = 5 puts the first sample at exactly t = 5 s.");

    auto* xColRow = new QHBoxLayout();
    xColRow->addWidget(useColumnRadio_);
    xColRow->addWidget(xColumnCombo_);
    xColRow->addStretch();
    auto* xRateRow = new QHBoxLayout();
    xRateRow->addWidget(useRateRadio_);
    xRateRow->addWidget(rateValue_);
    xRateRow->addWidget(rateUnit_);
    xRateRow->addStretch();
    auto* offsetRow = new QHBoxLayout();
    offsetRow->addWidget(new QLabel("Time offset:", xSourceGroup_));
    offsetRow->addWidget(offsetSpin_);
    offsetRow->addStretch();

    collapseCombo_ = new QComboBox(xSourceGroup_);
    collapseCombo_->addItem("Keep all (no collapse)",  static_cast<int>(ColumnMapping::CollapseMode::None));
    collapseCombo_->addItem("Collapse → first value",  static_cast<int>(ColumnMapping::CollapseMode::First));
    collapseCombo_->addItem("Collapse → last value",   static_cast<int>(ColumnMapping::CollapseMode::Last));
    collapseCombo_->addItem("Collapse → mean of run",  static_cast<int>(ColumnMapping::CollapseMode::Mean));
    collapseCombo_->setToolTip(
        "If several CSV rows produce the same X timestamp for this\n"
        "signal, what to do:\n"
        "  Keep all       — round-trip every row (current default).\n"
        "  Collapse first — keep only the first Y of each run.\n"
        "  Collapse last  — keep only the last Y of each run.\n"
        "  Collapse mean  — replace each run with its arithmetic mean.");
    auto* collapseRow = new QHBoxLayout();
    collapseRow->addWidget(new QLabel("Duplicate timestamps:", xSourceGroup_));
    collapseRow->addWidget(collapseCombo_, /*stretch=*/1);

    auto* xLayout = new QVBoxLayout(xSourceGroup_);
    xLayout->addLayout(xColRow);
    xLayout->addLayout(xRateRow);
    xLayout->addWidget(resetToZeroCheck_);
    xLayout->addLayout(offsetRow);
    xLayout->addLayout(collapseRow);

    connect(useColumnRadio_, &QRadioButton::toggled,
            this, [this](bool){ onXSourceModeChanged(); });
    connect(useRateRadio_, &QRadioButton::toggled,
            this, [this](bool){ onXSourceModeChanged(); });

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(xSourceGroup_);
    layout->addWidget(buttons);

    onRoleChanged();
    onXSourceModeChanged();
}

void ChannelEditDialog::setAvailableXColumns(const QStringList& labels) {
    availableXColumns_ = labels;
    rebuildXSourceCombo();
}

void ChannelEditDialog::rebuildXSourceCombo() {
    const QString prev = xColumnCombo_->currentText();
    xColumnCombo_->clear();
    if (availableXColumns_.isEmpty()) {
        xColumnCombo_->addItem("(no X column defined yet)");
        xColumnCombo_->setEnabled(false);
        useColumnRadio_->setEnabled(false);
        useRateRadio_->setChecked(true);
    } else {
        for (const auto& l : availableXColumns_) xColumnCombo_->addItem(l);
        xColumnCombo_->setEnabled(true);
        useColumnRadio_->setEnabled(true);
        const int idx = availableXColumns_.indexOf(prev);
        if (idx >= 0) xColumnCombo_->setCurrentIndex(idx);
    }
}

void ChannelEditDialog::onRoleChanged() {
    const bool isSignal = (roleCombo_->currentIndex() == 0);
    nameEdit_->setEnabled(isSignal);
    xSourceGroup_->setVisible(isSignal);
    adjustSize();
}

void ChannelEditDialog::onXSourceModeChanged() {
    const bool col = useColumnRadio_->isChecked();
    xColumnCombo_->setEnabled(col && !availableXColumns_.isEmpty());
    rateValue_->setEnabled(!col);
    rateUnit_->setEnabled(!col);
    // Reset is only meaningful in column-X mode (rate mode already starts at 0).
    if (resetToZeroCheck_) resetToZeroCheck_->setEnabled(col);
}

void ChannelEditDialog::setMapping(const ColumnMapping& m) {
    setWindowTitle("Edit channel");
    colEdit_->setText(m.columnId);
    rowsEdit_->setText(formatRowRange(m.rowStart, m.rowEnd));
    roleCombo_->setCurrentIndex(indexFromRole(m.role));
    nameEdit_->setText(m.signalName);
    unitEdit_->setText(m.unit);

    if (m.useSampleRate || (m.xSourceColumn.isEmpty() && m.sampleRateHz > 0)) {
        useRateRadio_->setChecked(true);
        setRateFromHz(m.sampleRateHz, m.sampleRateDisplayUnit, rateValue_, rateUnit_);
    } else {
        useColumnRadio_->setChecked(true);
        const int idx = availableXColumns_.indexOf(m.xSourceColumn);
        if (idx >= 0) xColumnCombo_->setCurrentIndex(idx);
    }
    if (resetToZeroCheck_) resetToZeroCheck_->setChecked(m.resetTimeToZero);
    if (offsetSpin_)       offsetSpin_->setValue(m.timeOffsetSec);
    if (collapseCombo_) {
        for (int i = 0; i < collapseCombo_->count(); ++i) {
            if (collapseCombo_->itemData(i).toInt() == static_cast<int>(m.collapseDuplicates)) {
                collapseCombo_->setCurrentIndex(i);
                break;
            }
        }
    }
    onRoleChanged();
    onXSourceModeChanged();
}

bool ChannelEditDialog::getMapping(ColumnMapping* out, QString* errorOut) const {
    if (!out) return false;
    const QString col = colEdit_->text().trimmed().toUpper();
    if (col.isEmpty()) {
        if (errorOut) *errorOut = "Column letter is required (e.g. A, B, ...)";
        return false;
    }
    for (QChar c : col) {
        if (!c.isLetter()) {
            if (errorOut) *errorOut = "Column must be letters only (A, B, ..., AA, AB, ...)";
            return false;
        }
    }
    int startRow = -1, endRow = -1;
    if (!parseRowRange(rowsEdit_->text(), &startRow, &endRow, errorOut)) return false;

    out->columnId   = col;
    out->role       = roleFromIndex(roleCombo_->currentIndex());
    out->signalName = nameEdit_->text().trimmed();
    out->unit       = unitEdit_->text().trimmed();
    out->rowStart   = startRow;
    out->rowEnd     = endRow;

    if (out->role == ColumnMapping::Role::Signal) {
        if (useRateRadio_->isChecked()) {
            out->useSampleRate         = true;
            out->sampleRateHz          = rateValueToHz(rateValue_->value(),
                                                       rateUnit_->currentIndex());
            out->sampleRateDisplayUnit = rateUnit_->currentText();
            out->xSourceColumn.clear();
        } else {
            out->useSampleRate = false;
            out->xSourceColumn = xColumnCombo_->isEnabled()
                                 ? xColumnCombo_->currentText() : QString();
        }
        out->resetTimeToZero = resetToZeroCheck_
            ? (resetToZeroCheck_->isEnabled() && resetToZeroCheck_->isChecked())
            : false;
        out->timeOffsetSec = offsetSpin_ ? offsetSpin_->value() : 0.0;
        out->collapseDuplicates = collapseCombo_
            ? static_cast<ColumnMapping::CollapseMode>(collapseCombo_->currentData().toInt())
            : ColumnMapping::CollapseMode::None;
    } else {
        // X-axis channel: not a Y, no X-source needed.
        out->useSampleRate = false;
        out->xSourceColumn.clear();
        out->sampleRateHz  = 0;
        out->resetTimeToZero = false;
        out->timeOffsetSec   = 0.0;
        out->collapseDuplicates = ColumnMapping::CollapseMode::None;
    }
    return true;
}

}  // namespace scope::converter::ui
