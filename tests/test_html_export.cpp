// exportInteractiveHtml: a self-contained interactive chart file. These
// tests check the file is genuinely standalone (uPlot embedded), carries
// the channel data at full precision, splits domains into two charts,
// aligns differing grids on their union with nulls, and turns NaN into
// gaps instead of fabricated numbers.

#include "scope/converter/HtmlExport.h"
#include "scope/converter/SignalIO.h"
#include "scope/core/Signal.h"
#include "scope/core/SignalStore.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QString>

#include <cmath>
#include <filesystem>
#include <memory>
#include <vector>

using namespace scope::core;
using namespace scope::converter;

namespace {

std::shared_ptr<Signal> makeSig(const QString& name, const QString& unit,
                                Signal::Domain domain, TimestampNs startNs,
                                TimestampNs dtNs, const std::vector<double>& vs) {
    Signal::Meta m;
    m.name = name;
    m.unit = unit;
    m.dataType = DataType::Float64;
    m.domain = domain;
    std::vector<TimestampNs> ts(vs.size());
    for (std::size_t i = 0; i < vs.size(); ++i)
        ts[i] = startNs + static_cast<TimestampNs>(i) * dtNs;
    auto s = std::make_shared<Signal>(m);
    s->append(ts.data(), reinterpret_cast<const std::byte*>(vs.data()), vs.size());
    return s;
}

QString exportToString(const SignalStore& store, bool* okOut = nullptr,
                       QString* errOut = nullptr) {
    const auto path = std::filesystem::temp_directory_path() / "scope_html_export_test.html";
    QString err;
    const bool ok = exportInteractiveHtml(QString::fromStdString(path.string()),
                                          store, &err);
    if (okOut) *okOut = ok;
    if (errOut) *errOut = err;
    QString content;
    {
        // Scoped so the handle is closed before remove() — Windows refuses
        // to delete a file that is still open.
        QFile f(QString::fromStdString(path.string()));
        if (f.open(QIODevice::ReadOnly)) content = QString::fromUtf8(f.readAll());
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return content;
}

}  // namespace

TEST(HtmlExport, SelfContainedWithEmbeddedLibraryAndData) {
    SignalStore store;
    store.add(makeSig("speed", "rpm", Signal::Domain::Time,
                      5'000'000'000LL, 100'000'000LL, {1.5, 2.5, 3.5}));
    bool ok = false;
    QString err;
    const QString html = exportToString(store, &ok, &err);
    ASSERT_TRUE(ok) << err.toStdString();
    EXPECT_GT(html.size(), 50'000) << "uPlot library not embedded";
    EXPECT_TRUE(html.contains("uPlot"));
    EXPECT_TRUE(html.contains("speed [rpm]"));
    EXPECT_TRUE(html.contains("\"t [s]\""));
    // No external requests — must work offline.
    EXPECT_FALSE(html.contains("http://"));
    EXPECT_FALSE(html.contains("https://cdn"));
    // x is relative seconds from the signal's origin; values full precision.
    EXPECT_TRUE(html.contains("[0,0.1,0.2]"));
    EXPECT_TRUE(html.contains("1.5,2.5,3.5"));
}

TEST(HtmlExport, SplitsTimeAndFrequencyIntoTwoCharts) {
    SignalStore store;
    store.add(makeSig("sig", "", Signal::Domain::Time,
                      0, 1'000'000LL, {1, 2, 3}));
    store.add(makeSig("spec", "mag", Signal::Domain::Frequency,
                      0, 1'000'000'000LL, {9, 8, 7}));
    bool ok = false;
    const QString html = exportToString(store, &ok);
    ASSERT_TRUE(ok);
    EXPECT_TRUE(html.contains("time:{xLabel:\"t [s]\""));
    EXPECT_TRUE(html.contains("frequency:{xLabel:\"f [Hz]\""));
    EXPECT_TRUE(html.contains("[0,1,2],[9,8,7]"));   // Hz axis + magnitudes
    // The View selector machinery is present.
    EXPECT_TRUE(html.contains("XY  (channel vs channel)"));
}

TEST(HtmlExport, UnionGridFillsMissingSamplesWithNull) {
    SignalStore store;
    // 10 Hz and 5 Hz: the 5 Hz channel has no sample on the odd grid points.
    store.add(makeSig("fast", "", Signal::Domain::Time,
                      0, 100'000'000LL, {1, 2, 3, 4, 5}));
    store.add(makeSig("slow", "", Signal::Domain::Time,
                      0, 200'000'000LL, {10, 20, 30}));
    bool ok = false;
    const QString html = exportToString(store, &ok);
    ASSERT_TRUE(ok);
    EXPECT_TRUE(html.contains("[10,null,20,null,30]"))
        << "slow channel must be null where it has no sample";
}

TEST(HtmlExport, NanBecomesNullNeverAFabricatedNumber) {
    SignalStore store;
    store.add(makeSig("gappy", "", Signal::Domain::Time, 0, 1'000'000LL,
                      {1.0, std::nan(""), 3.0}));
    bool ok = false;
    const QString html = exportToString(store, &ok);
    ASSERT_TRUE(ok);
    EXPECT_TRUE(html.contains("[1,null,3]"));
}

TEST(HtmlExport, EmptyStoreFailsWithMessage) {
    SignalStore store;
    bool ok = true;
    QString err;
    exportToString(store, &ok, &err);
    EXPECT_FALSE(ok);
    EXPECT_FALSE(err.isEmpty());
}

// The view parameter controls everything the Analyser mirrors into the
// page: axis labels/sides/colours, channel order, axis assignment, initial
// visibility, and trace colours.
TEST(HtmlExport, ViewControlsAxesAssignmentVisibilityAndColors) {
    SignalStore store;
    store.add(makeSig("a", "", Signal::Domain::Time, 0, 1'000'000LL, {1, 2}));
    store.add(makeSig("b", "", Signal::Domain::Time, 0, 1'000'000LL, {3, 4}));

    HtmlExportView view;
    HtmlAxis y1;  y1.label = "Y1";  y1.right = false; y1.color = "#1e1e1e";
    HtmlAxis y2;  y2.label = "bar"; y2.right = true;  y2.color = "#d95319";
    view.time.axes = {y1, y2};
    HtmlChannel ca; ca.name = "a"; ca.axisIndex = 0; ca.visible = true;  ca.color = "#112233";
    HtmlChannel cb; cb.name = "b"; cb.axisIndex = 1; cb.visible = false; cb.color = "#445566";
    view.time.channels = {ca, cb};

    const auto path = std::filesystem::temp_directory_path() / "scope_html_view_test.html";
    QString err;
    ASSERT_TRUE(exportInteractiveHtml(QString::fromStdString(path.string()),
                                      store, view, &err))
        << err.toStdString();
    QString html;
    {
        QFile f(QString::fromStdString(path.string()));
        ASSERT_TRUE(f.open(QIODevice::ReadOnly));
        html = QString::fromUtf8(f.readAll());
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);

    EXPECT_TRUE(html.contains(
        "axes:[{label:\"Y1\",right:false,color:\"#1e1e1e\"},"
        "{label:\"bar\",right:true,color:\"#d95319\"}]"));
    EXPECT_TRUE(html.contains(
        "{name:\"a\",label:\"a\",color:\"#112233\",axis:0,visible:true}"));
    EXPECT_TRUE(html.contains(
        "{name:\"b\",label:\"b\",color:\"#445566\",axis:1,visible:false}"));
    // The toolbar/measure/mode machinery is present.
    EXPECT_TRUE(html.contains("Δ Measure"));
    EXPECT_TRUE(html.contains("Line + points"));
    EXPECT_TRUE(html.contains("CHANNELS"));
}

// The page opens in the view the Analyser was showing, including the XY
// X channel.
TEST(HtmlExport, InitialViewAndXyChannelAreEmbedded) {
    SignalStore store;
    store.add(makeSig("a", "", Signal::Domain::Time, 0, 1'000'000LL, {1, 2}));
    store.add(makeSig("b", "", Signal::Domain::Time, 0, 1'000'000LL, {3, 4}));

    HtmlExportView view;
    HtmlAxis y1; y1.label = "Y1"; y1.color = "#1e1e1e";
    view.time.axes = {y1};
    HtmlChannel ca; ca.name = "a"; ca.color = "#112233";
    HtmlChannel cb; cb.name = "b"; cb.color = "#445566";
    view.time.channels = {ca, cb};
    view.initialView = "xy";
    view.xyChannel = "a";

    const auto path = std::filesystem::temp_directory_path() / "scope_html_xy_test.html";
    QString err;
    ASSERT_TRUE(exportInteractiveHtml(QString::fromStdString(path.string()),
                                      store, view, &err))
        << err.toStdString();
    QString html;
    {
        QFile f(QString::fromStdString(path.string()));
        ASSERT_TRUE(f.open(QIODevice::ReadOnly));
        html = QString::fromUtf8(f.readAll());
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);

    EXPECT_TRUE(html.contains("makeApp({view:\"xy\",xyChannel:\"a\""));
    EXPECT_TRUE(html.contains("function pairXY"));
}

// ---------------------------------------------------------------------------
// Storable HTML: the data is written ONCE (a JSON island) and re-imported, so
// HTML is a peer of .h5 / .mf4 / .csv — saveable and openable. The original
// exportInteractiveHtml() above is kept as the visualisation-only path.
// ---------------------------------------------------------------------------

namespace {

QString writeStorable(const SignalStore& store, const HtmlExportView* view,
                      const QString& tag, bool* okOut, QString* errOut) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("scope_storable_" + tag.toStdString() + ".html");
    const QString p = QString::fromStdString(path.string());
    QString err;
    const bool ok = view ? exportStorableHtml(p, store, *view, &err)
                         : exportStorableHtml(p, store, &err);
    if (okOut) *okOut = ok;
    if (errOut) *errOut = err;
    return p;
}

std::shared_ptr<Signal> byName(
    const std::vector<std::shared_ptr<Signal>>& v, const QString& name) {
    for (const auto& s : v) if (s && s->meta().name == name) return s;
    return nullptr;
}

}  // namespace

TEST(StorableHtml, RoundTripsChannelsValuesUnitsAndDomains) {
    SignalStore store;
    store.add(makeSig("speed", "rpm", Signal::Domain::Time,
                      5'000'000'000LL, 100'000'000LL, {1.5, 2.5, 3.5}));
    store.add(makeSig("mag", "dB", Signal::Domain::Frequency,
                      0, 1'000'000'000LL, {9.0, 8.0, 7.0}));

    bool ok = false;
    QString err;
    const QString p = writeStorable(store, nullptr, "rt", &ok, &err);
    ASSERT_TRUE(ok) << err.toStdString();

    std::vector<std::shared_ptr<Signal>> chans;
    QString layout, lerr;
    ASSERT_TRUE(loadStorableHtml(p, &chans, &layout, &lerr)) << lerr.toStdString();
    std::error_code ec; std::filesystem::remove(p.toStdString(), ec);

    ASSERT_EQ(chans.size(), 2u);

    auto speed = byName(chans, "speed");
    ASSERT_TRUE(speed != nullptr);
    EXPECT_EQ(speed->meta().unit.toStdString(), "rpm");
    EXPECT_EQ(static_cast<int>(speed->meta().domain),
              static_cast<int>(Signal::Domain::Time));
    ASSERT_EQ(speed->sampleCount(), 3u);
    {
        auto rv = speed->snapshotForRead();
        auto vs = speed->readAsDouble();
        EXPECT_EQ(rv.timestamps[0], 5'000'000'000LL);   // exact int64 ns
        EXPECT_EQ(rv.timestamps[1], 5'100'000'000LL);
        EXPECT_EQ(rv.timestamps[2], 5'200'000'000LL);
        EXPECT_DOUBLE_EQ(vs[0], 1.5);
        EXPECT_DOUBLE_EQ(vs[2], 3.5);
    }

    auto mag = byName(chans, "mag");
    ASSERT_TRUE(mag != nullptr);
    EXPECT_EQ(mag->meta().unit.toStdString(), "dB");
    EXPECT_EQ(static_cast<int>(mag->meta().domain),
              static_cast<int>(Signal::Domain::Frequency));
    {
        auto rv = mag->snapshotForRead();
        EXPECT_EQ(rv.timestamps[1], 1'000'000'000LL);
    }
}

TEST(StorableHtml, CompressesIslandStaysOfflineAndStillRoundTrips) {
    SignalStore store;
    store.add(makeSig("a", "V", Signal::Domain::Time, 0, 1'000'000LL,
                      {11.5, 22.5, 33.5}));
    bool ok = false;
    QString err;
    const QString p = writeStorable(store, nullptr, "compress", &ok, &err);
    ASSERT_TRUE(ok) << err.toStdString();

    QString html;
    { QFile f(p); ASSERT_TRUE(f.open(QIODevice::ReadOnly));
      html = QString::fromUtf8(f.readAll()); }

    EXPECT_TRUE(html.contains("id=\"scope-data\""));            // the single island
    EXPECT_TRUE(html.contains("data-encoding=\"gzip+base64\"")); // …compressed
    EXPECT_TRUE(html.contains("gunzipSync"));                   // fflate decoder embedded
    EXPECT_GT(html.size(), 50'000);                             // uPlot embedded
    EXPECT_TRUE(html.contains("uPlot"));
    EXPECT_FALSE(html.contains("http://"));
    // The island is gzipped, so the plaintext values must NOT appear verbatim.
    EXPECT_FALSE(html.contains("11.5,22.5,33.5"))
        << "data island should be compressed, not plaintext";

    // …and it still re-imports losslessly through the gunzip path.
    std::vector<std::shared_ptr<Signal>> chans;
    QString lerr;
    ASSERT_TRUE(loadStorableHtml(p, &chans, nullptr, &lerr)) << lerr.toStdString();
    std::error_code ec; std::filesystem::remove(p.toStdString(), ec);
    ASSERT_EQ(chans.size(), 1u);
    auto vs = chans[0]->readAsDouble();
    ASSERT_EQ(vs.size(), 3u);
    EXPECT_DOUBLE_EQ(vs[1], 22.5);
}

TEST(StorableHtml, EmbedsAndRestoresLayoutJson) {
    SignalStore store;
    store.add(makeSig("a", "", Signal::Domain::Time, 0, 1'000'000LL, {1, 2}));

    HtmlExportView view;
    HtmlAxis y1; y1.label = "Y1"; y1.color = "#1e1e1e";
    view.time.axes = {y1};
    HtmlChannel ca; ca.name = "a"; ca.color = "#112233";
    view.time.channels = {ca};
    view.layoutJson = "{\"schema\":1,\"label\":\"Y1\"}";

    bool ok = false;
    QString err;
    const QString p = writeStorable(store, &view, "layout", &ok, &err);
    ASSERT_TRUE(ok) << err.toStdString();

    std::vector<std::shared_ptr<Signal>> chans;
    QString layout, lerr;
    ASSERT_TRUE(loadStorableHtml(p, &chans, &layout, &lerr)) << lerr.toStdString();
    std::error_code ec; std::filesystem::remove(p.toStdString(), ec);

    EXPECT_TRUE(layout.contains("\"label\":\"Y1\""));
    EXPECT_TRUE(layout.contains("\"schema\":1"));
}

TEST(StorableHtml, OldVisualisationOnlyExportHasNoIslandAndWontLoad) {
    SignalStore store;
    store.add(makeSig("a", "", Signal::Domain::Time, 0, 1'000'000LL, {1, 2, 3}));
    const auto path = std::filesystem::temp_directory_path() /
                      "scope_old_export.html";
    const QString p = QString::fromStdString(path.string());
    // The original, untouched export path still works and produces no island.
    ASSERT_TRUE(exportInteractiveHtml(p, store, nullptr));
    QString html;
    { QFile f(p); ASSERT_TRUE(f.open(QIODevice::ReadOnly));
      html = QString::fromUtf8(f.readAll()); }
    EXPECT_FALSE(html.contains("id=\"scope-data\""));

    std::vector<std::shared_ptr<Signal>> chans;
    QString err;
    EXPECT_FALSE(loadStorableHtml(p, &chans, nullptr, &err));
    EXPECT_FALSE(err.isEmpty());
    std::error_code ec; std::filesystem::remove(path, ec);
}

TEST(SignalIo, HtmlFormatDetectionAndFilters) {
    using namespace scope::converter;
    EXPECT_EQ(static_cast<int>(detectFormatFromExtension("foo.html")),
              static_cast<int>(FileFormat::Html));
    EXPECT_EQ(static_cast<int>(detectFormatFromExtension("foo.HTM")),
              static_cast<int>(FileFormat::Html));
    EXPECT_EQ(defaultSuffix(FileFormat::Html).toStdString(), "html");
    EXPECT_FALSE(nameFilters(FileFormat::Html).isEmpty());
}

TEST(SignalIo, LoadFileDispatchesHtmlByExtension) {
    SignalStore store;
    store.add(makeSig("a", "V", Signal::Domain::Time, 0, 1'000'000LL, {1, 2, 3}));
    const auto path = std::filesystem::temp_directory_path() /
                      "scope_loadfile_dispatch.html";
    const QString p = QString::fromStdString(path.string());
    QString err;
    ASSERT_TRUE(exportStorableHtml(p, store, &err)) << err.toStdString();

    const auto r = scope::converter::loadFile(path);   // FileFormat::Auto
    std::error_code ec; std::filesystem::remove(path, ec);
    EXPECT_TRUE(r.ok);
    ASSERT_EQ(r.channels.size(), 1u);
    EXPECT_EQ(r.channels[0]->meta().name.toStdString(), "a");
}
