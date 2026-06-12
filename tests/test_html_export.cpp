// exportInteractiveHtml: a self-contained interactive chart file. These
// tests check the file is genuinely standalone (uPlot embedded), carries
// the channel data at full precision, splits domains into two charts,
// aligns differing grids on their union with nulls, and turns NaN into
// gaps instead of fabricated numbers.

#include "scope/converter/HtmlExport.h"
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
    QFile f(QString::fromStdString(path.string()));
    if (f.open(QIODevice::ReadOnly)) content = QString::fromUtf8(f.readAll());
    std::filesystem::remove(path);
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
    EXPECT_TRUE(html.contains("\"Time\""));
    EXPECT_TRUE(html.contains("\"Frequency\""));
    EXPECT_TRUE(html.contains("\"f [Hz]\""));
    EXPECT_TRUE(html.contains("[0,1,2],[9,8,7]"));   // Hz axis + magnitudes
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
