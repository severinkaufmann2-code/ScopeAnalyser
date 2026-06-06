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

enum Col { ColCol = 0, ColRows, ColRole, ColName, ColUnit, ColXSource, ColCount };

// Custom roles to stash the per-channel X-source state on the X-source item.
constexpr int kRoleXSourceColumn       = Qt::UserRole;       // QString
constexpr int kRoleUseSampleRate       = Qt::UserRole + 1;   // bool
constexpr int kRoleSampleRateHz        = Qt::UserRole + 2;   // double
constexpr int kRoleSampleRateUnit      = Qt::UserRole + 3;   // QString
constexpr int kRoleResetTimeToZero     = Qt::UserRole + 4;   // bool
constexpr int kRoleTimeOffsetSec       = Qt::UserRole + 5;   // double
constexpr int kRoleCollapseMode        = Qt::UserRole + 6;   // int (CollapseMode)

QString xSourceLabel(const ColumnMapping& m) {
    if (m.role != ColumnMapping::Role::Signal) return "—";
    QString base;
    if (m.useSampleRate) {
        base = QString("rate: %1 %2")
            .arg(QString::number(
                (m.sampleRateDisplayUnit == "Hz" || m.sampleRateDisplayUnit == "kHz"
                 || m.sampleRateDisplayUnit == "MHz")
                ? (m.sampleRateDisplayUnit == "Hz"  ? m.sampleRateHz
                  : m.sampleRateDisplayUnit == "kHz" ? m.sampleRateHz / 1e3
                  : m.sampleRateHz / 1e6)
                : (m.sampleRateDisplayUnit == "s"  ? 1.0 / m.sampleRateHz
                  : m.sampleRateDisplayUnit == "ms" ? 1e3 / m.sampleRateHz
                  : m.sampleRateDisplayUnit == "µs" ? 1e6 / m.sampleRateHz
                  : m.sampleRateDisplayUnit == "ns" ? 1e9 / m.sampleRateHz
                  : m.sampleRateHz), 'g', 4))
            .arg(m.sampleRateDisplayUnit);
    } else {
        base = m.xSourceColumn.isEmpty() ? "(auto)" : ("col " + m.xSourceColumn);
    }
    QStringList badges;
    if (m.resetTimeToZero)        badges << "rel";
    if (m.timeOffsetSec != 0.0)   badges << QString("%1%2s")
                                              .arg(m.timeOffsetSec >= 0 ? "+" : "")
                                              .arg(m.timeOffsetSec, 0, 'g', 4);
    switch (m.collapseDuplicates) {
        case ColumnMapping::CollapseMode::First: badges << "dedup:first"; break;
        case ColumnMapping::CollapseMode::Last:  badges << "dedup:last";  break;
        case ColumnMapping::CollapseMode::Mean:  badges << "dedup:mean";  break;
        case ColumnMapping::CollapseMode::None:  default: break;
    }
    if (badges.isEmpty()) return base;
    return base + "  [" + badges.join(", ") + "]";
}

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
    channelTable_->setHorizontalHeaderLabels({"Col", "Rows", "Role", "Name", "Unit", "X source"});
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

    // Profile-level sample-rate widgets used to live here; they're now
    // per-channel (in ChannelEditDialog). The data fields stay on the profile
    // struct for back-compat with older .scaconv files.

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

    auto* chBtnRow = new QHBoxLayout();
    chBtnRow->addWidget(addBtn_);
    chBtnRow->addWidget(editBtn_);
    chBtnRow->addWidget(removeBtn_);
    chBtnRow->addStretch();

    // "Apply to all" row: bulk-set Relative + Offset for every Y signal.
    bulkRelativeCheck_ = new QCheckBox("Relative", this);
    bulkOffsetSpin_    = new QDoubleSpinBox(this);
    bulkOffsetSpin_->setRange(-1e9, 1e9);
    bulkOffsetSpin_->setDecimals(6);
    bulkOffsetSpin_->setSuffix(" s");
    bulkOffsetSpin_->setValue(0.0);
    bulkApplyBtn_      = new QPushButton("Apply to all", this);
    bulkApplyBtn_->setToolTip(
        "Bulk-set the Relative + Offset on every Y signal in the table.\n"
        "Per-channel values can still be edited individually afterwards.");
    auto* bulkRow = new QHBoxLayout();
    bulkRow->addWidget(new QLabel("All channels:", this));
    bulkRow->addWidget(bulkRelativeCheck_);
    bulkRow->addWidget(new QLabel("Offset:", this));
    bulkRow->addWidget(bulkOffsetSpin_);
    bulkRow->addWidget(bulkApplyBtn_);
    bulkRow->addStretch();
    connect(bulkApplyBtn_, &QPushButton::clicked, this, [this]{
        const bool rel = bulkRelativeCheck_->isChecked();
        const double off = bulkOffsetSpin_->value();
        for (int r = 0; r < channelTable_->rowCount(); ++r) {
            auto m = rowToMapping(r);
            if (m.role != ColumnMapping::Role::Signal) continue;
            m.resetTimeToZero = rel;
            m.timeOffsetSec   = off;
            auto* xs = channelTable_->item(r, ColXSource);
            if (!xs) continue;
            xs->setText(xSourceLabel(m));
            xs->setData(kRoleResetTimeToZero, m.resetTimeToZero);
            xs->setData(kRoleTimeOffsetSec,   m.timeOffsetSec);
        }
    });

    auto* btnRow = new QHBoxLayout();
    btnRow->addWidget(applyBtn_);
    btnRow->addWidget(saveBtn_);
    btnRow->addWidget(loadBtn_);

    auto* root = new QVBoxLayout(this);
    root->addLayout(opts);
    root->addWidget(new QLabel("Channels:", this));
    root->addWidget(channelTable_, /*stretch=*/1);
    root->addLayout(chBtnRow);
    root->addLayout(bulkRow);
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
    auto* xSrcItem = new QTableWidgetItem(xSourceLabel(m));
    xSrcItem->setData(kRoleXSourceColumn,   m.xSourceColumn);
    xSrcItem->setData(kRoleUseSampleRate,   m.useSampleRate);
    xSrcItem->setData(kRoleSampleRateHz,    m.sampleRateHz);
    xSrcItem->setData(kRoleSampleRateUnit,  m.sampleRateDisplayUnit);
    xSrcItem->setData(kRoleResetTimeToZero, m.resetTimeToZero);
    xSrcItem->setData(kRoleTimeOffsetSec,   m.timeOffsetSec);
    xSrcItem->setData(kRoleCollapseMode,    static_cast<int>(m.collapseDuplicates));
    channelTable_->setItem(row, ColCol,    colItem);
    channelTable_->setItem(row, ColRows,   rowsItem);
    channelTable_->setItem(row, ColRole,   roleItem);
    channelTable_->setItem(row, ColName,   nameItem);
    channelTable_->setItem(row, ColUnit,   unitItem);
    channelTable_->setItem(row, ColXSource, xSrcItem);
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
    if (auto* xs = channelTable_->item(row, ColXSource)) {
        m.xSourceColumn         = xs->data(kRoleXSourceColumn).toString();
        m.useSampleRate         = xs->data(kRoleUseSampleRate).toBool();
        m.sampleRateHz          = xs->data(kRoleSampleRateHz).toDouble();
        m.sampleRateDisplayUnit = xs->data(kRoleSampleRateUnit).toString();
        if (m.sampleRateDisplayUnit.isEmpty()) m.sampleRateDisplayUnit = "ms";
        m.resetTimeToZero       = xs->data(kRoleResetTimeToZero).toBool();
        m.timeOffsetSec         = xs->data(kRoleTimeOffsetSec).toDouble();
        m.collapseDuplicates    = static_cast<ColumnMapping::CollapseMode>(
            xs->data(kRoleCollapseMode).toInt());
    }
    return m;
}

