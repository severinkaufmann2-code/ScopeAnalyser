#pragma once

#include "scope/converter/CsvWriter.h"

#include <QDialog>
#include <QStringList>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QRadioButton;

namespace scope::core { class SignalStore; }

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

    // HTML only: write the chart without the embedded data island — a much
    // smaller file that still works offline in a browser but can no longer
    // be re-opened in ScopeAnalyser. False for every other format.
    bool      htmlViewOnly() const;

    // Domain / origin filters and split toggle.
    bool      includeTimeDomain()        const;
    bool      includeFrequencyDomain()   const;
    bool      includeDerivedChannels()   const;
    bool      splitDomainsIntoTwoFiles() const;

    // Names of the individually ticked channels, for
    // ChartSaveFilters::selectedChannels. Empty when no picker was populated
    // (setChannels never called) — which that field reads as "no per-channel
    // restriction", so hosts that don't offer one are unaffected.
    QStringList selectedChannels() const;

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

    // Tell the dialog what a Shared-time-column CSV would silently drop (see
    // scanRepeatedTimestamps). Shows a warning next to the Time mode picker
    // while CSV + Shared are selected; a scan with nothing to report clears
    // it. Hosts pass their store's channels; formats other than CSV are
    // unaffected, so calling this is always safe.
    void setRepeatedTimestamps(const scope::converter::RepeatedTimestampScan& scan);

    // Populate the per-channel picker from a store: one ticked row per
    // channel, sorted by name, each labelled with its unit and (when the
    // store holds both) its domain. Without this call the group shows only
    // the domain-level toggles, exactly as before.
    void setChannels(const scope::core::SignalStore& store);

private slots:
    void onFormatChanged();
    void onCsvTimeModeChanged();
    // Grey out rows their domain / derived toggles exclude, and refresh the
    // "n of m selected" caption.
    void onChannelFiltersChanged();
    void onRangeModeChanged();
    void onMetadataChanged();

private:
    QRadioButton*   h5Radio_{nullptr};
    QRadioButton*   csvRadio_{nullptr};
    QRadioButton*   mf4Radio_{nullptr};
    QRadioButton*   htmlRadio_{nullptr};
    QRadioButton*   jsonRadio_{nullptr};
    QCheckBox*      htmlViewOnlyCheck_{nullptr};
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
    QLabel*     repeatWarning_{nullptr};
    scope::converter::RepeatedTimestampScan repeatScan_;

    QGroupBox*  filtersGroup_{nullptr};
    QCheckBox*  includeTimeCheck_{nullptr};
    QCheckBox*  includeFreqCheck_{nullptr};
    QCheckBox*  includeDerivedCheck_{nullptr};
    QCheckBox*  splitFilesCheck_{nullptr};
    QListWidget* channelList_{nullptr};
    QWidget*     channelButtons_{nullptr};
    QLabel*      channelCount_{nullptr};
    QDialogButtonBox* buttons_{nullptr};

    QGroupBox*  metadataGroup_{nullptr};
    QCheckBox*  addMetadataCheck_{nullptr};
    QCheckBox*  mathFormulaCheck_{nullptr};
    QCheckBox*  noMathDataCheck_{nullptr};
    QCheckBox*  layoutCheck_{nullptr};
};

}  // namespace scope::converter::ui
