#pragma once

#include "scope/converter/ConverterProfile.h"

#include <QDialog>
#include <QStringList>

class QLineEdit;
class QComboBox;
class QRadioButton;
class QDoubleSpinBox;
class QGroupBox;
class QCheckBox;
class QWidget;

namespace scope::converter::ui {

class ChannelEditDialog : public QDialog {
    Q_OBJECT
public:
    explicit ChannelEditDialog(QWidget* parent = nullptr);

    // List of currently-available X column letters in the parent panel.
    void setAvailableXColumns(const QStringList& labels);

    void setMapping(const ColumnMapping& m);
    bool getMapping(ColumnMapping* out, QString* errorOut = nullptr) const;

private slots:
    void onRoleChanged();
    void onXSourceModeChanged();

private:
    void rebuildXSourceCombo();

    QLineEdit*    colEdit_;
    QLineEdit*    rowsEdit_;
    QComboBox*    roleCombo_;
    QLineEdit*    nameEdit_;
    QLineEdit*    unitEdit_;

    // X-source group (visible only for Y signals)
    QGroupBox*    xSourceGroup_;
    QRadioButton* useColumnRadio_;
    QComboBox*    xColumnCombo_;
    QRadioButton* useRateRadio_;
    QDoubleSpinBox* rateValue_;
    QComboBox*    rateUnit_;
    QCheckBox*    resetToZeroCheck_;

    QStringList availableXColumns_;
};

}  // namespace scope::converter::ui
