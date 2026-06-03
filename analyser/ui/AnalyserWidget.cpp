#include "scope/analyser/AnalyserWidget.h"
#include "scope/analyser/FormulaEngine.h"
#include "scope/analyser/FunctionRegistry.h"

#include "AnalyserPlot.h"

#include <QAbstractItemView>
#include <QCompleter>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSplitter>
#include <QStringListModel>
#include <QTextBrowser>
#include <QTextCursor>
#include <QVBoxLayout>

#include <memory>

namespace scope::analyser {

namespace {

// QPlainTextEdit subclass that owns a QCompleter for channel names + functions.
class FormulaEdit : public QPlainTextEdit {
public:
    explicit FormulaEdit(QWidget* parent = nullptr) : QPlainTextEdit(parent) {
        completer_ = new QCompleter(this);
        completer_->setWidget(this);
        completer_->setCompletionMode(QCompleter::PopupCompletion);
        completer_->setCaseSensitivity(Qt::CaseInsensitive);
        QObject::connect(completer_, QOverload<const QString&>::of(&QCompleter::activated),
                         this, [this](const QString& chosen){ insertCompletion(chosen); });
        setPlaceholderText("e.g.  Out = Filter(Mock.sine_1hz, 0.05)\n"
                           "      Diff = Derivative(Mock.sawtooth)\n"
                           "      Sum  = Mock.sine_1hz + 2*Mock.cosine_10hz");
    }

    void setCompletions(const QStringList& items) {
        completer_->setModel(new QStringListModel(items, completer_));
    }

protected:
    void keyPressEvent(QKeyEvent* e) override {
        if (completer_->popup()->isVisible()) {
            switch (e->key()) {
                case Qt::Key_Enter:
                case Qt::Key_Return:
                case Qt::Key_Escape:
                case Qt::Key_Tab:
                case Qt::Key_Backtab:
                    e->ignore();
                    return;
                default:
                    break;
            }
        }
        const bool isShortcut = (e->modifiers() & Qt::ControlModifier) && e->key() == Qt::Key_Space;
        if (!isShortcut) QPlainTextEdit::keyPressEvent(e);

        const QString prefix = wordUnderCursor();
        if (!isShortcut && (e->text().isEmpty() || prefix.length() < 1)) {
            completer_->popup()->hide();
            return;
        }
        completer_->setCompletionPrefix(prefix);
        QRect r = cursorRect();
        r.setWidth(completer_->popup()->sizeHintForColumn(0)
                   + completer_->popup()->verticalScrollBar()->sizeHint().width());
        completer_->complete(r);
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

AnalyserWidget::AnalyserWidget(scope::core::SignalStore& store, QWidget* parent)
    : QWidget(parent), store_(store) {
    engine_ = std::make_unique<FormulaEngine>(store_);

    editor_     = new FormulaEdit(this);
    evalBtn_    = new QPushButton("Evaluate", this);
    channelList_ = new QListWidget(this);
    helpBrowser_ = new QTextBrowser(this);
    statusLabel_ = new QLabel("Ready", this);
    plot_       = new ui::AnalyserPlot(store_, this);

    channelList_->setMaximumWidth(220);
    helpBrowser_->setMaximumHeight(220);

    buildHelpPanel();
    refreshChannelList();

    auto* editorBox = new QVBoxLayout();
    editorBox->addWidget(new QLabel("Formula (Ctrl+Space for autocomplete):", this));
    editorBox->addWidget(editor_, /*stretch=*/1);
    auto* bottomRow = new QHBoxLayout();
    bottomRow->addWidget(evalBtn_);
    bottomRow->addStretch();
    bottomRow->addWidget(statusLabel_);
    editorBox->addLayout(bottomRow);

    auto* leftBox = new QVBoxLayout();
    leftBox->addWidget(new QLabel("Channels", this));
    leftBox->addWidget(channelList_);
    leftBox->addWidget(new QLabel("Function help", this));
    leftBox->addWidget(helpBrowser_);

    auto* top = new QHBoxLayout();
    top->addLayout(leftBox);
    top->addLayout(editorBox, /*stretch=*/1);

    auto* split = new QSplitter(Qt::Vertical, this);
    auto* topWrap = new QWidget(this);
    topWrap->setLayout(top);
    split->addWidget(topWrap);
    split->addWidget(plot_);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 2);

    auto* root = new QVBoxLayout(this);
    root->addWidget(split);

    connect(evalBtn_, &QPushButton::clicked, this, &AnalyserWidget::onEvaluateClicked);
    connect(&store_, &scope::core::SignalStore::channelAdded,
            this, &AnalyserWidget::onChannelAdded);
    connect(&store_, &scope::core::SignalStore::channelRemoved,
            this, &AnalyserWidget::onChannelRemoved);
    connect(channelList_, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem*){ onChannelDoubleClicked(); });
}

AnalyserWidget::~AnalyserWidget() = default;

void AnalyserWidget::buildHelpPanel() {
    QString html = "<h3>Functions</h3><ul>";
    for (const auto& f : FunctionRegistry::instance().all()) {
        html += QString("<li><b>%1</b> &mdash; %2</li>").arg(f.signature, f.summary);
    }
    html += "</ul><h3>Operators</h3><ul>"
            "<li><tt>+ - * /</tt> &mdash; element-wise on channels of equal length</li>"
            "<li>scalar literals broadcast: <tt>2 * Ch1 + 5</tt></li>"
            "</ul>";
    helpBrowser_->setHtml(html);
}

void AnalyserWidget::refreshChannelList() {
    channelList_->clear();
    QStringList completions;
    for (const auto& name : store_.channelNames()) {
        channelList_->addItem(name);
        completions << name;
    }
    for (const auto& f : FunctionRegistry::instance().all()) {
        completions << f.name + "(";
    }
    static_cast<FormulaEdit*>(editor_)->setCompletions(completions);
}

void AnalyserWidget::onChannelAdded(QString /*name*/)   { refreshChannelList(); }
void AnalyserWidget::onChannelRemoved(QString /*name*/) { refreshChannelList(); }

void AnalyserWidget::onChannelDoubleClicked() {
    auto* item = channelList_->currentItem();
    if (!item) return;
    editor_->insertPlainText(item->text());
    editor_->setFocus();
}

void AnalyserWidget::onEvaluateClicked() {
    const QString text = editor_->toPlainText();
    const auto lines = text.split('\n', Qt::SkipEmptyParts);
    int ok = 0, fail = 0;
    QString lastErr;
    for (const auto& line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith('#') || trimmed.startsWith("//")) continue;
        QString err;
        if (engine_->evaluate(trimmed, &err)) ++ok;
        else { ++fail; lastErr = err; }
    }
    if (fail == 0) {
        statusLabel_->setText(QString("OK — evaluated %1 line(s)").arg(ok));
    } else {
        statusLabel_->setText(QString("ERROR (%1 failed): %2").arg(fail).arg(lastErr));
    }
}

}  // namespace scope::analyser
