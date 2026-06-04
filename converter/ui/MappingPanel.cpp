#include "MappingPanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include <array>

namespace scope::converter::ui {

namespace {
enum Col { ColLabel = 0, ColRole, ColName, ColUnit, ColRows, ColCount };
const char* kRoles[] = {"Ignore", "X-axis (time)", "Signal"};

ColumnMapping::Role roleFromIndex(int i) {
    switch (i) {
        case 1: return ColumnMapping::Role::XTime;
        case 2: return ColumnMapping::Role::Signal;
        default: return ColumnMapping::Role::Ignore;
    }
}
int indexFromRole(ColumnMapping::Role r) {
    switch (r) {
        case ColumnMapping::Role::XTime:  return 1;
        case ColumnMapping::Role::Signal: return 2;
        default: return 0;
    }
}

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
    {"Newline (LF or CRLF)", "\n"},
    {"CRLF (\\r\\n)",        "\r\n"},
    {"Custom...",            ""}
}};

struct RateUnit { const char* label; double toHz; };
// toHz is the factor to multiply the spin value by to get Hz. For "period"
// units (s, ms, us, ns) it's negative — we use the absolute value as a marker
// and invert in computeHz().
const std::array<RateUnit, 7> kRateUnits = {{
    {"Hz",  1.0},
    {"kHz", 1e3},
    {"MHz", 1e6},
    {"s",  -1.0},     // 1 / value Hz
    {"ms", -1e-3},
    {"µs", -1e-6},
    {"ns", -1e-9},
}};

double rateValueToHz(double value, int unitIndex) {
    if (unitIndex < 0 || unitIndex >= (int)kRateUnits.size() || value == 0) return 0;
    const auto& u = kRateUnits[unitIndex];
    if (u.toHz > 0) return value * u.toHz;
    return 1.0 / (value * -u.toHz);  // period → frequency
}

int unitIndexFromName(const QString& name) {
    for (int i = 0; i < (int)kRateUnits.size(); ++i)
        if (name == QString::fromUtf8(kRateUnits[i].label)) return i;
    return 0;
}

QString rangeLabel(int lo, int hi) {
    if (lo < 0 && hi < 0) return "all";
    QString s = (lo < 0) ? "*" : QString::number(lo + 1);   // 1-based for display
    s += "-";
    s += (hi < 0) ? "*" : QString::number(hi + 1);
    return s;
}

}  // namespace

