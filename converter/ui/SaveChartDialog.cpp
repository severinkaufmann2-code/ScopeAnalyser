#include "scope/converter/SaveChartDialog.h"

#include "scope/core/Signal.h"
#include "scope/core/SignalStore.h"

#include "scope/style/StyleKit.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QScreen>
#include <QGuiApplication>
#include <QVBoxLayout>

#include <algorithm>
#include <array>

namespace scope::converter::ui {

namespace {

struct SepOption { const char* label; const char* value; };
const std::array<SepOption, 5> kColSepOptions = {{
    {"Comma  ,",      ","},
    {"Semicolon  ;",  ";"},
    {"Tab  \\t",      "\t"},
    {"Pipe  |",       "|"},
    {"Custom…",       ""},
}};
const std::array<SepOption, 3> kRowSepOptions = {{
    {"LF  \\n",       "\n"},
    {"CRLF  \\r\\n",  "\r\n"},
    {"Custom…",       ""},
}};
constexpr int kCustomColIdx = 4;
// Beyond this the channel list scrolls instead of growing.
constexpr int kMaxVisibleChannelRows = 6;
constexpr int kCustomRowIdx = 2;

}  // namespace

SaveChartDialog::SaveChartDialog(double currentXMinSec, double currentXMaxSec,
                                 QWidget* parent, bool offerMetadata)
    : QDialog(parent) {
    setWindowTitle("Save chart");
    setSizeGripEnabled(true);

    // ---- Format ------------------------------------------------------
    h5Radio_  = new QRadioButton("HDF5 (.h5) — lossless recording format", this);
    mf4Radio_ = new QRadioButton("MDF4 (.mf4) — ASAM standard, opens in DiaDem / asammdf / MATLAB", this);
    csvRadio_ = new QRadioButton("CSV (.csv) — text, configurable below", this);
    htmlRadio_ = new QRadioButton(
        "HTML (.html) — interactive chart, opens in any browser (offline) and "
        "re-opens in ScopeAnalyser", this);
    jsonRadio_ = new QRadioButton(
        "JSON (.json) — per-signal arrays, bit-exact re-open in ScopeAnalyser", this);
    h5Radio_->setChecked(true);
    // Nested under the HTML radio. What actually costs bytes is precision:
    // full-precision doubles off real instrumentation barely compress. This
    // rounds them to chart resolution, which also means the file is no
    // longer a faithful copy — hence one-way.
    htmlViewOnlyCheck_ = new QCheckBox(
        "Smaller file — chart only, can't be re-opened here", this);
    htmlViewOnlyCheck_->setChecked(false);
    htmlViewOnlyCheck_->setToolTip(
        "Leave off to keep the samples exactly, so the file re-opens in\n"
        "ScopeAnalyser like .h5 / .mf4 / .csv / .json do.\n\n"
        "Turn on to round values to 6 significant figures — more than a\n"
        "chart can resolve, and typically a little over half the size on\n"
        "real measurement data. The page still opens in any browser offline\n"
        "with zoom, Δ Measure and channel toggles, but the values are now\n"
        "display-quality, so ScopeAnalyser refuses to load it back. Don't\n"
        "use it as your only copy of the data.");
    auto* formatBox = new QGroupBox("Format", this);
    auto* formatLayout = new QVBoxLayout(formatBox);
    formatLayout->addWidget(h5Radio_);
    formatLayout->addWidget(mf4Radio_);
    formatLayout->addWidget(csvRadio_);
    formatLayout->addWidget(htmlRadio_);
    auto* htmlOptRow = new QHBoxLayout();
    htmlOptRow->addSpacing(20);
    htmlOptRow->addWidget(htmlViewOnlyCheck_);
    htmlOptRow->addStretch();
    formatLayout->addLayout(htmlOptRow);
    formatLayout->addWidget(jsonRadio_);

    // ---- Time range --------------------------------------------------
    allRangeRadio_    = new QRadioButton("All data", this);
    customRangeRadio_ = new QRadioButton("Custom range:", this);
    allRangeRadio_->setChecked(true);
    fromSpin_ = new QDoubleSpinBox(this);
    fromSpin_->setRange(-1e12, 1e12);
    fromSpin_->setDecimals(6);
    fromSpin_->setSuffix(" s");
    fromSpin_->setValue(currentXMinSec);
    toSpin_ = new QDoubleSpinBox(this);
    toSpin_->setRange(-1e12, 1e12);
    toSpin_->setDecimals(6);
    toSpin_->setSuffix(" s");
    toSpin_->setValue(currentXMaxSec);

    auto* rangeBox = new QGroupBox("Time range", this);
    auto* rangeLayout = new QVBoxLayout(rangeBox);
    rangeLayout->addWidget(allRangeRadio_);
    auto* customRow = new QHBoxLayout();
    customRow->addWidget(customRangeRadio_);
    customRow->addWidget(new QLabel("from", this));
    customRow->addWidget(fromSpin_);
    customRow->addWidget(new QLabel("to", this));
    customRow->addWidget(toSpin_);
    customRow->addStretch();
    rangeLayout->addLayout(customRow);

    // ---- CSV options (only relevant when CSV selected) --------------
    csvGroup_ = new QGroupBox("CSV options", this);
    headerCheck_ = new QCheckBox("Include header row", csvGroup_);
    headerCheck_->setChecked(true);

    colSepCombo_ = new QComboBox(csvGroup_);
    for (const auto& o : kColSepOptions)
        colSepCombo_->addItem(o.label, QString::fromUtf8(o.value));
    colSepCustom_ = new QLineEdit(csvGroup_);
    colSepCustom_->setMaximumWidth(80);
    colSepCustom_->setPlaceholderText("single char");
    colSepCustom_->setVisible(false);

    rowSepCombo_ = new QComboBox(csvGroup_);
    for (const auto& o : kRowSepOptions)
        rowSepCombo_->addItem(o.label, QString::fromUtf8(o.value));
    rowSepCustom_ = new QLineEdit(csvGroup_);
    rowSepCustom_->setMaximumWidth(120);
    rowSepCustom_->setVisible(false);

    decimalEdit_ = new QLineEdit(".", csvGroup_);
    decimalEdit_->setMaximumWidth(40);

    timeModeCombo_ = new QComboBox(csvGroup_);
    timeModeCombo_->addItem("Shared time column (interpolated)");
    timeModeCombo_->addItem("Per-signal time columns (exact)");
    timeModeCombo_->setToolTip(
        "How the time column(s) are written.\n"
        "Shared: one t column, signals interpolated onto the union of all\n"
        "timestamps — spreadsheet-friendly, but one row per distinct\n"
        "timestamp, so samples that repeat a timestamp don't fit.\n"
        "Per-signal: each signal keeps its own exact (t, value) pairs —\n"
        "every sample is written, at the cost of a wider file.");

    // Only shown once a scan says this data actually repeats timestamps and
    // Shared is selected — see setRepeatedTimestamps().
    repeatWarning_ = scope::style::makePill(csvGroup_);
    repeatWarning_->setWordWrap(true);
    repeatWarning_->setVisible(false);

    timeAxisCombo_ = new QComboBox(csvGroup_);
    timeAxisCombo_->addItem("Epoch nanoseconds (exact)");
    timeAxisCombo_->addItem("Relative seconds (readable)");
    timeAxisCombo_->setToolTip(
        "How the time column is written.\n"
        "Epoch nanoseconds: the absolute timestamp as integer ns — lossless\n"
        "and absolute, but large integers (not spreadsheet-friendly).\n"
        "Relative seconds: seconds from the first sample, as a decimal —\n"
        "readable, but loses the absolute time and sub-ns precision.\n"
        "Either re-opens correctly in ScopeAnalyser.");

    metadataCheck_ = new QCheckBox(
        "Include scope metadata header (# scope-csv: …)", csvGroup_);
    metadataCheck_->setChecked(true);
    metadataCheck_->setToolTip(
        "Embed a commented JSON line on top of the file describing each\n"
        "column's role (X-time / X-frequency / signal), unit and name.\n"
        "Lets the loader round-trip the file losslessly — frequency-domain\n"
        "channels go back into the Frequency view, etc. Most CSV consumers\n"
        "skip '#' comment lines; Excel does not.");

    auto* csvForm = new QFormLayout(csvGroup_);
    csvForm->addRow(headerCheck_);
    csvForm->addRow(metadataCheck_);
    auto* colSepRow = new QHBoxLayout();
    colSepRow->addWidget(colSepCombo_); colSepRow->addWidget(colSepCustom_); colSepRow->addStretch();
    auto* colSepWrap = new QWidget(csvGroup_);
    colSepWrap->setLayout(colSepRow);
    csvForm->addRow("Column separator:", colSepWrap);
    auto* rowSepRow = new QHBoxLayout();
    rowSepRow->addWidget(rowSepCombo_); rowSepRow->addWidget(rowSepCustom_); rowSepRow->addStretch();
    auto* rowSepWrap = new QWidget(csvGroup_);
    rowSepWrap->setLayout(rowSepRow);
    csvForm->addRow("Row separator:", rowSepWrap);
    csvForm->addRow("Decimal separator:", decimalEdit_);
    csvForm->addRow("Time mode:", timeModeCombo_);
    csvForm->addRow(QString(), repeatWarning_);
    csvForm->addRow("Time column:", timeAxisCombo_);

    // ---- Channel filters ---------------------------------------------
    filtersGroup_ = new QGroupBox("Channels to include", this);
    auto* filtersBox = filtersGroup_;
    includeTimeCheck_     = new QCheckBox("Time-domain channels", filtersBox);
    includeFreqCheck_     = new QCheckBox("Frequency-domain channels", filtersBox);
    includeDerivedCheck_  = new QCheckBox("Derived (formula) channels", filtersBox);
    splitFilesCheck_      = new QCheckBox(
        "Split time and frequency into separate files", filtersBox);
    includeTimeCheck_->setChecked(true);
    includeFreqCheck_->setChecked(true);
    includeDerivedCheck_->setChecked(true);
    splitFilesCheck_->setChecked(false);   // default off — single file
    splitFilesCheck_->setToolTip(
        "When on, time-domain channels go to <name>_time.<ext> and\n"
        "frequency-domain to <name>_frequency.<ext>. When off, both\n"
        "domains are written into one file. For CSV with the Shared\n"
        "Time mode, a mixed-domain single file gets one shared 't [s]'\n"
        "column plus one shared 'f [Hz]' column written side-by-side.");
    // Per-channel picker. Stays hidden until a host calls setChannels(), so
    // the group keeps its old shape for callers that don't offer one.
    channelList_ = new QListWidget(filtersBox);
    channelList_->setSelectionMode(QAbstractItemView::NoSelection);
    channelList_->setUniformItemSizes(true);
    channelList_->setVisible(false);
    channelList_->setToolTip(
        "Tick the channels to write. A channel also has to pass the "
        "domain / derived filters above — rows those exclude are greyed out.");

    auto* allBtn  = new QPushButton("All", filtersBox);
    auto* noneBtn = new QPushButton("None", filtersBox);
    for (auto* b : {allBtn, noneBtn}) b->setAutoDefault(false);
    channelCount_ = new QLabel(filtersBox);
    channelCount_->setProperty("scopeRole", "dim");

    auto* chanBtnRow = new QWidget(filtersBox);
    auto* chanBtnLayout = new QHBoxLayout(chanBtnRow);
    chanBtnLayout->setContentsMargins(0, 0, 0, 0);
    chanBtnLayout->addWidget(allBtn);
    chanBtnLayout->addWidget(noneBtn);
    chanBtnLayout->addWidget(channelCount_);
    chanBtnLayout->addStretch();
    chanBtnRow->setVisible(false);
    channelButtons_ = chanBtnRow;

    auto setAllChecked = [this](bool on) {
        for (int i = 0; i < channelList_->count(); ++i) {
            auto* it = channelList_->item(i);
            // Skip what the domain / derived filters already exclude —
            // "All" means "all the ones that can actually be written".
            if (it->flags() & Qt::ItemIsEnabled)
                it->setCheckState(on ? Qt::Checked : Qt::Unchecked);
        }
        onChannelFiltersChanged();
    };
    connect(allBtn,  &QPushButton::clicked, this, [setAllChecked]{ setAllChecked(true); });
    connect(noneBtn, &QPushButton::clicked, this, [setAllChecked]{ setAllChecked(false); });
    connect(channelList_, &QListWidget::itemChanged, this,
            [this](QListWidgetItem*){ onChannelFiltersChanged(); });

    auto* filtersLayout = new QVBoxLayout(filtersBox);
    filtersLayout->addWidget(includeTimeCheck_);
    filtersLayout->addWidget(includeFreqCheck_);
    filtersLayout->addWidget(includeDerivedCheck_);
    filtersLayout->addWidget(splitFilesCheck_);
    filtersLayout->addWidget(channelList_);
    filtersLayout->addWidget(chanBtnRow);

    // ---- Metadata (Analyser only) ------------------------------------
    // What gets embedded in the saved file so it can be re-opened: the math
    // formulas and/or the plot layout. Nested so a child is only choosable
    // once its parent is on.
    metadataGroup_ = new QGroupBox("Metadata", this);
    addMetadataCheck_ = new QCheckBox("Add metadata", metadataGroup_);
    addMetadataCheck_->setChecked(true);
    addMetadataCheck_->setToolTip(
        "Embed metadata so the file re-opens in ScopeAnalyser with its math\n"
        "channels and layout. Off → a plain data file (no layout / formulas).");
    mathFormulaCheck_ = new QCheckBox("Math formula", metadataGroup_);
    mathFormulaCheck_->setChecked(true);
    mathFormulaCheck_->setToolTip(
        "Embed each derived channel's formula so it can be edited / recomputed\n"
        "after re-opening. Off → math channels re-open as plain signals.");
    noMathDataCheck_ = new QCheckBox(
        "No data for math channels", metadataGroup_);
    noMathDataCheck_->setChecked(false);
    noMathDataCheck_->setToolTip(
        "Don't write the computed samples of math channels — keep only the\n"
        "formula. They're recomputed from the sources on re-open (smaller\n"
        "file; needs the sources present and 'Import formula' on re-open).");
    layoutCheck_ = new QCheckBox("Layout", metadataGroup_);
    layoutCheck_->setChecked(true);
    layoutCheck_->setToolTip(
        "Embed the Y axes, channel→axis assignments and view mode so the\n"
        "chart re-opens looking the same.");

    auto* metaLayout = new QVBoxLayout(metadataGroup_);
    metaLayout->addWidget(addMetadataCheck_);
    auto indent = [&](QCheckBox* cb, int px) {
        auto* row = new QHBoxLayout();
        row->addSpacing(px);
        row->addWidget(cb);
        row->addStretch();
        metaLayout->addLayout(row);
    };
    indent(mathFormulaCheck_, 20);
    indent(noMathDataCheck_, 40);
    indent(layoutCheck_, 20);
    metadataGroup_->setVisible(offerMetadata);

    buttons_ = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto* buttons = buttons_;
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // The option groups scroll; OK / Cancel do not.
    //
    // Laid out directly, this dialog's MINIMUM height was ~970 px empty and
    // ~1110 px with a channel list — and Qt refuses to shrink a window below
    // its layout minimum, so on a 1080p screen it could not fit and the
    // buttons fell off the bottom with no way to reach them. Putting the
    // groups in a scroll area makes the minimum small, so the dialog resizes
    // freely and the buttons stay put whatever the screen.
    auto* content = new QWidget(this);
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->addWidget(formatBox);
    contentLayout->addWidget(rangeBox);
    contentLayout->addWidget(filtersBox);
    contentLayout->addWidget(metadataGroup_);
    contentLayout->addWidget(csvGroup_);
    contentLayout->addStretch();

    auto* scroll = new QScrollArea(this);
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* root = new QVBoxLayout(this);
    root->addWidget(scroll, /*stretch=*/1);
    root->addWidget(buttons);

    connect(h5Radio_,   &QRadioButton::toggled, this, [this](bool){ onFormatChanged(); });
    connect(mf4Radio_,  &QRadioButton::toggled, this, [this](bool){ onFormatChanged(); });
    connect(csvRadio_,  &QRadioButton::toggled, this, [this](bool){ onFormatChanged(); });
    connect(htmlRadio_, &QRadioButton::toggled, this, [this](bool){ onFormatChanged(); });
    connect(jsonRadio_, &QRadioButton::toggled, this, [this](bool){ onFormatChanged(); });
    connect(allRangeRadio_,    &QRadioButton::toggled, this, [this](bool){ onRangeModeChanged(); });
    connect(customRangeRadio_, &QRadioButton::toggled, this, [this](bool){ onRangeModeChanged(); });

    connect(timeModeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int){ onCsvTimeModeChanged(); });

