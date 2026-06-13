#pragma once

#include "scope/converter/CsvWriter.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLineEdit;
class QRadioButton;

namespace scope::converter::ui {

// Save dialog shared by both Analyser ("Save chart…") and Converter
// ("Save chart…"). Returns:
//   - format (.h5, .mf4, .csv)
//   - time range (all data, or custom seconds From/To)
//   - CSV options (header, scope metadata, separators, decimal, time mode)
//     used only when format == csv. Defaults pre-filled so OK alone
//     produces a standard CSV.
//   - per-domain channel filters and split-files toggle.
class SaveChartDialog : public QDialog {
    Q_OBJECT
public:
    enum class Format { Hdf5, Csv, Mdf4, Html };

    SaveChartDialog(double currentXMinSec, double currentXMaxSec,
                    QWidget* parent = nullptr);

    Format    format()    const;
    bool      useCustomRange() const;
    double    fromSec()   const;
    double    toSec()     const;
    scope::converter::CsvExportOptions csvOptions() const;

    // Domain / origin filters and split toggle.
    bool      includeTimeDomain()        const;
    bool      includeFrequencyDomain()   const;
    bool      includeDerivedChannels()   const;
    bool      splitDomainsIntoTwoFiles() const;

private slots:
    void onFormatChanged();
    void onRangeModeChanged();

private:
    QRadioButton*   h5Radio_{nullptr};
    QRadioButton*   csvRadio_{nullptr};
    QRadioButton*   mf4Radio_{nullptr};
    QRadioButton*   htmlRadio_{nullptr};
    QRadioButton*   allRangeRadio_{nullptr};
    QRadioButton*   customRangeRadio_{nullptr};
    QDoubleSpinBox* fromSpin_{nullptr};
    QDoubleSpinBox* toSpin_{nullptr};

    QGroupBox*  csvGroup_{nullptr};
    QCheckBox*  headerCheck_{nullptr};
    QCheckBox*  metadataCheck_{nullptr};
    QComboBox*  colSepCombo_{nullptr};
    QLineEdit*  colSepCustom_{nullptr};
    QComboBox*  rowSepCombo_{nullptr};
    QLineEdit*  rowSepCustom_{nullptr};
    QLineEdit*  decimalEdit_{nullptr};
    QComboBox*  timeModeCombo_{nullptr};

    QCheckBox*  includeTimeCheck_{nullptr};
    QCheckBox*  includeFreqCheck_{nullptr};
    QCheckBox*  includeDerivedCheck_{nullptr};
    QCheckBox*  splitFilesCheck_{nullptr};
};

}  // namespace scope::converter::ui
