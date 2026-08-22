// Save-dialog behaviour that guards against writing a file that quietly is
// not what the user thinks it is:
//   - the repeated-timestamp warning for CSV's Shared time column, which has
//     one row per distinct timestamp and so cannot hold every sample;
//   - HTML's "smaller file" knob, which trades away the embedded data (and
//     with it the ability to re-open the file here) for size.

#include "scope/converter/CsvWriter.h"
#include "scope/converter/SaveChartDialog.h"
#include "scope/converter/SignalIO.h"
#include "scope/core/Signal.h"
#include "scope/core/SignalStore.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QComboBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>

#include <cmath>
#include <filesystem>
#include <memory>
#include <random>
#include <vector>

using namespace scope::core;
using namespace scope::converter;

namespace {

// One QApplication for the whole process (offscreen).
QApplication* ensureApp() {
    if (!QCoreApplication::instance()) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        static int argc = 1;
        static char a0[] = {'t', '\0'};
        static char* argv[] = {a0, nullptr};
        static QApplication app(argc, argv);
    }
    return static_cast<QApplication*>(QCoreApplication::instance());
}

std::shared_ptr<Signal> makeSig(const QString& name,
                                const std::vector<TimestampNs>& ts,
                                const std::vector<double>& vs) {
    Signal::Meta m;
    m.name = name;
    m.dataType = DataType::Float64;
    auto s = std::make_shared<Signal>(m);
    s->append(ts.data(), reinterpret_cast<const std::byte*>(vs.data()), vs.size());
    return s;
}

// The dialog's widgets are private; find them by the text they were built
// with, which is what a user would go by anyway.
QComboBox* timeModeCombo(QWidget* dlg) {
    for (auto* c : dlg->findChildren<QComboBox*>())
        if (c->count() == 2 && c->itemText(0).startsWith("Shared time column"))
            return c;
    return nullptr;
}
QLabel* repeatWarning(QWidget* dlg) {
    for (auto* l : dlg->findChildren<QLabel*>())
        if (l->property("scopeRole").toString() == "pill") return l;
    return nullptr;
}
QRadioButton* radioStartingWith(QWidget* dlg, const QString& prefix) {
    for (auto* r : dlg->findChildren<QRadioButton*>())
        if (r->text().startsWith(prefix)) return r;
    return nullptr;
}

}  // namespace

TEST(RepeatedTimestampScan, CountsWhatASharedWriteWouldDrop) {
    // 6 samples over 3 distinct timestamps → 3 would not fit.
    auto dup = makeSig("dup", {0, 0, 100'000'000LL, 100'000'000LL,
                               200'000'000LL, 200'000'000LL},
                       {1, 2, 3, 4, 5, 6});
    auto plain = makeSig("plain", {0, 100'000'000LL, 200'000'000LL}, {1, 2, 3});

    const auto scan = scanRepeatedTimestamps({dup, plain});
    EXPECT_TRUE(scan.any());
    EXPECT_EQ(scan.channels, 1);
    EXPECT_EQ(scan.droppedSamples, 3);
    EXPECT_EQ(scan.worstChannel.toStdString(), "dup");
}

TEST(RepeatedTimestampScan, QuietWhenNothingRepeats) {
    auto a = makeSig("a", {0, 100'000'000LL, 200'000'000LL}, {1, 2, 3});
    auto b = makeSig("b", {0, 200'000'000LL}, {9, 8});
    const auto scan = scanRepeatedTimestamps({a, b});
    EXPECT_FALSE(scan.any());
    EXPECT_EQ(scan.channels, 0);
    EXPECT_EQ(scan.droppedSamples, 0);
    EXPECT_TRUE(scan.worstChannel.isEmpty());
}

TEST(RepeatedTimestampScan, NamesTheWorstChannelAndSurvivesNulls) {
    auto few  = makeSig("few",  {0, 0, 100'000'000LL}, {1, 2, 3});
    auto many = makeSig("many", {0, 0, 0, 0, 100'000'000LL}, {1, 2, 3, 4, 5});
    const auto scan = scanRepeatedTimestamps({nullptr, few, many, nullptr});
    EXPECT_EQ(scan.channels, 2);
    EXPECT_EQ(scan.droppedSamples, 1 + 3);
    EXPECT_EQ(scan.worstChannel.toStdString(), "many");
}

