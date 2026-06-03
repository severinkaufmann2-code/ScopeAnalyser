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
}

MappingPanel::MappingPanel(QWidget* parent) : QWidget(parent) {
    table_ = new QTableWidget(0, ColCount, this);
    table_->setHorizontalHeaderLabels({"Col", "Role", "Signal name", "Unit"});
    table_->horizontalHeader()->setSectionResizeMode(ColName, QHeaderView::Stretch);
    table_->verticalHeader()->setVisible(false);

    headerSpin_  = new QSpinBox(this);
    headerSpin_->setRange(0, 1000);
    headerSpin_->setValue(1);
    decimalEdit_ = new QLineEdit(".", this);
    decimalEdit_->setMaximumWidth(40);

    applyBtn_ = new QPushButton("Apply (import signals)", this);
    saveBtn_  = new QPushButton("Save profile…", this);
    loadBtn_  = new QPushButton("Load profile…", this);

    auto* opts = new QGridLayout();
    opts->addWidget(new QLabel("Header row (1-based):", this), 0, 0);
    opts->addWidget(headerSpin_, 0, 1);
    opts->addWidget(new QLabel("Decimal separator:", this), 1, 0);
    opts->addWidget(decimalEdit_, 1, 1);

    auto* btnRow = new QHBoxLayout();
    btnRow->addWidget(applyBtn_);
    btnRow->addWidget(saveBtn_);
    btnRow->addWidget(loadBtn_);

    auto* root = new QVBoxLayout(this);
    root->addLayout(opts);
    root->addWidget(table_, /*stretch=*/1);
    root->addLayout(btnRow);

    connect(applyBtn_, &QPushButton::clicked, this, &MappingPanel::applyRequested);
    connect(saveBtn_,  &QPushButton::clicked, this, &MappingPanel::saveProfileRequested);
    connect(loadBtn_,  &QPushButton::clicked, this, &MappingPanel::loadProfileRequested);
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

void MappingPanel::setHeaderRow(int row) { headerSpin_->setValue(row); }
int  MappingPanel::headerRow() const     { return headerSpin_->value(); }
void MappingPanel::setDecimal(const QString& dec) { decimalEdit_->setText(dec); }
QString MappingPanel::decimal() const { return decimalEdit_->text(); }

}  // namespace scope::converter::ui
