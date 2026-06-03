#pragma once

#include "scope/converter/ConverterProfile.h"

#include <QHash>
#include <QWidget>

class QTableWidget;
class QPushButton;
class QSpinBox;
class QLineEdit;

namespace scope::converter::ui {

// Per-column role mapping editor. One row per column found in the source.
// Each row: column label (read-only), role combo, signal name (editable when
// role=Signal), unit (editable).
class MappingPanel : public QWidget {
    Q_OBJECT
public:
    explicit MappingPanel(QWidget* parent = nullptr);

    void setColumns(const QStringList& columnLabels);
    void setProfile(const ConverterProfile& p);
    ConverterProfile buildProfile(const QString& sourceType) const;
    void setHeaderRow(int row);
    int  headerRow() const;
    void setDecimal(const QString& dec);
    QString decimal() const;

signals:
    void applyRequested();
    void saveProfileRequested();
    void loadProfileRequested();

private:
    QTableWidget* table_;
    QSpinBox*     headerSpin_;
    QLineEdit*    decimalEdit_;
    QPushButton*  applyBtn_;
    QPushButton*  saveBtn_;
    QPushButton*  loadBtn_;
};

}  // namespace scope::converter::ui
