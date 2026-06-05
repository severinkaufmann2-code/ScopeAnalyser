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

    QString columnDelimiter() const;
    QString rowDelimiter()    const;
    int     headerRow()       const;
    QString decimal()         const;
    bool    useSampleRate()   const;
    double  sampleRateHz()    const;
    QString sampleRateDisplayUnit() const;

    // Number of rows in the channel-mapping table. Used by the host so it
    // can detect "user configured mappings but didn't apply yet" before
    // switching to a different source file.
    int channelRowCount() const;

signals:
    void applyRequested();
    void saveProfileRequested();
    void loadProfileRequested();
    void parseOptionsChanged();

private slots:
    void onAddChannel();
    void onEditChannel();
    void onRemoveChannel();

private:
    QTableWidget* channelTable_;
    QPushButton*  addBtn_;
    QPushButton*  editBtn_;
    QPushButton*  removeBtn_;

    QComboBox*    colSepCombo_;
    QLineEdit*    colSepCustom_;
    QComboBox*    rowSepCombo_;
    QLineEdit*    rowSepCustom_;
    QSpinBox*     headerSpin_;
    QLineEdit*    decimalEdit_;

    QPushButton*  applyBtn_;
    QPushButton*  saveBtn_;
    QPushButton*  loadBtn_;

    void appendChannelRow(const ColumnMapping& m);
    ColumnMapping rowToMapping(int row) const;
};

}  // namespace scope::converter::ui