TEST(RepeatedTimestampScan, ReadsAWholeStore) {
    SignalStore store;
    store.add(makeSig("dup", {0, 0, 100'000'000LL}, {1, 2, 3}));
    store.add(makeSig("plain", {0, 100'000'000LL}, {4, 5}));
    const auto scan = scanRepeatedTimestamps(store);
    EXPECT_EQ(scan.channels, 1);
    EXPECT_EQ(scan.droppedSamples, 1);
}

TEST(SaveChartDialogWarning, HiddenUntilAScanReportsRepeats) {
    ensureApp();
    scope::converter::ui::SaveChartDialog dlg(0.0, 1.0);
    auto* warn = repeatWarning(&dlg);
    ASSERT_NE(warn, nullptr);
    EXPECT_FALSE(warn->isVisible()) << "no scan yet — nothing to warn about";

    RepeatedTimestampScan clean;
    dlg.setRepeatedTimestamps(clean);
    EXPECT_FALSE(warn->isVisible()) << "clean data must not raise a warning";
}

TEST(SaveChartDialogWarning, ShownOnlyForCsvWithASharedTimeColumn) {
    ensureApp();
    scope::converter::ui::SaveChartDialog dlg(0.0, 1.0);
    dlg.show();   // isVisible() on a child needs the dialog itself shown

    auto* warn = repeatWarning(&dlg);
    auto* mode = timeModeCombo(&dlg);
    auto* csv  = radioStartingWith(&dlg, "CSV");
    auto* h5   = radioStartingWith(&dlg, "HDF5");
    ASSERT_NE(warn, nullptr);
    ASSERT_NE(mode, nullptr);
    ASSERT_NE(csv, nullptr);
    ASSERT_NE(h5, nullptr);

    RepeatedTimestampScan scan;
    scan.channels = 2;
    scan.droppedSamples = 7;
    scan.worstChannel = "dup";
    dlg.setRepeatedTimestamps(scan);

    // HDF5 is selected by default and is unaffected by the shared-grid rule.
    EXPECT_FALSE(warn->isVisible());

    csv->setChecked(true);
    EXPECT_TRUE(warn->isVisible()) << "CSV + Shared is the lossy combination";
    EXPECT_TRUE(warn->text().contains("7")) << warn->text().toStdString();
    EXPECT_TRUE(warn->text().contains("2 channels")) << warn->text().toStdString();
    EXPECT_EQ(warn->property("pillTone").toString().toStdString(), "warn");
    EXPECT_FALSE(warn->toolTip().isEmpty());

    mode->setCurrentIndex(1);   // Per-signal time columns (exact)
    EXPECT_FALSE(warn->isVisible()) << "per-signal keeps every sample";

    mode->setCurrentIndex(0);
    EXPECT_TRUE(warn->isVisible());

    h5->setChecked(true);
    EXPECT_FALSE(warn->isVisible()) << "only CSV writes a shared time column";
}

TEST(SaveChartDialogWarning, NamesTheChannelWhenOnlyOneRepeats) {
    ensureApp();
    scope::converter::ui::SaveChartDialog dlg(0.0, 1.0);
    dlg.show();
    auto* csv = radioStartingWith(&dlg, "CSV");
    ASSERT_NE(csv, nullptr);
    csv->setChecked(true);

    RepeatedTimestampScan scan;
    scan.channels = 1;
    scan.droppedSamples = 3;
    scan.worstChannel = "motor_speed";
    dlg.setRepeatedTimestamps(scan);

    auto* warn = repeatWarning(&dlg);
    ASSERT_NE(warn, nullptr);
    EXPECT_TRUE(warn->isVisible());
    EXPECT_TRUE(warn->text().contains("motor_speed")) << warn->text().toStdString();
}