    connect(colSepCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int i){ colSepCustom_->setVisible(i == kCustomColIdx); });
    connect(rowSepCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int i){ rowSepCustom_->setVisible(i == kCustomRowIdx); });

    for (auto* cb : {includeTimeCheck_, includeFreqCheck_, includeDerivedCheck_})
        connect(cb, &QCheckBox::toggled, this,
                [this](bool){ onChannelFiltersChanged(); });

    connect(addMetadataCheck_, &QCheckBox::toggled, this, [this](bool){ onMetadataChanged(); });
    connect(mathFormulaCheck_, &QCheckBox::toggled, this, [this](bool){ onMetadataChanged(); });

    onFormatChanged();
    onRangeModeChanged();
    onMetadataChanged();
    onCsvTimeModeChanged();

    // Open showing as much as fits: the content's natural size, capped to the
    // available screen so the dialog is never taller than the desktop it opens
    // on. Anything beyond that scrolls.
    const QRect avail = screen() ? screen()->availableGeometry()
                                 : QGuiApplication::primaryScreen()->availableGeometry();
    const QSize want = content->sizeHint() + QSize(28, buttons->sizeHint().height() + 28);
    resize(std::min(want.width(),  static_cast<int>(avail.width()  * 0.9)),
           std::min(want.height(), static_cast<int>(avail.height() * 0.9)));
}

