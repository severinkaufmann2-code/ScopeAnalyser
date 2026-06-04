#include "MappingPanel.h"

#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

namespace scope::converter::ui {

namespace {
enum Col { ColLabel = 0, ColRole, ColName, ColUnit, ColCount };
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

constexpr int kCustomColIdx = 4;   // index of "Custom..." in column-sep combo
constexpr int kCustomRowIdx = 2;   // index of "Custom..." in row-sep combo

// Each combo item carries its actual delimiter string in Qt::UserRole.
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

}  // namespace

MappingPanel::MappingPanel(QWidget* parent) : QWidget(parent) {
    table_ = new QTableWidget(0, ColCount, this);
    table_->setHorizontalHeaderLabels({"Col", "Role", "Signal name", "Unit"});
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
    headerSpin_->setRange(0, 1000);
    headerSpin_->setValue(1);
    decimalEdit_ = new QLineEdit(".", this);
    decimalEdit_->setMaximumWidth(40);

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
    // Header row and decimal don't change the column count; emit anyway so
    // host can re-render preview headers if it wants to.
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
    }
}

void MappingPanel::setProfile(const ConverterProfile& p) {
    // Setting combos triggers parseOptionsChanged signals; the host re-parses
    // the file after this call, which is what we want.
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

    for (const auto& c : p.columns) {
        for (int r = 0; r < table_->rowCount(); ++r) {
            if (table_->item(r, ColLabel)->text() == c.columnId) {
                if (auto* combo = qobject_cast<QComboBox*>(table_->cellWidget(r, ColRole)))
                    combo->setCurrentIndex(indexFromRole(c.role));
                table_->item(r, ColName)->setText(c.signalName);
                table_->item(r, ColUnit)->setText(c.unit);
                break;
            }
        }
    }
}

ConverterProfile MappingPanel::buildProfile(const QString& sourceType) const {
    ConverterProfile p;
    p.sourceType = sourceType;
    p.headerRow = headerSpin_->value();
    p.decimalSeparator = decimalEdit_->text().isEmpty() ? QString(".") : decimalEdit_->text();
    p.columnDelimiter = columnDelimiter();
    p.rowDelimiter    = rowDelimiter();
    for (int r = 0; r < table_->rowCount(); ++r) {
        ColumnMapping cm;
        cm.columnId = table_->item(r, ColLabel)->text();
        if (auto* combo = qobject_cast<QComboBox*>(table_->cellWidget(r, ColRole)))
            cm.role = roleFromIndex(combo->currentIndex());
        cm.signalName = table_->item(r, ColName)->text();
        cm.unit       = table_->item(r, ColUnit)->text();
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

}  // namespace scope::converter::ui
