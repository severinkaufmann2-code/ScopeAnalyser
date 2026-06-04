#pragma once

#include "scope/converter/CsvWriter.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLineEdit;

namespace scope::converter::ui {

// Small options form for "Save CSV". Defaults are pre-filled so OK alone
// produces a sensible standard CSV (comma columns, newline rows, dot decimal,
// header row, shared time column).
class CsvExportDialog : public QDialog {
    Q_OBJECT
public:
    explicit CsvExportDialog(QWidget* parent = nullptr);

    CsvExportOptions options() const;

private:
    QCheckBox* headerCheck_;
    QComboBox* colSepCombo_;
    QLineEdit* colSepCustom_;
    QComboBox* rowSepCombo_;
    QLineEdit* rowSepCustom_;
    QLineEdit* decimalEdit_;
    QComboBox* timeModeCombo_;
};

}  // namespace scope::converter::ui