void SaveChartDialog::onMetadataChanged() {
    const bool meta    = addMetadataCheck_->isChecked();
    const bool formula = meta && mathFormulaCheck_->isChecked();
    mathFormulaCheck_->setEnabled(meta);
    layoutCheck_->setEnabled(meta);
    noMathDataCheck_->setEnabled(formula);
}

bool SaveChartDialog::addMetadata() const {
    return addMetadataCheck_->isChecked();
}
bool SaveChartDialog::includeMathFormula() const {
    return addMetadata() && mathFormulaCheck_->isChecked();
}
bool SaveChartDialog::noDataForMathChannels() const {
    return includeMathFormula() && noMathDataCheck_->isChecked();
}
bool SaveChartDialog::includeLayout() const {
    return addMetadata() && layoutCheck_->isChecked();
}

void SaveChartDialog::setChannelFiltersVisible(bool on) {
    filtersGroup_->setVisible(on);
}

void SaveChartDialog::setMetadataLayoutOnly(bool layoutOnly) {
    if (!layoutOnly) return;
    // Keep the master checked (hidden) so includeLayout() stays honoured,
    // then hide everything in the group except the Layout checkbox.
    addMetadataCheck_->setChecked(true);
    addMetadataCheck_->setVisible(false);
    mathFormulaCheck_->setVisible(false);
    noMathDataCheck_->setVisible(false);
}

