#include "ChannelEditDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>

namespace scope::converter::ui {

namespace {

// Parse "5:100", "5-100", "5:", ":100", "all" or "" into (start, end) using
// 1-based row numbers from the user's perspective. Output is 0-based;
// returns -1 for an open end.
bool parseRowRange(const QString& text, int* startOut, int* endOut, QString* errOut) {
    const QString s = text.trimmed();
    if (s.isEmpty() || s.compare("all", Qt::CaseInsensitive) == 0) {
        *startOut = -1; *endOut = -1; return true;
    }

    // Accept ":" or "-" as the separator.
    int sep = s.indexOf(':');
    if (sep < 0) sep = s.indexOf('-');
    QString lhs, rhs;
    if (sep < 0) {
        // Single value: treat as both start AND end (one row).
        lhs = s; rhs = s;
    } else {
        lhs = s.left(sep).trimmed();
        rhs = s.mid(sep + 1).trimmed();
    }

    auto parseEnd = [&](const QString& t, int* out) -> bool {
        if (t.isEmpty() || t == "*") { *out = -1; return true; }
        bool ok = false;
        const int n = t.toInt(&ok);
        if (!ok || n < 1) return false;
        *out = n - 1;  // 1-based UI → 0-based internal
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
        case ColumnMapping::Role::Signal: return 0;
        default: return 0;
    }
}
ColumnMapping::Role roleFromIndex(int i) {
    switch (i) {
        case 0: return ColumnMapping::Role::Signal;
        case 1: return ColumnMapping::Role::XTime;
        default: return ColumnMapping::Role::Signal;
    }
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

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);
}

void ChannelEditDialog::setMapping(const ColumnMapping& m) {
    setWindowTitle("Edit channel");
    colEdit_->setText(m.columnId);
    rowsEdit_->setText(formatRowRange(m.rowStart, m.rowEnd));
    roleCombo_->setCurrentIndex(indexFromRole(m.role));
    nameEdit_->setText(m.signalName);
    unitEdit_->setText(m.unit);
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
    return true;
}

}  // namespace scope::converter::ui
