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
class QStackedWidget;
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
    // Unit input: a stacked widget so the parser-supported X-axis unit
    // ladders are enforced via a dropdown (no more silent fallback when
    // the user types a bogus string), while Y signals stay free-form
    // (V / rpm / °C / whatever).
    QStackedWidget* unitStack_{nullptr};
    QLineEdit*      unitEdit_{nullptr};      // index 0 — Y signal
    QComboBox*      unitTimeCombo_{nullptr}; // index 1 — X-axis (time)
    QComboBox*      unitFreqCombo_{nullptr}; // index 2 — X-axis (frequency)

    // X-source group (visible only for Y signals)
    QGroupBox*    xSourceGroup_;
    QRadioButton* useColumnRadio_;
    QComboBox*    xColumnCombo_;
    QRadioButton* useRateRadio_;
    QDoubleSpinBox* rateValue_;
    QComboBox*    rateUnit_;
    QCheckBox*      resetToZeroCheck_{nullptr};
    QDoubleSpinBox* offsetSpin_{nullptr};
    QComboBox*      collapseCombo_{nullptr};
    QComboBox*      plateauCombo_{nullptr};

    QStringList availableXColumns_;
};

}  // namespace scope::converter::ui