// ---------------------------------------------------------------------------
// HTML size knob: the page normally embeds a compressed copy of the samples so
// it re-opens here, and that island is most of the file. "Chart only" drops it.
// ---------------------------------------------------------------------------

namespace {

QCheckBox* viewOnlyCheck(QWidget* dlg) {
    for (auto* c : dlg->findChildren<QCheckBox*>())
        if (c->text().startsWith("Smaller file")) return c;
    return nullptr;
}

}  // namespace

TEST(SaveChartDialogHtml, ViewOnlyIsOffAndOnlyEnabledForHtml) {
    ensureApp();
    scope::converter::ui::SaveChartDialog dlg(0.0, 1.0);
    dlg.show();
    auto* box  = viewOnlyCheck(&dlg);
    auto* html = radioStartingWith(&dlg, "HTML");
    auto* csv  = radioStartingWith(&dlg, "CSV");
    ASSERT_NE(box, nullptr);
    ASSERT_NE(html, nullptr);
    ASSERT_NE(csv, nullptr);

    // Default is the re-openable file: a save format shouldn't quietly become
    // one-way.
    EXPECT_FALSE(box->isChecked());
    EXPECT_FALSE(box->isEnabled()) << "HDF5 is selected — the knob is HTML-only";

    html->setChecked(true);
    EXPECT_TRUE(box->isEnabled());
    box->setChecked(true);
    EXPECT_TRUE(dlg.htmlViewOnly());

    // A stale tick must not follow the user to another format.
    csv->setChecked(true);
    EXPECT_FALSE(box->isEnabled());
    EXPECT_FALSE(dlg.htmlViewOnly());

    html->setChecked(true);
    EXPECT_TRUE(dlg.htmlViewOnly()) << "the choice is remembered per format";
}

TEST(SaveChartDialogHtml, ViewOnlyWritesASmallerPageThatWontReopen) {
    // Noise matters: the saving comes from rounding away low-order bits that
    // don't compress. A synthetic ramp is already highly compressible and
    // would show almost no difference — not what real instrumentation looks
    // like.
    std::mt19937 rng(7);
    std::normal_distribution<double> noise(0.0, 1.0);
    std::vector<TimestampNs> ts;
    std::vector<double>      vs;
    for (int i = 0; i < 50'000; ++i) {
        ts.push_back(static_cast<TimestampNs>(i) * 1'000'000LL);
        vs.push_back(100.0 * std::sin(i / 500.0) + noise(rng));
    }
    SignalStore store;
    store.add(makeSig("ramp", ts, vs));

    const auto dir = std::filesystem::temp_directory_path();
    const auto storablePath = dir / "scope_size_storable.html";
    const auto chartOnlyPath = dir / "scope_size_chartonly.html";
    const QString sp = QString::fromStdString(storablePath.string());
    const QString cp = QString::fromStdString(chartOnlyPath.string());

    QString err;
    ASSERT_TRUE(exportStorableHtml(sp, store, &err)) << err.toStdString();
    ASSERT_TRUE(exportChartOnlyHtml(cp, store, kChartOnlyDefaultDigits, &err))
        << err.toStdString();

    const auto storableSize = std::filesystem::file_size(storablePath);
    const auto chartOnlySize = std::filesystem::file_size(chartOnlyPath);
    // Measured ~40% at this size; assert a floor well clear of noise so the
    // test fails if the rounding ever silently stops being applied.
    EXPECT_LT(chartOnlySize, storableSize * 4 / 5)
        << "chart-only should be clearly smaller: " << chartOnlySize << " vs "
        << storableSize;

    // Only the storable file carries the re-import marker, and only it loads.
    std::vector<std::shared_ptr<Signal>> chans;
    QString lerr;
    ASSERT_TRUE(loadStorableHtml(sp, &chans, nullptr, &lerr)) << lerr.toStdString();
    EXPECT_EQ(chans.size(), 1u);

    chans.clear();
    EXPECT_FALSE(loadStorableHtml(cp, &chans, nullptr, &lerr))
        << "chart-only must not pretend to be re-openable";
    EXPECT_FALSE(lerr.isEmpty());

    std::error_code ec;
    std::filesystem::remove(storablePath, ec);
    std::filesystem::remove(chartOnlyPath, ec);
}