void SaveChartDialog::onFormatChanged() {
    csvGroup_->setEnabled(csvRadio_->isChecked());
    // HTML is always a single file with its own Time / Frequency / XY view
    // selector, so the split-files option doesn't apply.
    const bool html = htmlRadio_->isChecked();
    splitFilesCheck_->setEnabled(!html);
    if (html) splitFilesCheck_->setChecked(false);
    htmlViewOnlyCheck_->setEnabled(html);
    if (repeatWarning_) onCsvTimeModeChanged();
}

// The Shared time column has one row per distinct timestamp, so a channel
// with several samples at one instant loses all but the first — silently, in
// the file. Say so while that combination is selected, and point at the mode
// that keeps them.
void SaveChartDialog::onCsvTimeModeChanged() {
    const bool shared = timeModeCombo_->currentIndex() == 0;
    const bool show   = csvRadio_->isChecked() && shared && repeatScan_.any();
    repeatWarning_->setVisible(show);
    if (!show) return;

    const QString who = repeatScan_.channels == 1
        ? QString("\u201c%1\u201d repeats timestamps").arg(repeatScan_.worstChannel)
        : QString("%1 channels repeat timestamps").arg(repeatScan_.channels);
    const QString what = repeatScan_.droppedSamples == 1
        ? QString("1 sample would be dropped")
        : QString("%1 samples would be dropped").arg(repeatScan_.droppedSamples);
    scope::style::setPill(repeatWarning_, scope::style::PillTone::Warn,
                          QString("%1 — %2").arg(who, what));
    repeatWarning_->setToolTip(
        QString("A shared time column has one row per distinct timestamp, so\n"
                "where a channel holds several samples at the same instant only\n"
                "the first one fits — %1 across %2 would not be written.\n\n"
                "Choose \u201cPer-signal time columns (exact)\u201d to keep every\n"
                "sample, or save as HDF5 / MDF4 / HTML / JSON, which are all\n"
                "unaffected.")
            .arg(repeatScan_.droppedSamples == 1
                     ? QString("1 sample")
                     : QString("%1 samples").arg(repeatScan_.droppedSamples),
                 repeatScan_.channels == 1
                     ? QString("1 channel")
                     : QString("%1 channels").arg(repeatScan_.channels)));
}

