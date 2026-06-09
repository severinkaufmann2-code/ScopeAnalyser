#include "AddChannelDialog.h"

#include "scope/analyser/FormulaEngine.h"
#include "scope/analyser/FunctionRegistry.h"
#include "scope/core/SignalStore.h"

#include <QCompleter>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSpinBox>
#include <QSplitter>
#include <QStringListModel>
#include <QTextBrowser>
#include <QTextCursor>
#include <QVBoxLayout>

#include <qcustomplot.h>

#include <array>

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

// Map a function name to a previewable filter spec. Multi-order families
// overlay 3 representative orders; the 1st-order ones a single curve.
bool filterPreviewSpec(const QString& fn, QString& family, int& band,
                       std::vector<int>& orders) {
    if (fn == "Filter")      { family = "Filter";      band = 0; orders = {1};       return true; }
    if (fn == "FilterHP")    { family = "Filter";      band = 1; orders = {1};       return true; }
    if (fn == "PT")          { family = "PT";          band = 0; orders = {1, 2, 4}; return true; }
    if (fn == "Butterworth") { family = "Butterworth"; band = 0; orders = {2, 4, 8}; return true; }
    if (fn == "Cheby1")      { family = "Cheby1";      band = 0; orders = {2, 4, 8}; return true; }
    if (fn == "Cheby2")      { family = "Cheby2";      band = 0; orders = {2, 4, 8}; return true; }
    return false;
}

}  // namespace

