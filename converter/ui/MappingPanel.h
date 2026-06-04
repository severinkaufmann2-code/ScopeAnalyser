#pragma once

#include "scope/converter/ConverterProfile.h"

#include <QHash>
#include <QWidget>

class QTableWidget;
class QPushButton;
class QSpinBox;
class QDoubleSpinBox;
class QLineEdit;
class QComboBox;
class QCheckBox;

namespace scope::converter::ui {

class MappingPanel : public QWidget {
    Q_OBJECT
public:
    explicit MappingPanel(QWidget* parent = nullptr);

    void setColumns(const QStringList& columnLabels);
    void setProfile(const ConverterProfile& p);
    ConverterProfile buildProfile(const QString& sourceType) const;

    // Hooks used by ConverterWidget's selection-driven buttons.
    void setColumnMapping(const QString& columnLabel,
                          ColumnMapping::Role role,
                          int rowStart, int rowEnd,
                          const QString& name = QString(),
                          const QString& unit = QString());
    void clearColumnRole(const QString& columnLabel);
    void setUseSampleRate(bool use, double hz, const QString& displayUnit);

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

private:
    QTableWidget* table_;
    QComboBox*    colSepCombo_;
    QLineEdit*    colSepCustom_;
    QComboBox*    rowSepCombo_;
    QLineEdit*    rowSepCustom_;
    QSpinBox*     headerSpin_;
    QLineEdit*    decimalEdit_;

    QCheckBox*       useSampleRateCheck_;
    QDoubleSpinBox*  sampleRateValue_;
    QComboBox*       sampleRateUnit_;

    QPushButton*  applyBtn_;
    QPushButton*  saveBtn_;
    QPushButton*  loadBtn_;

    int rowIndexFor(const QString& columnLabel) const;
};

}  // namespace scope::converter::ui
