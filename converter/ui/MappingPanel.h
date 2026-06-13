#pragma once

#include "scope/converter/ConverterProfile.h"

#include <QWidget>

class QTableWidget;
class QPushButton;
class QSpinBox;
class QDoubleSpinBox;
class QLineEdit;
class QComboBox;
class QCheckBox;

namespace scope::converter::ui {

// Manual channel-mapping editor. The host opens a CSV; the user clicks
// "Add channel" and types the column letter + row range + role + name + unit.
// The user can also enable an X-axis-from-sample-rate mode (default unit ms).
class MappingPanel : public QWidget {
    Q_OBJECT
public:
    explicit MappingPanel(QWidget* parent = nullptr);

    void setProfile(const ConverterProfile& p);
    ConverterProfile buildProfile(const QString& sourceType) const;

    // CSV exposes the delimiter / row / header / decimal / auto-detect parse
    // options; JSON's grid is fixed by JsonSource, so those are hidden for it.
    enum class SourceKind { Csv, Json };
    void setSourceKind(SourceKind kind);

    QString columnDelimiter() const;
    QString rowDelimiter()    const;
    int     headerRow()       const;
    QString decimal()         const;
    bool    useSampleRate()   const;
    double  sampleRateHz()    const;
    QString sampleRateDisplayUnit() const;

signals:
    void applyRequested();
    void saveProfileRequested();
    void loadProfileRequested();
    void parseOptionsChanged();
    // A column mapping was added, edited, or removed (for undo/redo).
    void mappingChanged();
    // The user picked an auto-detector in the dropdown and clicked Apply.
    // The host runs the matching detector on the active file and fills the
    // mapping (delimiter, decimal, header row, channel columns).
    void autoDetectRequested(const QString& detectorId);

private slots:
    void onAddChannel();
    void onEditChannel();
    void onRemoveChannel();

private:
    QTableWidget* channelTable_;
    QPushButton*  addBtn_;
    QPushButton*  editBtn_;
    QPushButton*  removeBtn_;

    QComboBox*    autoDetectCombo_{nullptr};
    QPushButton*  autoDetectBtn_{nullptr};

    // Holds the CSV-only parse options grid so setSourceKind() can hide it.
    QWidget*      parseOptionsWidget_{nullptr};

    QComboBox*    colSepCombo_;
    QLineEdit*    colSepCustom_;
    QComboBox*    rowSepCombo_;
    QLineEdit*    rowSepCustom_;
    QSpinBox*     headerSpin_;
    QLineEdit*    decimalEdit_;

    QPushButton*  applyBtn_;
    QPushButton*  saveBtn_;
    QPushButton*  loadBtn_;

    // Bulk-set row (above the Apply button): set Relative + Offset on
    // every Y signal in one click.
    QCheckBox*      bulkRelativeCheck_{nullptr};
    QDoubleSpinBox* bulkOffsetSpin_{nullptr};
    QPushButton*    bulkApplyBtn_{nullptr};

    void appendChannelRow(const ColumnMapping& m);
    ColumnMapping rowToMapping(int row) const;
};

}  // namespace scope::converter::ui