void MappingPanel::onAddChannel() {
    ChannelEditDialog dlg(this);
    QStringList xCols;
    for (int r = 0; r < channelTable_->rowCount(); ++r) {
        const auto m = rowToMapping(r);
        if (m.role == ColumnMapping::Role::XTime) xCols << m.columnId;
    }
    dlg.setAvailableXColumns(xCols);
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
    QStringList xCols;
    for (int r = 0; r < channelTable_->rowCount(); ++r) {
        if (r == row) continue;
        const auto m = rowToMapping(r);
        if (m.role == ColumnMapping::Role::XTime) xCols << m.columnId;
    }
    dlg.setAvailableXColumns(xCols);
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
        auto* xs = channelTable_->item(row, ColXSource);
        xs->setText(xSourceLabel(m));
        xs->setData(kRoleXSourceColumn,   m.xSourceColumn);
        xs->setData(kRoleUseSampleRate,   m.useSampleRate);
        xs->setData(kRoleSampleRateHz,    m.sampleRateHz);
        xs->setData(kRoleSampleRateUnit,  m.sampleRateDisplayUnit);
        xs->setData(kRoleResetTimeToZero, m.resetTimeToZero);
        xs->setData(kRoleTimeOffsetSec,   m.timeOffsetSec);
        xs->setData(kRoleCollapseMode,    static_cast<int>(m.collapseDuplicates));
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

    // Profile-level useSampleRate / sampleRateHz are kept on the loaded
    // profile for back-compat with old files; no UI here.

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
    // New profiles use only per-channel sample rate. Profile-level fields
    // are zero-initialised in the data model; old loaded profiles preserve
    // their values for back-compat (we don't write them back to false here).
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
bool    MappingPanel::useSampleRate()   const { return false; }
double  MappingPanel::sampleRateHz()    const { return 0.0; }
QString MappingPanel::sampleRateDisplayUnit() const { return QString("ms"); }

}  // namespace scope::converter::ui