AddChannelDialog::AddChannelDialog(scope::core::SignalStore& store,
                                   FormulaEngine&            engine,
                                   QWidget*                  parent)
    : QDialog(parent), store_(store), engine_(engine) {
    setWindowTitle("Add channel");
    resize(1000, 700);

    nameEdit_ = new QLineEdit(this);
    nameEdit_->setPlaceholderText("e.g. SmoothedSpeed");

    formulaEdit_ = new FormulaEdit(this);
    formulaEdit_->setPlaceholderText(
        "e.g.  Filter(speed, 0.05)\n"
        "      Butterworth(speed, 0, 4, 10)\n"
        "      2 * a + b\n"
        "\n"
        "Tab to autocomplete channel and function names.");

    auto* form = new QFormLayout();
    form->addRow("Name:",    nameEdit_);
    form->addRow("Formula:", formulaEdit_);

    // ---- Filter builder -------------------------------------------------
    auto* builder = new QGroupBox("Filter builder", this);
    srcCombo_ = new QComboBox(this);
    srcCombo_->setEditable(true);
    familyCombo_ = new QComboBox(this);
    familyCombo_->addItem("1st-order (Filter)", "Filter");
    familyCombo_->addItem("PT (n-th order lag)", "PT");
    familyCombo_->addItem("Butterworth", "Butterworth");
    familyCombo_->addItem("Chebyshev I", "Cheby1");
    familyCombo_->addItem("Chebyshev II", "Cheby2");
    bandCombo_ = new QComboBox(this);
    orderSpin_ = new QSpinBox(this); orderSpin_->setRange(1, 8); orderSpin_->setValue(2);
    orderLabel_ = new QLabel("Order:", this);
    tauSpin_ = new QDoubleSpinBox(this);
    tauSpin_->setRange(1e-6, 1e6); tauSpin_->setDecimals(4); tauSpin_->setValue(0.05);
    tauSpin_->setSuffix(" s");
    tauLabel_ = new QLabel("Time constant:", this);
    f1Spin_ = new QDoubleSpinBox(this);
    f1Spin_->setRange(1e-6, 1e9); f1Spin_->setDecimals(3); f1Spin_->setValue(10.0);
    f1Spin_->setSuffix(" Hz");
    f1Label_ = new QLabel("Cutoff f1:", this);
    f2Spin_ = new QDoubleSpinBox(this);
    f2Spin_->setRange(1e-6, 1e9); f2Spin_->setDecimals(3); f2Spin_->setValue(50.0);
    f2Spin_->setSuffix(" Hz");
    f2Label_ = new QLabel("Cutoff f2:", this);
    rippleSpin_ = new QDoubleSpinBox(this);
    rippleSpin_->setRange(0.01, 120.0); rippleSpin_->setDecimals(2); rippleSpin_->setValue(1.0);
    rippleSpin_->setSuffix(" dB");
    rippleLabel_ = new QLabel("Ripple:", this);
    insertBtn_ = new QPushButton("Insert formula", this);

    auto* g = new QGridLayout(builder);
    int gr = 0;
    g->addWidget(new QLabel("Source:", this), gr, 0); g->addWidget(srcCombo_,    gr++, 1);
    g->addWidget(new QLabel("Family:", this), gr, 0); g->addWidget(familyCombo_, gr++, 1);
    g->addWidget(new QLabel("Type:",   this), gr, 0); g->addWidget(bandCombo_,   gr++, 1);
    g->addWidget(orderLabel_,  gr, 0); g->addWidget(orderSpin_,  gr++, 1);
    g->addWidget(tauLabel_,    gr, 0); g->addWidget(tauSpin_,    gr++, 1);
    g->addWidget(f1Label_,     gr, 0); g->addWidget(f1Spin_,     gr++, 1);
    g->addWidget(f2Label_,     gr, 0); g->addWidget(f2Spin_,     gr++, 1);
    g->addWidget(rippleLabel_, gr, 0); g->addWidget(rippleSpin_, gr++, 1);
    g->addWidget(insertBtn_,   gr, 0, 1, 2);

    auto* leftBox = new QWidget(this);
    auto* leftLayout = new QVBoxLayout(leftBox);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->addLayout(form);
    leftLayout->addWidget(builder);
    leftLayout->addStretch();

    // ---- Functions + live response --------------------------------------
    funcList_ = new QListWidget(this);
    funcList_->setMouseTracking(true);
    helpBrowser_ = new QTextBrowser(this);
    helpBrowser_->setMaximumHeight(96);

    auto setupPlot = [](QCustomPlot* p, const QString& yLabel) {
        p->xAxis->setScaleType(QCPAxis::stLogarithmic);
        QSharedPointer<QCPAxisTickerLog> ticker(new QCPAxisTickerLog);
        p->xAxis->setTicker(ticker);
        p->xAxis->setLabel("f / f_c");
        p->yAxis->setLabel(yLabel);
        p->legend->setVisible(true);
        p->legend->setBrush(QColor(255, 255, 255, 190));
        p->setMinimumHeight(140);
    };
    magPlot_   = new QCustomPlot(this);
    phasePlot_ = new QCustomPlot(this);
    setupPlot(magPlot_,   "Magnitude [dB]");
    setupPlot(phasePlot_, "Phase [deg]");

    auto* rightBox = new QWidget(this);
    auto* rightLayout = new QVBoxLayout(rightBox);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->addWidget(new QLabel("Functions (hover a filter to preview):", this));
    rightLayout->addWidget(funcList_, /*stretch=*/2);
    rightLayout->addWidget(helpBrowser_);
    rightLayout->addWidget(new QLabel("Response (illustrative — shape only):", this));
    rightLayout->addWidget(magPlot_,   /*stretch=*/2);
    rightLayout->addWidget(phasePlot_, /*stretch=*/2);

    auto* split = new QSplitter(Qt::Horizontal, this);
    split->addWidget(leftBox);
    split->addWidget(rightBox);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 4);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &AddChannelDialog::onAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* root = new QVBoxLayout(this);
    root->addWidget(split, /*stretch=*/1);
    root->addWidget(buttons);

    connect(familyCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int){ rebuildBandCombo(); onBuilderChanged(); });
    connect(bandCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int){ onBuilderChanged(); });
    connect(orderSpin_, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int){ onBuilderChanged(); });
    connect(rippleSpin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double){ onBuilderChanged(); });
    connect(insertBtn_, &QPushButton::clicked, this, &AddChannelDialog::onInsertFilter);
    connect(funcList_, &QListWidget::itemEntered, this, &AddChannelDialog::onFunctionHovered);
    connect(funcList_, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem* cur, QListWidgetItem*){ if (cur) onFunctionHovered(cur); });

    buildHelp();
    refreshCompletions();
    rebuildBandCombo();
    onBuilderChanged();
}