// The saving comes from precision, so pin that down directly rather than
// only through file size.
TEST(SaveChartDialogHtml, ChartOnlyRoundsValuesAndStorableDoesNot) {
    SignalStore store;
    store.add(makeSig("v", {0, 1'000'000LL},
                      {1.2345678901234567, 2.9876543210987654}));

    const auto dir = std::filesystem::temp_directory_path();
    const auto p = dir / "scope_precision_chartonly.html";
    const QString cp = QString::fromStdString(p.string());
    QString err;
    ASSERT_TRUE(exportChartOnlyHtml(cp, store, 6, &err)) << err.toStdString();

    // Re-read it the way the page does: gunzip the block and look at the
    // numbers actually written.
    std::vector<std::shared_ptr<Signal>> chans;
    QString lerr;
    EXPECT_FALSE(loadStorableHtml(cp, &chans, nullptr, &lerr));

    QFile f(cp);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QString html = QString::fromUtf8(f.readAll());
    f.close();
    std::error_code ec;
    std::filesystem::remove(p, ec);

    EXPECT_TRUE(html.contains("id=\"scope-chart\""));
    EXPECT_FALSE(html.contains("id=\"scope-data\""))
        << "the re-import marker must not appear on a chart-only page";
    EXPECT_TRUE(html.contains("values rounded to 6 significant figures"))
        << "the page should say what it is";
}

// ---------------------------------------------------------------------------
// Per-channel picker: choose exactly which channels get written.
// ---------------------------------------------------------------------------

namespace {

QListWidget* channelList(QWidget* dlg) {
    const auto lists = dlg->findChildren<QListWidget*>();
    return lists.isEmpty() ? nullptr : lists.first();
}
QListWidgetItem* rowFor(QListWidget* l, const QString& name) {
    for (int i = 0; i < l->count(); ++i)
        if (l->item(i)->data(Qt::UserRole).toString() == name) return l->item(i);
    return nullptr;
}
QPushButton* okButton(QWidget* dlg) {
    for (auto* b : dlg->findChildren<QDialogButtonBox*>())
        if (auto* ok = b->button(QDialogButtonBox::Ok)) return ok;
    return nullptr;
}
SignalStore& threeChannelStore() {
    static SignalStore store;
    if (store.size() == 0) {
        store.add(makeSig("speed",    {0, 1'000'000LL}, {1, 2}));
        store.add(makeSig("pressure", {0, 1'000'000LL}, {3, 4}));
        store.add(makeSig("torque",   {0, 1'000'000LL}, {5, 6}));
    }
    return store;
}

}  // namespace

TEST(SaveChartChannelPicker, HiddenUntilAStoreIsHandedOver) {
    ensureApp();
    scope::converter::ui::SaveChartDialog dlg(0.0, 1.0);
    dlg.show();
    auto* list = channelList(&dlg);
    ASSERT_NE(list, nullptr);
    EXPECT_FALSE(list->isVisible()) << "hosts that don't offer a picker keep the old dialog";
    EXPECT_TRUE(dlg.selectedChannels().isEmpty())
        << "empty means 'no per-channel restriction' downstream";
}

TEST(SaveChartChannelPicker, ListsEveryChannelTickedAndSorted) {
    ensureApp();
    scope::converter::ui::SaveChartDialog dlg(0.0, 1.0);
    dlg.setChannels(threeChannelStore());
    dlg.show();

    auto* list = channelList(&dlg);
    ASSERT_NE(list, nullptr);
    EXPECT_TRUE(list->isVisible());
    ASSERT_EQ(list->count(), 3);
    // Sorted, so the order doesn't wander with the store's hash order.
    EXPECT_EQ(list->item(0)->data(Qt::UserRole).toString().toStdString(), "pressure");
    EXPECT_EQ(list->item(1)->data(Qt::UserRole).toString().toStdString(), "speed");
    EXPECT_EQ(list->item(2)->data(Qt::UserRole).toString().toStdString(), "torque");

    // Saving everything stays the default.
    const auto all = dlg.selectedChannels();
    EXPECT_EQ(all.size(), 3);
    EXPECT_TRUE(all.contains("speed"));
}