void SaveChartDialog::setChannels(const scope::core::SignalStore& store) {
    channelList_->clear();

    // Show the domain suffix only when it disambiguates: a time-only store
    // (the common case) would just repeat "time" on every row.
    bool haveTime = false, haveFreq = false;
    QStringList names = store.channelNames();
    std::sort(names.begin(), names.end());
    for (const auto& n : names) {
        auto s = store.get(n);
        if (!s) continue;
        (s->meta().domain == scope::core::Signal::Domain::Frequency ? haveFreq
                                                                    : haveTime) = true;
    }

    const QSignalBlocker block(channelList_);   // one refresh at the end
    for (const auto& n : names) {
        auto s = store.get(n);
        if (!s) continue;
        const auto& meta = s->meta();
        const bool freq = meta.domain == scope::core::Signal::Domain::Frequency;
        QString label = meta.name;
        if (!meta.unit.isEmpty()) label += QString(" [%1]").arg(meta.unit);
        if (haveTime && haveFreq)
            label += freq ? QString("  · frequency") : QString("  · time");
        auto* item = new QListWidgetItem(label, channelList_);
        item->setData(Qt::UserRole, meta.name);
        item->setData(Qt::UserRole + 1, freq);
        item->setCheckState(Qt::Checked);   // saving everything stays the default
    }

    // Show every row up to a handful, then scroll — a long store must not
    // push OK off the bottom of the screen.
    if (channelList_->count() > 0) {
        const int rows = std::min(channelList_->count(), kMaxVisibleChannelRows);
        const int frame = 2 * channelList_->frameWidth();
        // Fixed, not maximum: a QListWidget's own size hint is tiny, so a
        // maximum alone still leaves it collapsed to a scrolling stub.
        channelList_->setFixedHeight(
            rows * channelList_->sizeHintForRow(0) + frame + 2);
    }

    const bool any = channelList_->count() > 0;
    channelList_->setVisible(any);
    channelButtons_->setVisible(any);
    onChannelFiltersChanged();
}

