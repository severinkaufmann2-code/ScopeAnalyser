#pragma once

#include "scope/converter/ConverterProfile.h"

#include <QHash>
#include <QWidget>

class QTableWidget;
class QPushButton;
class QSpinBox;
class QLineEdit;
class QComboBox;

namespace scope::converter::ui {

// Per-column role mapping editor + parse options (column / row / decimal
// separators, header row). Emits `parseOptionsChanged` when any parse option
// changes so the host can re-read the source file; `applyRequested` when the
// user confirms a profile to import.
class MappingPanel : public QWidget {
    Q_OBJECT
public:
    explicit MappingPanel(QWidget* parent = nullptr);

    void setColumns(const QStringList& columnLabels);
    void setProfile(const ConverterProfile& p);
    ConverterProfile buildProfile(const QString& sourceType) const;

    QString columnDelimiter() const;  // single-character string
    QString rowDelimiter()    const;  // "\n", "\r\n", or arbitrary user-entered string
    int     headerRow()       const;
    QString decimal()         const;

signals:
    void applyRequested();
    void saveProfileRequested();
    void loadProfileRequested();
    void parseOptionsChanged();       // separators / header row / decimal changed

private:
    QTableWidget* table_;
    QComboBox*    colSepCombo_;
    QLineEdit*    colSepCustom_;
    QComboBox*    rowSepCombo_;
    QLineEdit*    rowSepCustom_;
    QSpinBox*     headerSpin_;
    QLineEdit*    decimalEdit_;
    QPushButton*  applyBtn_;
    QPushButton*  saveBtn_;
    QPushButton*  loadBtn_;
};

}  // namespace scope::converter::ui
