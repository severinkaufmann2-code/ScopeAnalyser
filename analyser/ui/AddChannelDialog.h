#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

#include <vector>

class QLineEdit;
class QPlainTextEdit;
class QTextBrowser;
class QComboBox;
class QSpinBox;
class QDoubleSpinBox;
class QListWidget;
class QLabel;
class QPushButton;
class QListWidgetItem;
class QStackedWidget;
class QCustomPlot;

namespace scope::core { class SignalStore; }

namespace scope::analyser {
class FormulaEngine;
}

namespace scope::analyser::ui {

// Dialog for adding (or editing) a derived channel. The user types a
// name and a formula expression (e.g. "Filter(speed, 0.05)" or
// "a + 2*b"). On accept, the dialog runs `<name> = <formula>` through
// the FormulaEngine. Tab in the formula field triggers / cycles
// autocompletion over channel names + function signatures.
//
// Edit mode: call setEditMode(originalName, formula) before exec().
// The dialog pre-fills both fields and changes its title. If the user
// renames the channel during edit, the original entry is dropped from
// the store after the new formula evaluates successfully.
class AddChannelDialog : public QDialog {
    Q_OBJECT
public:
    AddChannelDialog(scope::core::SignalStore& store,
                     FormulaEngine&            engine,
                     QWidget*                  parent = nullptr);

    void setEditMode(const QString& originalName, const QString& formula);

    QString channelName() const;
    QString formula()     const;

private slots:
    void onAccept();

private:
    void refreshCompletions();
    void buildHelp();

    // Filter builder + live response preview.
    void rebuildBandCombo();          // band options depend on the family
    void onBuilderChanged();          // refresh field visibility + preview
    void onInsertFilter();            // write the generated formula
    void onFunctionSelected(QListWidgetItem* item);  // chart/describe on click
    // fixedScale = true (hover) → identical axes for every filter, for easy
    // comparison; false (builder) → auto-fit the one filter being designed.
    void plotResponse(const QString& family, int band, int ripDb, int ripStopDb,
                      const std::vector<int>& orders, bool fixedScale);
    // Time-domain input→output example for a non-filter function: runs the
    // real impl on a representative input and plots in/out.
    void plotExample(const QString& name);

    scope::core::SignalStore& store_;
    FormulaEngine&            engine_;

    QLineEdit*       nameEdit_{nullptr};
    QPlainTextEdit*  formulaEdit_{nullptr};
    QTextBrowser*    helpBrowser_{nullptr};
    QStringList      completions_;
    QString          originalName_;   // non-empty → editing

    // Builder widgets.
    QComboBox*       srcCombo_{nullptr};
    QComboBox*       familyCombo_{nullptr};
    QComboBox*       bandCombo_{nullptr};
    QSpinBox*        orderSpin_{nullptr};
    QLabel*          orderLabel_{nullptr};
    QDoubleSpinBox*  tauSpin_{nullptr};
    QLabel*          tauLabel_{nullptr};
    QDoubleSpinBox*  f1Spin_{nullptr};
    QLabel*          f1Label_{nullptr};
    QDoubleSpinBox*  f2Spin_{nullptr};
    QLabel*          f2Label_{nullptr};
    QDoubleSpinBox*  rippleSpin_{nullptr};
    QLabel*          rippleLabel_{nullptr};
    QDoubleSpinBox*  rippleStopSpin_{nullptr};   // Elliptic stopband atten (Rs)
    QLabel*          rippleStopLabel_{nullptr};
    QPushButton*     insertBtn_{nullptr};

    QListWidget*     funcList_{nullptr};
    QLabel*          previewLabel_{nullptr};
    QStackedWidget*  previewStack_{nullptr};   // page 0 = bode, 1 = I/O example
    QCustomPlot*     magPlot_{nullptr};
    QCustomPlot*     phasePlot_{nullptr};
    QCustomPlot*     ioInPlot_{nullptr};        // example input (time)
    QCustomPlot*     ioOutPlot_{nullptr};       // example output (time or spectrum)
};

}  // namespace scope::analyser::ui