// A channel is writable only if its row is ticked AND the domain / derived
// toggles admit it. Rather than let those disagree silently, disable the
// rows the toggles rule out so the list always shows what will be written.
void SaveChartDialog::onChannelFiltersChanged() {
    if (!channelList_ || channelList_->count() == 0) return;

    int writable = 0;
    const QSignalBlocker block(channelList_);
    for (int i = 0; i < channelList_->count(); ++i) {
        auto* it = channelList_->item(i);
        const bool freq = it->data(Qt::UserRole + 1).toBool();
        const bool admitted = freq ? includeFreqCheck_->isChecked()
                                   : includeTimeCheck_->isChecked();
        auto flags = it->flags();
        it->setFlags(admitted ? (flags |  Qt::ItemIsEnabled)
                              : (flags & ~Qt::ItemIsEnabled));
        if (admitted && it->checkState() == Qt::Checked) ++writable;
    }
    channelCount_->setText(QString("%1 of %2 selected")
                               .arg(writable).arg(channelList_->count()));

    // An empty selection reaches ChartSaveFilters as "no restriction", i.e.
    // save everything — the opposite of what unticking every row means. Block
    // OK instead of writing the wrong file. Only when a picker exists, so
    // hosts without one keep their previous behaviour.
    if (buttons_)
        buttons_->button(QDialogButtonBox::Ok)->setEnabled(writable > 0);
}

