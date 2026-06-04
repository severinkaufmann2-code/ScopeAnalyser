#include "MappingPanel.h"
#include "ChannelEditDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include <array>

namespace scope::converter::ui {

namespace {

enum Col { ColCol = 0, ColRows, ColRole, ColName, ColUnit, ColCount };

const char* roleName(ColumnMapping::Role r) {
    switch (r) {
        case ColumnMapping::Role::Ignore: return "Ignore";
        case ColumnMapping::Role::XTime:  return "X-axis";
        case ColumnMapping::Role::Signal: return "Y signal";
    }
    return "?";
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

// "Hz" units have a positive multiplier (value × m = Hz). "Period" units
// (s, ms, µs, ns) have a negative multiplier: Hz = 1 / (value × |m|).
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
constexpr int kDefaultRateUnitIndex = 1;  // "ms"

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

QString formatRowRange(int start, int end) {
    if (start < 0 && end < 0) return "all";
    const QString lhs = (start < 0) ? QString("*") : QString::number(start + 1);
    const QString rhs = (end   < 0) ? QString("*") : QString::number(end + 1);
    return lhs + ":" + rhs;
}

}  // namespace

MappingPanel::MappingPanel(QWidget* parent) : QWidget(parent) {
    channelTable_ = new QTableWidget(0, ColCount, this);
    channelTable_->setHorizontalHeaderLabels({"Col", "Rows", "Role", "Name", "Unit"});
    channelTable_->horizontalHeader()->setSectionResizeMode(ColName, QHeaderView::Stretch);
    channelTable_->verticalHeader()->setVisible(false);
    channelTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    channelTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);

    addBtn_    = new QPushButton("Add channel…", this);
    editBtn_   = new QPushButton("Edit…", this);
    removeBtn_ = new QPushButton("Remove", this);

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

    useSampleRateCheck_ = new QCheckBox("X-axis from sample time / rate", this);
    sampleRateValue_    = new QDoubleSpinBox(this);
    sampleRateValue_->setRange(0.0, 1e12);
    sampleRateValue_->setDecimals(6);
    sampleRateValue_->setValue(1.0);
    sampleRateUnit_     = new QComboBox(this);
    for (const auto& u : kRateUnits) sampleRateUnit_->addItem(QString::fromUtf8(u.label));
    sampleRateUnit_->setCurrentIndex(kDefaultRateUnitIndex);

    applyBtn_ = new QPushButton("Apply (import signals)", this);
    saveBtn_  = new QPushButton("Save profile…", this);
    loadBtn_  = new QPushButton("Load profile…", this);

    auto* opts = new QGridLayout();
    int row = 0;
    opts->addWidget(new QLabel("Column separator:", this), row, 0);
    auto* csr = new QHBoxLayout();
    csr->addWidget(colSepCombo_); csr->addWidget(colSepCustom_); csr->addStretch();
    opts->addLayout(csr, row++, 1);

    opts->addWidget(new QLabel("Row separator:", this), row, 0);
    auto* rsr = new QHBoxLayout();
    rsr->addWidget(rowSepCombo_); rsr->addWidget(rowSepCustom_); rsr->addStretch();
    opts->addLayout(rsr, row++, 1);

    opts->addWidget(new QLabel("Header row (1-based):", this), row, 0);
    opts->addWidget(headerSpin_, row++, 1);

    opts->addWidget(new QLabel("Decimal separator:", this), row, 0);
    opts->addWidget(decimalEdit_, row++, 1);

    opts->addWidget(useSampleRateCheck_, row, 0);
    auto* rr = new QHBoxLayout();
    rr->addWidget(sampleRateValue_); rr->addWidget(sampleRateUnit_); rr->addStretch();
    opts->addLayout(rr, row++, 1);

    auto* chBtnRow = new QHBoxLayout();
    chBtnRow->addWidget(addBtn_);
    chBtnRow->addWidget(editBtn_);
    chBtnRow->addWidget(removeBtn_);
    chBtnRow->addStretch();

    auto* btnRow = new QHBoxLayout();
    btnRow->addWidget(applyBtn_);
    btnRow->addWidget(saveBtn_);
    btnRow->addWidget(loadBtn_);

    auto* root = new QVBoxLayout(this);
    root->addLayout(opts);
    root->addWidget(new QLabel("Channels:", this));
    root->addWidget(channelTable_, /*stretch=*/1);
    root->addLayout(chBtnRow);
    root->addLayout(btnRow);

    auto syncCustomVisibility = [this]{
        colSepCustom_->setVisible(colSepCombo_->currentIndex() == kCustomColIdx);
        rowSepCustom_->setVisible(rowSepCombo_->currentIndex() == kCustomRowIdx);
    };
    syncCustomVisibility();

    connect(applyBtn_, &QPushButton::clicked, this, &MappingPanel::applyRequested);
    connect(saveBtn_,  &QPushButton::clicked, this, &MappingPanel::saveProfileRequested);
    connect(loadBtn_,  &QPushButton::clicked, this, &MappingPanel::loadProfileRequested);
    connect(addBtn_,   &QPushButton::clicked, this, &MappingPanel::onAddChannel);
    connect(editBtn_,  &QPushButton::clicked, this, &MappingPanel::onEditChannel);
    connect(removeBtn_,&QPushButton::clicked, this, &MappingPanel::onRemoveChannel);
    connect(channelTable_, &QTableWidget::cellDoubleClicked,
            this, [this](int, int){ onEditChannel(); });