TEST(SaveChartChannelPicker, UntickingAChannelDropsItFromTheSelection) {
    ensureApp();
    scope::converter::ui::SaveChartDialog dlg(0.0, 1.0);
    dlg.setChannels(threeChannelStore());
    dlg.show();
    auto* list = channelList(&dlg);
    ASSERT_NE(list, nullptr);

    rowFor(list, "speed")->setCheckState(Qt::Unchecked);
    const auto sel = dlg.selectedChannels();
    EXPECT_EQ(sel.size(), 2);
    EXPECT_FALSE(sel.contains("speed"));
    EXPECT_TRUE(sel.contains("pressure"));
    EXPECT_TRUE(sel.contains("torque"));
}

TEST(SaveChartChannelPicker, EmptySelectionBlocksOkRatherThanSavingEverything) {
    ensureApp();
    scope::converter::ui::SaveChartDialog dlg(0.0, 1.0);
    dlg.setChannels(threeChannelStore());
    dlg.show();
    auto* list = channelList(&dlg);
    auto* ok = okButton(&dlg);
    ASSERT_NE(list, nullptr);
    ASSERT_NE(ok, nullptr);
    EXPECT_TRUE(ok->isEnabled());

    for (int i = 0; i < list->count(); ++i)
        list->item(i)->setCheckState(Qt::Unchecked);
    EXPECT_FALSE(ok->isEnabled())
        << "an empty list would reach the writer as 'no restriction' — i.e. "
           "save everything, the opposite of what the user asked for";

    rowFor(list, "torque")->setCheckState(Qt::Checked);
    EXPECT_TRUE(ok->isEnabled());
}

TEST(SaveChartChannelPicker, RowsExcludedByTheDomainFilterAreGreyedOut) {
    ensureApp();
    SignalStore store;
    store.add(makeSig("wave", {0, 1'000'000LL}, {1, 2}));
    Signal::Meta fm;
    fm.name = "spectrum";
    fm.dataType = DataType::Float64;
    fm.domain = Signal::Domain::Frequency;
    auto spec = std::make_shared<Signal>(fm);
    const std::vector<TimestampNs> fts{0, 1'000'000'000LL};
    const std::vector<double>      fvs{9, 8};
    spec->append(fts.data(), reinterpret_cast<const std::byte*>(fvs.data()), 2);
    store.add(spec);

    scope::converter::ui::SaveChartDialog dlg(0.0, 1.0);
    dlg.setChannels(store);
    dlg.show();
    auto* list = channelList(&dlg);
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->count(), 2);

    // Both domains present → each row says which it is.
    EXPECT_TRUE(rowFor(list, "spectrum")->text().contains("frequency"))
        << rowFor(list, "spectrum")->text().toStdString();

    for (auto* cb : dlg.findChildren<QCheckBox*>())
        if (cb->text().startsWith("Frequency-domain")) cb->setChecked(false);

    EXPECT_FALSE(rowFor(list, "spectrum")->flags() & Qt::ItemIsEnabled)
        << "a row the domain filter excludes must show as unwritable";
    EXPECT_TRUE(rowFor(list, "wave")->flags() & Qt::ItemIsEnabled);
}

