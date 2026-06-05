#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

class QLineEdit;
class QPlainTextEdit;
class QTextBrowser;

namespace scope::core { class SignalStore; }

namespace scope::analyser {
class FormulaEngine;
}

namespace scope::analyser::ui {

// Dialog for adding a derived channel. The user types a name and a
// formula expression (e.g. "Filter(speed, 0.05)" or "a + 2*b").
// On accept, the dialog runs `<name> = <formula>` through the
// FormulaEngine. Tab in the formula field triggers / cycles
// autocompletion over channel names + function signatures.
class AddChannelDialog : public QDialog {
    Q_OBJECT
public:
    AddChannelDialog(scope::core::SignalStore& store,
                     FormulaEngine&            engine,
                     QWidget*                  parent = nullptr);

    QString channelName() const;
    QString formula()     const;

private slots:
    void onAccept();

private:
    void refreshCompletions();
    void buildHelp();

    scope::core::SignalStore& store_;
    FormulaEngine&            engine_;

    QLineEdit*       nameEdit_{nullptr};
    QPlainTextEdit*  formulaEdit_{nullptr};
    QTextBrowser*    helpBrowser_{nullptr};
    QStringList      completions_;
};

}  // namespace scope::analyser::ui
