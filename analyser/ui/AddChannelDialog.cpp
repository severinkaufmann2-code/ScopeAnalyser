#include "AddChannelDialog.h"

#include "scope/analyser/FormulaEngine.h"
#include "scope/analyser/FunctionRegistry.h"
#include "scope/core/SignalStore.h"

#include <QCompleter>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSplitter>
#include <QStringListModel>
#include <QTextBrowser>
#include <QTextCursor>
#include <QVBoxLayout>

namespace scope::analyser::ui {

namespace {

// QPlainTextEdit that pops a completer on Tab, navigates with arrow keys,
// and inserts on Enter / Tab.
class FormulaEdit : public QPlainTextEdit {
public:
    explicit FormulaEdit(QWidget* parent = nullptr) : QPlainTextEdit(parent) {
        completer_ = new QCompleter(this);
        completer_->setWidget(this);
        completer_->setCompletionMode(QCompleter::PopupCompletion);
        completer_->setCaseSensitivity(Qt::CaseInsensitive);
        QObject::connect(completer_, QOverload<const QString&>::of(&QCompleter::activated),
                         this, [this](const QString& chosen){ insertCompletion(chosen); });
    }

    void setCompletions(const QStringList& items) {
        completer_->setModel(new QStringListModel(items, completer_));
    }

protected:
    void keyPressEvent(QKeyEvent* e) override {
        // Tab: trigger the popup OR accept the current highlight.
        if (e->key() == Qt::Key_Tab) {
            if (completer_->popup()->isVisible()) {
                const auto modelIdx = completer_->popup()->currentIndex();
                if (modelIdx.isValid()) {
                    insertCompletion(modelIdx.data(Qt::DisplayRole).toString());
                    completer_->popup()->hide();
                    return;
                }
            }
            // Open the popup with the word under the cursor as the prefix.
            const QString prefix = wordUnderCursor();
            completer_->setCompletionPrefix(prefix);
            // Pre-select the first entry so a second Tab inserts it.
            if (completer_->completionCount() > 0) {
                completer_->popup()->setCurrentIndex(
                    completer_->completionModel()->index(0, 0));
            }
            QRect r = cursorRect();
            r.setWidth(completer_->popup()->sizeHintForColumn(0)
                       + completer_->popup()->verticalScrollBar()->sizeHint().width());
            completer_->complete(r);
            return;
        }
        if (completer_->popup()->isVisible()) {
            switch (e->key()) {
                case Qt::Key_Enter:
                case Qt::Key_Return: {
                    const auto modelIdx = completer_->popup()->currentIndex();
                    if (modelIdx.isValid()) {
                        insertCompletion(modelIdx.data(Qt::DisplayRole).toString());
                        completer_->popup()->hide();
                        return;
                    }
                    break;
                }
                case Qt::Key_Escape:
                    completer_->popup()->hide();
                    return;
                default: break;
            }
        }
        QPlainTextEdit::keyPressEvent(e);
        // Keep the popup live as the user types.
        if (completer_->popup()->isVisible()) {
            completer_->setCompletionPrefix(wordUnderCursor());
            if (completer_->completionCount() == 0) completer_->popup()->hide();
        }
    }

private:
    QCompleter* completer_;

    QString wordUnderCursor() const {
        QTextCursor c = textCursor();
        c.movePosition(QTextCursor::StartOfWord, QTextCursor::KeepAnchor);
        return c.selectedText();
    }

    void insertCompletion(const QString& chosen) {
        QTextCursor c = textCursor();
        const int extra = chosen.length() - completer_->completionPrefix().length();
        c.movePosition(QTextCursor::Left);
        c.movePosition(QTextCursor::EndOfWord);
        c.insertText(chosen.right(extra));
        setTextCursor(c);
    }
};

}  // namespace

AddChannelDialog::AddChannelDialog(scope::core::SignalStore& store,
                                   FormulaEngine&            engine,
                                   QWidget*                  parent)
    : QDialog(parent), store_(store), engine_(engine) {
    setWindowTitle("Add channel");
    resize(720, 380);

    nameEdit_ = new QLineEdit(this);
    nameEdit_->setPlaceholderText("e.g. SmoothedSpeed");

    formulaEdit_ = new FormulaEdit(this);
    formulaEdit_->setPlaceholderText(
        "e.g.  Filter(speed, 0.05)\n"
        "      Derivative(position, 0.05)\n"
        "      2 * a + b\n"
        "\n"
        "Tab to autocomplete channel and function names.");

    helpBrowser_ = new QTextBrowser(this);
    helpBrowser_->setMinimumWidth(260);

    auto* form = new QFormLayout();
    form->addRow("Name:",    nameEdit_);
    form->addRow("Formula:", formulaEdit_);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &AddChannelDialog::onAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* split = new QSplitter(Qt::Horizontal, this);

    auto* leftBox = new QWidget(this);
    auto* leftLayout = new QVBoxLayout(leftBox);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->addLayout(form);

    split->addWidget(leftBox);
    split->addWidget(helpBrowser_);
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 1);

    auto* root = new QVBoxLayout(this);
    root->addWidget(split, /*stretch=*/1);
    root->addWidget(buttons);

    buildHelp();
    refreshCompletions();
}

QString AddChannelDialog::channelName() const { return nameEdit_->text().trimmed(); }
QString AddChannelDialog::formula()     const { return formulaEdit_->toPlainText().trimmed(); }

void AddChannelDialog::refreshCompletions() {
    completions_.clear();
    for (const auto& n : store_.channelNames()) completions_ << n;
    for (const auto& f : FunctionRegistry::instance().all())
        completions_ << f.name + "(";
    static_cast<FormulaEdit*>(formulaEdit_)->setCompletions(completions_);
}

void AddChannelDialog::buildHelp() {
    QString html = "<h3>Functions</h3><ul>";
    for (const auto& f : FunctionRegistry::instance().all()) {
        html += QString("<li><b>%1</b> &mdash; %2</li>")
                    .arg(f.signature, f.summary);
    }
    html += "</ul><h3>Operators</h3><ul>"
            "<li><tt>+ - * /</tt> &mdash; element-wise on signals</li>"
            "<li>scalars broadcast: <tt>2 * a + 5</tt></li>"
            "</ul>";
    helpBrowser_->setHtml(html);
}

void AddChannelDialog::onAccept() {
    const QString name = channelName();
    const QString expr = formula();
    if (name.isEmpty()) {
        QMessageBox::warning(this, "Missing name", "Please enter a channel name.");
        nameEdit_->setFocus();
        return;
    }
    if (expr.isEmpty()) {
        QMessageBox::warning(this, "Missing formula", "Please enter a formula.");
        formulaEdit_->setFocus();
        return;
    }
    const QString line = name + " = " + expr;
    QString err;
    if (!engine_.evaluate(line, &err)) {
        QMessageBox::critical(this, "Formula error",
            err.isEmpty() ? QString("Unknown error.") : err);
        return;
    }
    accept();
}

}  // namespace scope::analyser::ui