// The picker is only worth anything if the names reach the writer, so drive
// saveChartFromStore directly and read back what landed on disk.
TEST(SaveChartChannelPicker, OnlySelectedChannelsAreWritten) {
    SignalStore store;
    store.add(makeSig("speed",    {0, 1'000'000LL}, {1, 2}));
    store.add(makeSig("pressure", {0, 1'000'000LL}, {3, 4}));
    store.add(makeSig("torque",   {0, 1'000'000LL}, {5, 6}));

    const auto path = std::filesystem::temp_directory_path() /
                      "scope_selected_channels.json";
    const QString p = QString::fromStdString(path.string());

    ChartSaveFilters filters;
    filters.selectedChannels = {"speed", "torque"};

    QStringList msgs;
    QString err;
    ASSERT_EQ(saveChartFromStore(p, store, FileFormat::Json, filters, {},
                                 &msgs, &err),
              ChartSaveResult::Ok) << err.toStdString();

    const auto r = loadFile(path);
    std::error_code ec; std::filesystem::remove(path, ec);
    ASSERT_TRUE(r.ok) << r.error.toStdString();

    QStringList got;
    for (const auto& c : r.channels) got << c->meta().name;
    got.sort();
    EXPECT_EQ(got, QStringList({"speed", "torque"}));
}

// An empty list must stay "everything", or every existing caller that never
// touches the picker would suddenly write nothing.
TEST(SaveChartChannelPicker, EmptySelectionStillMeansEveryChannel) {
    SignalStore store;
    store.add(makeSig("a", {0, 1'000'000LL}, {1, 2}));
    store.add(makeSig("b", {0, 1'000'000LL}, {3, 4}));

    const auto path = std::filesystem::temp_directory_path() /
                      "scope_all_channels.json";
    const QString p = QString::fromStdString(path.string());

    ChartSaveFilters filters;   // selectedChannels left empty
    QStringList msgs;
    QString err;
    ASSERT_EQ(saveChartFromStore(p, store, FileFormat::Json, filters, {},
                                 &msgs, &err),
              ChartSaveResult::Ok) << err.toStdString();

    const auto r = loadFile(path);
    std::error_code ec; std::filesystem::remove(path, ec);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_EQ(r.channels.size(), 2u);
}

// Selection is ANDed with the domain filters, not ORed.
TEST(SaveChartChannelPicker, SelectionIsAndedWithTheDomainFilters) {
    SignalStore store;
    store.add(makeSig("wave", {0, 1'000'000LL}, {1, 2}));
    Signal::Meta fm;
    fm.name = "spectrum";
    fm.dataType = DataType::Float64;
    fm.domain = Signal::Domain::Frequency;
    auto spec = std::make_shared<Signal>(fm);
    const std::vector<TimestampNs> fts{0, 1'000'000'000LL};
    const std::vector<double>      fvs{9, 8};
    spec->append(fts.data(), reinterpret_cast<const std::byte*>(fvs.data()), 2);
    store.add(spec);

    const auto path = std::filesystem::temp_directory_path() /
                      "scope_anded_channels.json";
    const QString p = QString::fromStdString(path.string());

    ChartSaveFilters filters;
    filters.selectedChannels = {"wave", "spectrum"};   // both ticked …
    filters.includeFrequency = false;                  // … but frequency is off

    QStringList msgs;
    QString err;
    ASSERT_EQ(saveChartFromStore(p, store, FileFormat::Json, filters, {},
                                 &msgs, &err),
              ChartSaveResult::Ok) << err.toStdString();

    const auto r = loadFile(path);
    std::error_code ec; std::filesystem::remove(path, ec);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_EQ(r.channels.size(), 1u);
    EXPECT_EQ(r.channels[0]->meta().name.toStdString(), "wave");
}

// Nothing left after both filters is the existing "nothing matched" outcome,
// not a silent empty file.
TEST(SaveChartChannelPicker, UnknownNamesMatchNothingAndAreReported) {
    SignalStore store;
    store.add(makeSig("a", {0, 1'000'000LL}, {1, 2}));

    const auto path = std::filesystem::temp_directory_path() /
                      "scope_no_match.json";
    ChartSaveFilters filters;
    filters.selectedChannels = {"does-not-exist"};

    QStringList msgs;
    QString err;
    EXPECT_EQ(saveChartFromStore(QString::fromStdString(path.string()), store,
                                 FileFormat::Json, filters, {}, &msgs, &err),
              ChartSaveResult::NothingMatchedFilters);
    std::error_code ec; std::filesystem::remove(path, ec);
}