MappingPanel::MappingPanel(QWidget* parent) : QWidget(parent) {
    table_ = new QTableWidget(0, ColCount, this);
    table_->setHorizontalHeaderLabels({"Col", "Role", "Signal name", "Unit", "Rows"});
    table_->horizontalHeader()->setSectionResizeMode(ColName, QHeaderView::Stretch);
    table_->verticalHeader()->setVisible(false);

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
    rowSepCustom_->setPlaceholderText("e.g. ;");
    rowSepCustom_->setVisible(false);

    headerSpin_  = new QSpinBox(this);
    headerSpin_->setRange(0, 1'000'000);
    headerSpin_->setValue(1);
    decimalEdit_ = new QLineEdit(".", this);
    decimalEdit_->setMaximumWidth(40);

    useSampleRateCheck_ = new QCheckBox("X-axis from sample rate", this);
    sampleRateValue_    = new QDoubleSpinBox(this);
    sampleRateValue_->setRange(0.0, 1e12);
    sampleRateValue_->setDecimals(6);
    sampleRateValue_->setValue(1000.0);
    sampleRateUnit_     = new QComboBox(this);
    for (const auto& u : kRateUnits) sampleRateUnit_->addItem(QString::fromUtf8(u.label));

    applyBtn_ = new QPushButton("Apply (import signals)", this);
    saveBtn_  = new QPushButton("Save profile…", this);
    loadBtn_  = new QPushButton("Load profile…", this);

    auto* opts = new QGridLayout();
    int row = 0;
    opts->addWidget(new QLabel("Column separator:", this), row, 0);
    auto* colSepRow = new QHBoxLayout();
    colSepRow->addWidget(colSepCombo_);
    colSepRow->addWidget(colSepCustom_);
    colSepRow->addStretch();
    opts->addLayout(colSepRow, row++, 1);

    opts->addWidget(new QLabel("Row separator:", this), row, 0);
    auto* rowSepRow = new QHBoxLayout();
    rowSepRow->addWidget(rowSepCombo_);
    rowSepRow->addWidget(rowSepCustom_);
    rowSepRow->addStretch();
    opts->addLayout(rowSepRow, row++, 1);

    opts->addWidget(new QLabel("Header row (1-based):", this), row, 0);
    opts->addWidget(headerSpin_, row++, 1);

    opts->addWidget(new QLabel("Decimal separator:", this), row, 0);
    opts->addWidget(decimalEdit_, row++, 1);

    opts->addWidget(useSampleRateCheck_, row, 0);
    auto* rateRow = new QHBoxLayout();
    rateRow->addWidget(sampleRateValue_);
    rateRow->addWidget(sampleRateUnit_);
    rateRow->addStretch();
    opts->addLayout(rateRow, row++, 1);

    auto* btnRow = new QHBoxLayout();
    btnRow->addWidget(applyBtn_);
    btnRow->addWidget(saveBtn_);
    btnRow->addWidget(loadBtn_);

    auto* root = new QVBoxLayout(this);
    root->addLayout(opts);
    root->addWidget(table_, /*stretch=*/1);
    root->addLayout(btnRow);

    auto syncCustomVisibility = [this]{
        colSepCustom_->setVisible(colSepCombo_->currentIndex() == kCustomColIdx);
        rowSepCustom_->setVisible(rowSepCombo_->currentIndex() == kCustomRowIdx);
    };
    syncCustomVisibility();

    connect(applyBtn_, &QPushButton::clicked, this, &MappingPanel::applyRequested);
    connect(saveBtn_,  &QPushButton::clicked, this, &MappingPanel::saveProfileRequested);
    connect(loadBtn_,  &QPushButton::clicked, this, &MappingPanel::loadProfileRequested);

    auto emitChange = [this, syncCustomVisibility]{
        syncCustomVisibility();
        emit parseOptionsChanged();
    };
    connect(colSepCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [emitChange](int){ emitChange(); });
    connect(rowSepCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [emitChange](int){ emitChange(); });
    connect(colSepCustom_, &QLineEdit::editingFinished, this, [emitChange]{ emitChange(); });
    connect(rowSepCustom_, &QLineEdit::editingFinished, this, [emitChange]{ emitChange(); });
    connect(headerSpin_, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [emitChange](int){ emitChange(); });
    connect(decimalEdit_, &QLineEdit::editingFinished, this, [emitChange]{ emitChange(); });
}

void MappingPanel::setColumns(const QStringList& columnLabels) {
    table_->setRowCount(0);
    for (const auto& label : columnLabels) {
        const int row = table_->rowCount();
        table_->insertRow(row);

        auto* labelItem = new QTableWidgetItem(label);
        labelItem->setFlags(labelItem->flags() & ~Qt::ItemIsEditable);
        table_->setItem(row, ColLabel, labelItem);

        auto* roleCombo = new QComboBox(table_);
        for (const char* r : kRoles) roleCombo->addItem(r);
        table_->setCellWidget(row, ColRole, roleCombo);

        table_->setItem(row, ColName, new QTableWidgetItem(""));
        table_->setItem(row, ColUnit, new QTableWidgetItem(""));
        auto* rowsItem = new QTableWidgetItem("all");
        rowsItem->setFlags(rowsItem->flags() & ~Qt::ItemIsEditable);
        rowsItem->setData(Qt::UserRole,     -1);  // rowStart
        rowsItem->setData(Qt::UserRole + 1, -1);  // rowEnd
        table_->setItem(row, ColRows, rowsItem);
    }
}

int MappingPanel::rowIndexFor(const QString& columnLabel) const {
    for (int r = 0; r < table_->rowCount(); ++r) {
        if (table_->item(r, ColLabel) && table_->item(r, ColLabel)->text() == columnLabel)
            return r;
    }
    return -1;
}

void MappingPanel::setColumnMapping(const QString& columnLabel,
                                    ColumnMapping::Role role,
                                    int rowStart, int rowEnd,
                                    const QString& name,
                                    const QString& unit) {
    const int r = rowIndexFor(columnLabel);
    if (r < 0) return;
    if (auto* combo = qobject_cast<QComboBox*>(table_->cellWidget(r, ColRole)))
        combo->setCurrentIndex(indexFromRole(role));
    if (!name.isEmpty()) table_->item(r, ColName)->setText(name);
    if (!unit.isEmpty()) table_->item(r, ColUnit)->setText(unit);
    auto* rowsItem = table_->item(r, ColRows);
    rowsItem->setData(Qt::UserRole, rowStart);
    rowsItem->setData(Qt::UserRole + 1, rowEnd);
    rowsItem->setText(rangeLabel(rowStart, rowEnd));
}