QString AddChannelDialog::channelName() const { return nameEdit_->text().trimmed(); }
QString AddChannelDialog::formula()     const { return formulaEdit_->toPlainText().trimmed(); }

void AddChannelDialog::setEditMode(const QString& originalName, const QString& formula) {
    originalName_ = originalName;
    setWindowTitle("Edit channel");
    nameEdit_->setText(originalName);
    formulaEdit_->setPlainText(formula);
    formulaEdit_->setFocus();
}

void AddChannelDialog::refreshCompletions() {
    completions_.clear();
    for (const auto& n : store_.channelNames()) completions_ << n;
    for (const auto& f : FunctionRegistry::instance().all())
        completions_ << f.name + "(";
    static_cast<FormulaEdit*>(formulaEdit_)->setCompletions(completions_);

    if (srcCombo_) {
        const QString keep = srcCombo_->currentText();
        srcCombo_->clear();
        for (const auto& n : store_.channelNames()) srcCombo_->addItem(n);
        if (!keep.isEmpty()) srcCombo_->setCurrentText(keep);
    }
}

void AddChannelDialog::buildHelp() {
    funcList_->clear();
    for (const auto& f : FunctionRegistry::instance().all()) {
        auto* it = new QListWidgetItem(f.signature, funcList_);
        it->setData(Qt::UserRole, f.name);
        it->setToolTip(f.summary);
    }
    helpBrowser_->setHtml(
        "Hover a function for details. Filters also show their response above. "
        "Operators: <tt>+ - * /</tt> (element-wise; scalars broadcast).");
}

void AddChannelDialog::rebuildBandCombo() {
    const QString fam = familyCombo_->currentData().toString();
    const bool iir = (fam == "Butterworth" || fam == "Cheby1" || fam == "Cheby2");
    bandCombo_->blockSignals(true);
    bandCombo_->clear();
    bandCombo_->addItem("Low-pass", 0);
    bandCombo_->addItem("High-pass", 1);
    if (iir) {
        bandCombo_->addItem("Band-pass", 2);
        bandCombo_->addItem("Band-stop", 3);
    }
    bandCombo_->blockSignals(false);
}

void AddChannelDialog::onBuilderChanged() {
    const QString fam = familyCombo_->currentData().toString();
    const int band = bandCombo_->currentData().toInt();
    const bool iir = (fam == "Butterworth" || fam == "Cheby1" || fam == "Cheby2");
    const bool isFirstOrder = (fam == "Filter");
    const bool isBandType = (band == 2 || band == 3);
    const bool hasRipple = (fam == "Cheby1" || fam == "Cheby2");

    orderLabel_->setVisible(!isFirstOrder);  orderSpin_->setVisible(!isFirstOrder);
    tauLabel_->setVisible(!iir);             tauSpin_->setVisible(!iir);
    f1Label_->setVisible(iir);               f1Spin_->setVisible(iir);
    f2Label_->setVisible(iir && isBandType); f2Spin_->setVisible(iir && isBandType);
    rippleLabel_->setVisible(hasRipple);     rippleSpin_->setVisible(hasRipple);
    rippleLabel_->setText(fam == "Cheby2" ? "Stop atten:" : "Ripple:");

    const int order = isFirstOrder ? 1 : orderSpin_->value();
    plotResponse(fam, band, static_cast<int>(rippleSpin_->value()), {order});
}