    auto emitParseChange = [this, syncCustomVisibility]{
        syncCustomVisibility();
        emit parseOptionsChanged();
    };
    connect(colSepCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [emitParseChange](int){ emitParseChange(); });
    connect(rowSepCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [emitParseChange](int){ emitParseChange(); });
    connect(colSepCustom_, &QLineEdit::editingFinished, this, [emitParseChange]{ emitParseChange(); });
    connect(rowSepCustom_, &QLineEdit::editingFinished, this, [emitParseChange]{ emitParseChange(); });
    connect(headerSpin_, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [emitParseChange](int){ emitParseChange(); });
    connect(decimalEdit_, &QLineEdit::editingFinished, this, [emitParseChange]{ emitParseChange(); });
}

void MappingPanel::appendChannelRow(const ColumnMapping& m) {
    const int row = channelTable_->rowCount();
    channelTable_->insertRow(row);
    auto* colItem  = new QTableWidgetItem(m.columnId);
    auto* rowsItem = new QTableWidgetItem(formatRowRange(m.rowStart, m.rowEnd));
    rowsItem->setData(Qt::UserRole,     m.rowStart);
    rowsItem->setData(Qt::UserRole + 1, m.rowEnd);
    auto* roleItem = new QTableWidgetItem(roleName(m.role));
    roleItem->setData(Qt::UserRole, static_cast<int>(m.role));
    auto* nameItem = new QTableWidgetItem(m.signalName);
    auto* unitItem = new QTableWidgetItem(m.unit);
    channelTable_->setItem(row, ColCol,  colItem);
    channelTable_->setItem(row, ColRows, rowsItem);
    channelTable_->setItem(row, ColRole, roleItem);
    channelTable_->setItem(row, ColName, nameItem);
    channelTable_->setItem(row, ColUnit, unitItem);
}

ColumnMapping MappingPanel::rowToMapping(int row) const {
    ColumnMapping m;
    if (row < 0 || row >= channelTable_->rowCount()) return m;
    m.columnId  = channelTable_->item(row, ColCol)->text();
    m.rowStart  = channelTable_->item(row, ColRows)->data(Qt::UserRole).toInt();
    m.rowEnd    = channelTable_->item(row, ColRows)->data(Qt::UserRole + 1).toInt();
    m.role      = static_cast<ColumnMapping::Role>(
                      channelTable_->item(row, ColRole)->data(Qt::UserRole).toInt());
    m.signalName= channelTable_->item(row, ColName)->text();
    m.unit      = channelTable_->item(row, ColUnit)->text();
    return m;
}

void MappingPanel::onAddChannel() {
    ChannelEditDialog dlg(this);
    while (dlg.exec() == QDialog::Accepted) {
        ColumnMapping m;
        QString err;
        if (!dlg.getMapping(&m, &err)) {
            QMessageBox::warning(this, "Invalid input", err);
            continue;
        }
        appendChannelRow(m);
        return;
    }
}

void MappingPanel::onEditChannel() {
    const int row = channelTable_->currentRow();
    if (row < 0) return;
    ChannelEditDialog dlg(this);
    dlg.setMapping(rowToMapping(row));
    while (dlg.exec() == QDialog::Accepted) {
        ColumnMapping m;
        QString err;
        if (!dlg.getMapping(&m, &err)) {
            QMessageBox::warning(this, "Invalid input", err);
            continue;
        }
        // Replace row in place
        channelTable_->item(row, ColCol)->setText(m.columnId);
        channelTable_->item(row, ColRows)->setText(formatRowRange(m.rowStart, m.rowEnd));
        channelTable_->item(row, ColRows)->setData(Qt::UserRole,     m.rowStart);
        channelTable_->item(row, ColRows)->setData(Qt::UserRole + 1, m.rowEnd);
        channelTable_->item(row, ColRole)->setText(roleName(m.role));
        channelTable_->item(row, ColRole)->setData(Qt::UserRole, static_cast<int>(m.role));
        channelTable_->item(row, ColName)->setText(m.signalName);
        channelTable_->item(row, ColUnit)->setText(m.unit);
        return;
    }
}

void MappingPanel::onRemoveChannel() {
    const int row = channelTable_->currentRow();
    if (row < 0) return;
    channelTable_->removeRow(row);
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

    useSampleRateCheck_->setChecked(p.useSampleRate);
    if (p.sampleRateHz > 0) {
        const int idx = unitIndexFromName(p.sampleRateDisplayUnit);
        sampleRateUnit_->setCurrentIndex(idx);
        const auto& u = kRateUnits[idx];
        const double v = (u.m > 0) ? p.sampleRateHz / u.m : 1.0 / (p.sampleRateHz * -u.m);
        sampleRateValue_->setValue(v);
    }

    channelTable_->setRowCount(0);
    for (const auto& c : p.columns) appendChannelRow(c);
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
    for (int r = 0; r < channelTable_->rowCount(); ++r) {
        p.columns.push_back(rowToMapping(r));
    }
    return p;
}

QString MappingPanel::columnDelimiter() const {
    if (colSepCombo_->currentIndex() == kCustomColIdx)
        return colSepCustom_->text().isEmpty() ? QString(",") : colSepCustom_->text();
    return colSepCombo_->currentData().toString();
}
QString MappingPanel::rowDelimiter() const {
    if (rowSepCombo_->currentIndex() == kCustomRowIdx)
        return rowSepCustom_->text().isEmpty() ? QString("\n") : rowSepCustom_->text();
    return rowSepCombo_->currentData().toString();
}
int     MappingPanel::headerRow()       const { return headerSpin_->value(); }
QString MappingPanel::decimal()         const { return decimalEdit_->text(); }
bool    MappingPanel::useSampleRate()   const { return useSampleRateCheck_->isChecked(); }
double  MappingPanel::sampleRateHz()    const {
    return rateValueToHz(sampleRateValue_->value(), sampleRateUnit_->currentIndex());
}
QString MappingPanel::sampleRateDisplayUnit() const { return sampleRateUnit_->currentText(); }

}  // namespace scope::converter::ui