void MappingPanel::clearColumnRole(const QString& columnLabel) {
    setColumnMapping(columnLabel, ColumnMapping::Role::Ignore, -1, -1);
}

void MappingPanel::setUseSampleRate(bool use, double hz, const QString& displayUnit) {
    useSampleRateCheck_->setChecked(use);
    if (hz > 0) {
        // Round-trip display: pick the unit, then derive the spin value.
        const int idx = unitIndexFromName(displayUnit);
        sampleRateUnit_->setCurrentIndex(idx);
        const auto& u = kRateUnits[idx];
        const double v = (u.toHz > 0) ? hz / u.toHz : 1.0 / (hz * -u.toHz);
        sampleRateValue_->setValue(v);
    }
}

void MappingPanel::setProfile(const ConverterProfile& p) {
    auto setComboFromValue = [](QComboBox* c, const QString& v, int customIdx,
                                QLineEdit* custom) {
        for (int i = 0; i < c->count(); ++i) {
            if (i != customIdx && c->itemData(i).toString() == v) {
                c->setCurrentIndex(i);
                custom->setVisible(false);
                custom->clear();
                return;
            }
        }
        c->setCurrentIndex(customIdx);
        custom->setVisible(true);
        custom->setText(v);
    };
    setComboFromValue(colSepCombo_, p.columnDelimiter, kCustomColIdx, colSepCustom_);
    setComboFromValue(rowSepCombo_, p.rowDelimiter,    kCustomRowIdx, rowSepCustom_);
    headerSpin_->setValue(p.headerRow);
    decimalEdit_->setText(p.decimalSeparator);
    setUseSampleRate(p.useSampleRate, p.sampleRateHz, p.sampleRateDisplayUnit);

    for (const auto& c : p.columns) {
        setColumnMapping(c.columnId, c.role, c.rowStart, c.rowEnd,
                         c.signalName, c.unit);
    }
}

ConverterProfile MappingPanel::buildProfile(const QString& sourceType) const {
    ConverterProfile p;
    p.sourceType = sourceType;
    p.headerRow = headerSpin_->value();
    p.decimalSeparator = decimalEdit_->text().isEmpty() ? QString(".") : decimalEdit_->text();
    p.columnDelimiter = columnDelimiter();
    p.rowDelimiter    = rowDelimiter();
    p.useSampleRate   = useSampleRate();
    p.sampleRateHz    = sampleRateHz();
    p.sampleRateDisplayUnit = sampleRateDisplayUnit();
    for (int r = 0; r < table_->rowCount(); ++r) {
        ColumnMapping cm;
        cm.columnId = table_->item(r, ColLabel)->text();
        if (auto* combo = qobject_cast<QComboBox*>(table_->cellWidget(r, ColRole)))
            cm.role = roleFromIndex(combo->currentIndex());
        cm.signalName = table_->item(r, ColName)->text();
        cm.unit       = table_->item(r, ColUnit)->text();
        auto* rowsItem = table_->item(r, ColRows);
        cm.rowStart = rowsItem ? rowsItem->data(Qt::UserRole).toInt()     : -1;
        cm.rowEnd   = rowsItem ? rowsItem->data(Qt::UserRole + 1).toInt() : -1;
        p.columns.push_back(std::move(cm));
    }
    return p;
}

QString MappingPanel::columnDelimiter() const {
    if (colSepCombo_->currentIndex() == kCustomColIdx) {
        return colSepCustom_->text().isEmpty() ? QString(",") : colSepCustom_->text();
    }
    return colSepCombo_->currentData().toString();
}

QString MappingPanel::rowDelimiter() const {
    if (rowSepCombo_->currentIndex() == kCustomRowIdx) {
        return rowSepCustom_->text().isEmpty() ? QString("\n") : rowSepCustom_->text();
    }
    return rowSepCombo_->currentData().toString();
}

int MappingPanel::headerRow() const { return headerSpin_->value(); }
QString MappingPanel::decimal() const { return decimalEdit_->text(); }
bool MappingPanel::useSampleRate() const { return useSampleRateCheck_->isChecked(); }
double MappingPanel::sampleRateHz() const {
    return rateValueToHz(sampleRateValue_->value(), sampleRateUnit_->currentIndex());
}
QString MappingPanel::sampleRateDisplayUnit() const {
    return sampleRateUnit_->currentText();
}

}  // namespace scope::converter::ui