void AddChannelDialog::onInsertFilter() {
    const QString fam = familyCombo_->currentData().toString();
    const int band = bandCombo_->currentData().toInt();
    QString src = srcCombo_->currentText().trimmed();
    if (src.isEmpty()) src = "signal";
    const int order = (fam == "Filter") ? 1 : orderSpin_->value();
    auto num = [](double v) { return QString::number(v, 'g', 6); };

    QString formula;
    if (fam == "Filter") {
        formula = (band == 1)
            ? QString("FilterHP(%1, %2)").arg(src, num(tauSpin_->value()))
            : QString("Filter(%1, %2)").arg(src, num(tauSpin_->value()));
    } else if (fam == "PT") {
        formula = QString("PT(%1, %2, %3, %4)")
                      .arg(src).arg(band).arg(order).arg(num(tauSpin_->value()));
    } else {
        const QString cut = (band == 2 || band == 3)
            ? QString("%1, %2").arg(num(f1Spin_->value()), num(f2Spin_->value()))
            : num(f1Spin_->value());
        if (fam == "Butterworth")
            formula = QString("Butterworth(%1, %2, %3, %4)")
                          .arg(src).arg(band).arg(order).arg(cut);
        else if (fam == "Cheby1")
            formula = QString("Cheby1(%1, %2, %3, %4, %5)")
                          .arg(src).arg(band).arg(order).arg(num(rippleSpin_->value()), cut);
        else
            formula = QString("Cheby2(%1, %2, %3, %4, %5)")
                          .arg(src).arg(band).arg(order).arg(num(rippleSpin_->value()), cut);
    }
    formulaEdit_->setPlainText(formula);
    if (nameEdit_->text().trimmed().isEmpty())
        nameEdit_->setText(src + "_" + fam.toLower());
}

void AddChannelDialog::onFunctionHovered(QListWidgetItem* item) {
    if (!item) return;
    const QString name = item->data(Qt::UserRole).toString();
    if (const auto* d = FunctionRegistry::instance().find(name)) {
        helpBrowser_->setHtml(QString("<b>%1</b><br>%2")
            .arg(d->signature.toHtmlEscaped(), d->summary.toHtmlEscaped()));
    }
    QString family; int band = 0; std::vector<int> orders;
    if (filterPreviewSpec(name, family, band, orders)) {
        const int rip = (family == "Cheby2") ? 40 : 1;
        plotResponse(family, band, rip, orders);
    }
}

void AddChannelDialog::plotResponse(const QString& family, int band, int ripDb,
                                    const std::vector<int>& orders) {
    static const std::array<QColor, 3> cols{
        QColor(31, 119, 180), QColor(255, 127, 14), QColor(44, 160, 44)};
    magPlot_->clearGraphs();
    phasePlot_->clearGraphs();
    for (std::size_t i = 0; i < orders.size(); ++i) {
        FilterPlotSpec spec;
        spec.family = family;
        spec.band   = band;
        spec.order  = orders[i];
        spec.ripple = ripDb > 0 ? ripDb : 1;
        const auto resp = filterFreqResponse(spec);
        QVector<double> x, mdb, ph;
        x.reserve(static_cast<int>(resp.freqHz.size()));
        for (std::size_t k = 0; k < resp.freqHz.size(); ++k) {
            const double fc = resp.fc > 0 ? resp.fc : 1.0;
            x.push_back(resp.freqHz[k] / fc);
            mdb.push_back(resp.magDb[k]);
            ph.push_back(resp.phaseDeg[k]);
        }
        const QColor c = cols[i % cols.size()];
        const QString nm = (orders.size() == 1) ? family
                                                : QString("order %1").arg(orders[i]);
        auto* gm = magPlot_->addGraph();   gm->setData(x, mdb); gm->setPen(QPen(c)); gm->setName(nm);
        auto* gp = phasePlot_->addGraph(); gp->setData(x, ph);  gp->setPen(QPen(c)); gp->setName(nm);
    }
    magPlot_->xAxis->setRange(0.01, 100.0);
    magPlot_->yAxis->setRange(-80.0, 6.0);
    phasePlot_->xAxis->setRange(0.01, 100.0);
    phasePlot_->yAxis->rescale();
    const bool legend = orders.size() > 1;
    magPlot_->legend->setVisible(legend);
    phasePlot_->legend->setVisible(legend);
    magPlot_->replot();
    phasePlot_->replot();
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
    // Edit mode + rename: drop the old entry so we don't leave a stale
    // copy under the previous name.
    if (!originalName_.isEmpty() && originalName_ != name) {
        store_.remove(originalName_);
    }
    accept();
}

}  // namespace scope::analyser::ui