QStringList SaveChartDialog::selectedChannels() const {
    QStringList out;
    if (!channelList_) return out;
    for (int i = 0; i < channelList_->count(); ++i) {
        const auto* it = channelList_->item(i);
        if (it->checkState() == Qt::Checked)
            out << it->data(Qt::UserRole).toString();
    }
    return out;
}

void SaveChartDialog::setRepeatedTimestamps(
    const scope::converter::RepeatedTimestampScan& scan) {
    repeatScan_ = scan;
    onCsvTimeModeChanged();
}

void SaveChartDialog::onRangeModeChanged() {
    const bool custom = customRangeRadio_->isChecked();
    fromSpin_->setEnabled(custom);
    toSpin_->setEnabled(custom);
}

SaveChartDialog::Format SaveChartDialog::format() const {
    if (csvRadio_->isChecked())  return Format::Csv;
    if (mf4Radio_->isChecked())  return Format::Mdf4;
    if (htmlRadio_->isChecked()) return Format::Html;
    if (jsonRadio_->isChecked()) return Format::Json;
    return Format::Hdf5;
}

// Guarded on the format so a stale tick can't strip the data island from a
// non-HTML save (where the flag is meaningless anyway).
bool SaveChartDialog::htmlViewOnly() const {
    return htmlRadio_->isChecked() && htmlViewOnlyCheck_->isChecked();
}

bool   SaveChartDialog::useCustomRange() const { return customRangeRadio_->isChecked(); }
double SaveChartDialog::fromSec()        const { return fromSpin_->value(); }
double SaveChartDialog::toSec()          const { return toSpin_->value(); }

bool SaveChartDialog::includeTimeDomain()        const { return includeTimeCheck_->isChecked(); }
bool SaveChartDialog::includeFrequencyDomain()   const { return includeFreqCheck_->isChecked(); }
bool SaveChartDialog::includeDerivedChannels()   const { return includeDerivedCheck_->isChecked(); }
bool SaveChartDialog::splitDomainsIntoTwoFiles() const { return splitFilesCheck_->isChecked(); }

scope::converter::CsvExportOptions SaveChartDialog::csvOptions() const {
    scope::converter::CsvExportOptions o;
    o.includeHeader = headerCheck_->isChecked();
    o.includeMetadata = metadataCheck_->isChecked();
    o.columnDelimiter = (colSepCombo_->currentIndex() == kCustomColIdx)
        ? (colSepCustom_->text().isEmpty() ? QString(",") : colSepCustom_->text())
        : colSepCombo_->currentData().toString();
    o.rowDelimiter = (rowSepCombo_->currentIndex() == kCustomRowIdx)
        ? (rowSepCustom_->text().isEmpty() ? QString("\n") : rowSepCustom_->text())
        : rowSepCombo_->currentData().toString();
    o.decimalSeparator = decimalEdit_->text().isEmpty() ? QString(".") : decimalEdit_->text();
    o.timeMode = (timeModeCombo_->currentIndex() == 1)
        ? scope::converter::CsvExportOptions::TimeMode::PerSignal
        : scope::converter::CsvExportOptions::TimeMode::Shared;
    o.timeAxis = (timeAxisCombo_->currentIndex() == 1)
        ? scope::converter::CsvExportOptions::TimeAxis::RelativeSeconds
        : scope::converter::CsvExportOptions::TimeAxis::EpochNs;
    return o;
}

}  // namespace scope::converter::ui
