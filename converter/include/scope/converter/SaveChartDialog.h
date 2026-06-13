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
    enum class Format { Hdf5, Csv, Mdf4, Html, Json };

    // offerMetadata=false hides the "Metadata" group (the Converter embeds no
    // layout/formula metadata, so the toggles would be inert there).
    SaveChartDialog(double currentXMinSec, double currentXMaxSec,
                    QWidget* parent = nullptr, bool offerMetadata = true);

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

    // Embedded-metadata toggles (Analyser). Each accessor returns the
    // *effective* value, already accounting for the parent toggles:
    //   includeMathFormula()    ⇒ addMetadata() && the box
    //   noDataForMathChannels() ⇒ includeMathFormula() && the box
    //   includeLayout()         ⇒ addMetadata() && the box
    bool      addMetadata()            const;
    bool      includeMathFormula()     const;
    bool      noDataForMathChannels()  const;
    bool      includeLayout()          const;

    // Opt-in section trimming for hosts with a narrower scope (e.g. the
    // Recorder, which is time-domain only with no math channels).
    //   setChannelFiltersVisible(false) — hide the "Channels to include"
    //     group; its checkboxes stay checked so the include-all / no-split
    //     getters keep their defaults.
    //   setMetadataLayoutOnly(true) — within "Metadata", show only the
    //     Layout checkbox (hide the "Add metadata" master + the math
    //     toggles). The master stays checked so includeLayout() is honoured.
    void setChannelFiltersVisible(bool on);
    void setMetadataLayoutOnly(bool layoutOnly);

private slots:
    void onFormatChanged();
    void onRangeModeChanged();
    void onMetadataChanged();

private:
    QRadioButton*   h5Radio_{nullptr};
    QRadioButton*   csvRadio_{nullptr};
    QRadioButton*   mf4Radio_{nullptr};
    QRadioButton*   htmlRadio_{nullptr};
    QRadioButton*   jsonRadio_{nullptr};
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
    QComboBox*  timeAxisCombo_{nullptr};

    QGroupBox*  filtersGroup_{nullptr};
    QCheckBox*  includeTimeCheck_{nullptr};
    QCheckBox*  includeFreqCheck_{nullptr};
    QCheckBox*  includeDerivedCheck_{nullptr};
    QCheckBox*  splitFilesCheck_{nullptr};

    QGroupBox*  metadataGroup_{nullptr};
    QCheckBox*  addMetadataCheck_{nullptr};
    QCheckBox*  mathFormulaCheck_{nullptr};
    QCheckBox*  noMathDataCheck_{nullptr};
    QCheckBox*  layoutCheck_{nullptr};
};

}  // namespace scope::converter::ui
