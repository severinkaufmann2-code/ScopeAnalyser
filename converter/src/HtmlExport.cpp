#include "scope/converter/HtmlExport.h"

#include "scope/core/Signal.h"
#include "scope/core/SignalStore.h"

#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QStringList>

#include <zlib.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <memory>
#include <vector>

// Q_INIT_RESOURCE must expand at global scope — inside a namespace its
// extern declaration would resolve into that namespace and not link.
static void initUplotResource() { Q_INIT_RESOURCE(uplot); }

namespace scope::converter {

namespace {

using scope::core::Signal;
using scope::core::SignalStore;
using scope::core::TimestampNs;

QString readResource(const char* path, QString* errorOut) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (errorOut) *errorOut = QString("embedded resource missing: %1").arg(path);
        return {};
    }
    return QString::fromUtf8(f.readAll());
}

// Shortest exact-round-trip double for the embedded JSON (std::to_chars:
// "0.1" stays "0.1", full precision preserved); NaN/inf become null (a gap).
//
// digits > 0 caps the significant figures instead. Only the chart-only
// export uses that: full-precision doubles off real instrumentation are
// essentially random in their low bits and so barely compress, while a
// chart cannot resolve anything like that many figures — rounding is what
// makes that file small.
void appendJsNumber(QString& out, double v, int digits = 0) {
    if (std::isnan(v) || std::isinf(v)) {
        out += QLatin1String("null");
        return;
    }
    char buf[40];
    const auto res = (digits > 0)
        ? std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::general,
                        digits)
        : std::to_chars(buf, buf + sizeof(buf), v);
    out += QLatin1String(buf, static_cast<int>(res.ptr - buf));
}

QString jsString(QString s) {
    s.replace('\\', QLatin1String("\\\\"));
    s.replace('"', QLatin1String("\\\""));
    return '"' + s + '"';
}

// The light-theme axis palette (the page is light): index 0 is the neutral
// ink Y1 colour, the rest are the hue entries. Mirrors ScopePlot's light
// palette without making the converter depend on the plot lib.
const char* kLightAxisPalette[] = {"#1e1e1e", "#d95319", "#3c781e", "#7e2f8e",
                                   "#0072bd", "#edb120", "#a2142f", "#4dbeee"};

// Default trace colour rule for the store-only overload — same sequence the
// app uses on the neutral Y1 axis: first channel ink, then the hues.
QString defaultChannelColor(int n) {
    if (n == 0) return kLightAxisPalette[0];
    return kLightAxisPalette[1 + (n - 1) % 7];
}

HtmlChartView defaultChartView(const SignalStore& store, Signal::Domain domain) {
    HtmlChartView v;
    HtmlAxis y1;
    y1.label = "Y1";
    y1.color = kLightAxisPalette[0];
    v.axes.append(y1);
    int n = 0;
    for (const auto& name : store.channelNames()) {
        auto s = store.get(name);
        if (!s || s->meta().domain != domain || s->sampleCount() == 0) continue;
        HtmlChannel c;
        c.name = name;
        c.color = defaultChannelColor(n++);
        v.channels.append(c);
    }
    return v;
}

struct ChartJs {
    QString specJs;      // {xLabel, axes, series, data}
    int     seriesCount{0};
};

// One source channel prepared for the union-grid merge: its samples in
// timestamp order (duplicates kept) plus the cursor the emit loops walk.
struct MergeSrc {
    std::shared_ptr<Signal> sig;
    const HtmlChannel*      ch{nullptr};
    const TimestampNs*      ts{nullptr};   // -> rv.timestamps, or tsOwned
    std::size_t             count{0};
    std::vector<double>     vals;
    std::size_t             cursor{0};

    Signal::ReadView         rv{};        // sig keeps the pointed-at buffer alive
    std::vector<TimestampNs> tsOwned;     // only used when the channel was unsorted
};

struct MergedGrid {
    std::vector<MergeSrc>    srcs;
    std::vector<TimestampNs> grid;
    TimestampNs              origin{0};
};

// Collect the view's plottable channels and build the union timestamp grid
// every channel is emitted against.
//
// A timestamp appears in the grid as many times as the channel that repeats
// it most. Scope data legitimately carries several samples at one instant
// (two ADS notifications inside a tick, a CSV whose time column lost
// resolution), and giving every repeat its own grid slot is what keeps the
// per-channel cursors in the emit loops in step. Collapsing repeats into a
// single slot — what a plain std::unique does — leaves the repeating
// channel's cursor one behind the grid for good, so every later timestamp
// compare fails and the whole rest of that column is written as null.
MergedGrid buildMergedGrid(const SignalStore& store, const HtmlChartView& view,
                           bool frequencyDomain) {
    MergedGrid out;
    out.srcs.reserve(view.channels.size());

    bool haveOrigin = false;
    for (const auto& ch : view.channels) {
        auto s = store.get(ch.name);
        if (!s || s->sampleCount() == 0) continue;
        MergeSrc src;
        src.sig   = s;
        src.ch    = &ch;
        src.rv    = s->snapshotForRead();
        src.vals  = s->readAsDouble();
        src.count = std::min(src.rv.count, src.vals.size());
        src.ts    = src.rv.timestamps;

        // Timestamps are expected non-decreasing, but an imported column can
        // still arrive out of order — which desynchronises the merge exactly
        // like a duplicate does. Sort a local copy (stable, so samples
        // sharing a timestamp keep their acquisition order); the Signal
        // itself is left untouched.
        if (!std::is_sorted(src.ts, src.ts + src.count)) {
            std::vector<std::size_t> idx(src.count);
            for (std::size_t i = 0; i < src.count; ++i) idx[i] = i;
            std::stable_sort(idx.begin(), idx.end(),
                             [&](std::size_t a, std::size_t b) {
                                 return src.rv.timestamps[a] < src.rv.timestamps[b];
                             });
            std::vector<double> sortedVals(src.count);
            src.tsOwned.resize(src.count);
            for (std::size_t i = 0; i < src.count; ++i) {
                src.tsOwned[i] = src.rv.timestamps[idx[i]];
                sortedVals[i]  = src.vals[idx[i]];
            }
            src.vals = std::move(sortedVals);
            src.ts   = src.tsOwned.data();   // survives the move into srcs
        }

        if (!frequencyDomain) {
            // Same origin rule as the app's time axis (Slice/Gate outputs
            // anchor at their source's start).
            const TimestampNs o = s->displayOriginNs();
            if (!haveOrigin || o < out.origin) { out.origin = o; haveOrigin = true; }
        }
        out.srcs.push_back(std::move(src));
    }
    if (out.srcs.empty()) return out;

    std::size_t total = 0;
    bool anyDuplicate = false;
    for (const auto& s : out.srcs) {
        total += s.count;
        for (std::size_t i = 1; !anyDuplicate && i < s.count; ++i)
            if (s.ts[i] == s.ts[i - 1]) anyDuplicate = true;
    }

    // Common case — no channel repeats a timestamp — is the plain distinct
    // union, byte-for-byte what this always produced.
    if (!anyDuplicate) {
        out.grid.reserve(total);
        for (const auto& s : out.srcs)
            out.grid.insert(out.grid.end(), s.ts, s.ts + s.count);
        std::sort(out.grid.begin(), out.grid.end());
        out.grid.erase(std::unique(out.grid.begin(), out.grid.end()),
                       out.grid.end());
        return out;
    }

    // Repeats present: reduce each channel to (timestamp, run length) and
    // merge by max, so the grid has room for the longest run at each instant.
    std::vector<std::pair<TimestampNs, std::size_t>> runs;
    runs.reserve(total);
    for (const auto& s : out.srcs) {
        std::size_t i = 0;
        while (i < s.count) {
            std::size_t j = i + 1;
            while (j < s.count && s.ts[j] == s.ts[i]) ++j;
            runs.emplace_back(s.ts[i], j - i);
            i = j;
        }
    }
    std::sort(runs.begin(), runs.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (std::size_t i = 0; i < runs.size();) {
        std::size_t j = i, n = 0;
        while (j < runs.size() && runs[j].first == runs[i].first)
            n = std::max(n, runs[j++].second);
        out.grid.insert(out.grid.end(), n, runs[i].first);
        i = j;
    }
    return out;
}

// Merge the view's channels onto their union timestamp grid and emit the
// per-domain spec. Where a channel has no sample at a grid point it gets
// null; spanGaps in the page draws through those (grid-alignment
// artifacts, not real gaps) while real NaN samples also become null. The
// page's XY view re-pairs channels from this same data.
ChartJs buildChart(const SignalStore& store, const HtmlChartView& view,
                   bool frequencyDomain, const QString& xLabel) {
    ChartJs out;

    MergedGrid merged = buildMergedGrid(store, view, frequencyDomain);
    if (merged.srcs.empty()) return out;
    const auto& grid = merged.grid;

    QStringList axesJs;
    for (const auto& ax : view.axes) {
        axesJs << QString("{label:%1,right:%2,color:%3}")
                      .arg(jsString(ax.label),
                           ax.right ? "true" : "false",
                           jsString(ax.color));
    }

    QString data;
    data.reserve(static_cast<int>(grid.size() * (merged.srcs.size() + 1) * 14 + 64));
    data += '[';
    data += '[';
    for (std::size_t i = 0; i < grid.size(); ++i) {
        if (i) data += ',';
        appendJsNumber(data, (grid[i] - merged.origin) / 1e9);
    }
    data += ']';

    QStringList seriesJs;
    const int axisMax = static_cast<int>(view.axes.size()) - 1;
    for (auto& src : merged.srcs) {
        const auto& meta = src.sig->meta();
        const QString label = meta.unit.isEmpty()
                                  ? meta.name
                                  : meta.name + " [" + meta.unit + "]";
        seriesJs << QString("{name:%1,label:%2,color:%3,axis:%4,visible:%5}")
                        .arg(jsString(meta.name),
                             jsString(label),
                             jsString(src.ch->color),
                             QString::number(std::clamp(src.ch->axisIndex, 0, axisMax)),
                             src.ch->visible ? "true" : "false");

        data += ",[";
        for (std::size_t i = 0; i < grid.size(); ++i) {
            if (i) data += ',';
            // The grid carries every channel timestamp with every repeat, so
            // an unconsumed sample either sits at the cursor or the channel
            // has nothing here.
            if (src.cursor < src.count && src.ts[src.cursor] == grid[i]) {
                appendJsNumber(data, src.vals[src.cursor]);
                ++src.cursor;
            } else {
                data += QLatin1String("null");
            }
        }
        data += ']';
    }
    data += ']';

    out.specJs = QString("{xLabel:%1,axes:[%2],series:[%3],data:%4}")
                     .arg(jsString(xLabel), axesJs.join(','),
                          seriesJs.join(','), data);
    out.seriesCount = static_cast<int>(merged.srcs.size());
    return out;
}

}  // namespace

bool exportInteractiveHtml(const QString& path,
                           const SignalStore& store,
                           QString* errorOut) {
    HtmlExportView view;
    view.time      = defaultChartView(store, Signal::Domain::Time);
    view.frequency = defaultChartView(store, Signal::Domain::Frequency);
    view.initialView = view.time.channels.isEmpty() ? "frequency" : "time";
    return exportInteractiveHtml(path, store, view, errorOut);
}

bool exportInteractiveHtml(const QString& path,
                           const SignalStore& store,
                           const HtmlExportView& view,
                           QString* errorOut) {
    initUplotResource();   // static-lib resources need an explicit pull-in

    QString err;
    const QString css    = readResource(":/uplot/uPlot.min.css", &err);
    const QString js     = readResource(":/uplot/uPlot.iife.min.js", &err);
    const QString pageJs = readResource(":/uplot/scope_page.js", &err);
    if (css.isEmpty() || js.isEmpty() || pageJs.isEmpty()) {
        if (errorOut) *errorOut = err;
        return false;
    }

    int totalChannels = 0;
    QString timeSpec = "null", freqSpec = "null";
    if (!view.time.channels.isEmpty()) {
        const auto c = buildChart(store, view.time, /*frequencyDomain=*/false,
                                  "t [s]");
        if (c.seriesCount > 0) {
            timeSpec = c.specJs;
            totalChannels += c.seriesCount;
        }
    }
    if (!view.frequency.channels.isEmpty()) {
        const auto c = buildChart(store, view.frequency, /*frequencyDomain=*/true,
                                  "f [Hz]");
        if (c.seriesCount > 0) {
            freqSpec = c.specJs;
            totalChannels += c.seriesCount;
        }
    }
    if (totalChannels == 0) {
        if (errorOut) *errorOut = "The signal store is empty — nothing to export.";
        return false;
    }

    const QString appJs =
        QString("makeApp({view:%1,xyChannel:%2,time:%3,frequency:%4});")
            .arg(jsString(view.initialView), jsString(view.xyChannel),
                 timeSpec, freqSpec);

    QString html = QString(R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ScopeAnalyser — interactive chart</title>
<style>%CSS%
body { font-family: system-ui, sans-serif; margin: 24px; color: #20242a; background: #f4f5f7; }
h1 { font-size: 20px; margin: 0 0 4px 0; }
.meta { color: #5d6570; font-size: 13px; margin: 0 0 20px 0; }
.block { background: #ffffff; border: 1px solid #d4d8dd; border-radius: 8px;
         margin-bottom: 24px; overflow: hidden; }
.toolbar { display: flex; align-items: center; gap: 6px; padding: 8px 12px;
           background: #eceef1; border-bottom: 1px solid #d4d8dd; flex-wrap: wrap; }
.tbgroup { display: flex; gap: 2px; border: 1px solid #d4d8dd; border-radius: 6px;
           padding: 2px; background: transparent; }
.tbcap { color: #5d6570; font-size: 12px; margin: 0 8px 0 3px; }
.tbtn { font: inherit; font-size: 13px; border: 1px solid transparent; border-radius: 4px;
        background: transparent; color: #20242a; padding: 3px 9px; cursor: pointer; }
.tbtn:hover { background: rgba(0,0,0,0.07); }
.tbtn.on { background: rgba(46,111,214,0.18); color: #265fba;
           border-color: rgba(46,111,214,0.45); }
select.tbtn, select.psel { border: 1px solid #d4d8dd; background: #ffffff;
        font: inherit; font-size: 13px; border-radius: 4px; padding: 3px 6px; }
select.psel { width: 100%; box-sizing: border-box; margin-bottom: 8px; }
.body { display: flex; align-items: stretch; }
.panel { width: 250px; min-width: 250px; flex: none;
         padding: 8px 10px; box-sizing: border-box; }
/* Divider between the channel list and the plot: a 1px rule inside a 7px
   grab strip, so it reads as a hairline but is still easy to hit. */
.pgrip { flex: none; width: 7px; cursor: col-resize; background: #e6e8eb;
         border-left: 3px solid #ffffff; border-right: 3px solid #ffffff;
         background-clip: padding-box; box-sizing: border-box; }
.pgrip:hover, .pgrip.on { background: #2e6fd6; }
/* Same strip under the chart: drag to set the chart height, double-click to
   go back to filling the window. */
.vgrip { height: 7px; cursor: row-resize; background: #e6e8eb;
         border-top: 3px solid #ffffff; border-bottom: 3px solid #ffffff;
         background-clip: padding-box; box-sizing: border-box; }
.vgrip:hover, .vgrip.on { background: #2e6fd6; }
.ptitle { color: #5d6570; font-size: 11px; font-weight: 700; letter-spacing: 1px;
          margin: 6px 0 6px 0; }
.crow { display: flex; align-items: center; gap: 7px; padding: 3px 0; font-size: 13px; }
.crow input { margin: 0; }
.swatch { width: 11px; height: 11px; border-radius: 3px; flex: none; }
.cname { flex: 1; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.cval { color: #5d6570; font-variant-numeric: tabular-nums; font-size: 12px; }
.plotwrap { flex: 1; padding: 8px 10px; min-width: 0; }
.u-legend { display: none; }
</style>
<script>%JS%</script>
</head>
<body>
<h1>ScopeAnalyser — interactive chart</h1>
<p class="meta">Exported %DATE% · %NCH% channel(s) · scroll = zoom X and Y at the
cursor · Ctrl+scroll = X only · Shift+scroll = Y only · scroll over an axis = that
axis · drag = pan · double-click = fit · Δ Measure: click two points, right-click
clears · drag the divider left of the plot to widen the channel list · drag
the strip under the chart to resize it</p>
<div id="charts"></div>
<script>
%PAGEJS%
%APP%
</script>
</body>
</html>
)HTML");
    html.replace("%CSS%", css);
    html.replace("%JS%", js);
    html.replace("%PAGEJS%", pageJs);
    html.replace("%DATE%", QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm"));
    html.replace("%NCH%", QString::number(totalChannels));
    html.replace("%APP%", appJs);

    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut) *errorOut = QString("Couldn't write %1: %2")
                                      .arg(path, out.errorString());
        return false;
    }
    out.write(html.toUtf8());
    return true;
}

namespace {

// gzip (RFC 1952) so the browser's embedded fflate can gunzip it and the
// CLI `gunzip` can inspect it; windowBits 15+16 selects the gzip wrapper.
QByteArray gzipCompress(const QByteArray& in) {
    z_stream s{};
    if (deflateInit2(&s, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK)
        return {};
    s.next_in  = reinterpret_cast<Bytef*>(const_cast<char*>(in.constData()));
    s.avail_in = static_cast<uInt>(in.size());
    QByteArray out;
    char buf[1 << 16];
    int ret;
    do {
        s.next_out  = reinterpret_cast<Bytef*>(buf);
        s.avail_out = sizeof(buf);
        ret = deflate(&s, Z_FINISH);
        out.append(buf, static_cast<int>(sizeof(buf) - s.avail_out));
    } while (ret == Z_OK);
    deflateEnd(&s);
    return (ret == Z_STREAM_END) ? out : QByteArray{};
}

// inflate gzip or zlib (15+32 auto-detects the wrapper).
QByteArray gzipDecompress(const QByteArray& in, bool* ok) {
    *ok = false;
    z_stream s{};
    if (inflateInit2(&s, 15 + 32) != Z_OK) return {};
    s.next_in  = reinterpret_cast<Bytef*>(const_cast<char*>(in.constData()));
    s.avail_in = static_cast<uInt>(in.size());
    QByteArray out;
    char buf[1 << 16];
    int ret;
    do {
        s.next_out  = reinterpret_cast<Bytef*>(buf);
        s.avail_out = sizeof(buf);
        ret = inflate(&s, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) { inflateEnd(&s); return {}; }
        out.append(buf, static_cast<int>(sizeof(buf) - s.avail_out));
    } while (ret != Z_STREAM_END);
    inflateEnd(&s);
    *ok = true;
    return out;
}

// ----- Storable HTML --------------------------------------------------
// Emits ONE domain's data island object: the same union-grid merge as
// buildChart() above (shared via buildMergedGrid), but with exact int64-ns
// timestamps as strings, an explicit unit per series, and a per-domain
// display origin — everything loadStorableHtml() needs to reconstruct the
// signals. Returns "null" (and leaves *nonEmpty false) when the domain has
// no plottable channel.
QString buildIslandDomain(const SignalStore& store, const HtmlChartView& view,
                          bool frequencyDomain, const QString& xLabel,
                          int digits, bool* nonEmpty) {
    *nonEmpty = false;

    MergedGrid merged = buildMergedGrid(store, view, frequencyDomain);
    if (merged.srcs.empty()) return QStringLiteral("null");
    const auto& grid = merged.grid;

    QList<HtmlAxis> axes = view.axes;
    if (axes.isEmpty()) {
        HtmlAxis a; a.label = "Y1"; a.color = kLightAxisPalette[0];
        axes.append(a);
    }
    QStringList axesJs;
    for (const auto& ax : axes) {
        axesJs << QString("{\"label\":%1,\"right\":%2,\"color\":%3}")
                      .arg(jsString(ax.label), ax.right ? "true" : "false",
                           jsString(ax.color));
    }
    const int axisMax = static_cast<int>(axes.size()) - 1;

    // Exact int64-ns timestamps as strings (lossless for any range/frequency).
    QString tArr;
    tArr.reserve(static_cast<int>(grid.size() * 22 + 8));
    tArr += '[';
    for (std::size_t i = 0; i < grid.size(); ++i) {
        if (i) tArr += ',';
        tArr += '"';
        tArr += QString::number(static_cast<qlonglong>(grid[i]));
        tArr += '"';
    }
    tArr += ']';

    QStringList seriesJs, colsJs;
    for (auto& src : merged.srcs) {
        const auto& meta = src.sig->meta();
        const QString label = meta.unit.isEmpty()
                                  ? meta.name
                                  : meta.name + " [" + meta.unit + "]";
        seriesJs << QString("{\"name\":%1,\"unit\":%2,\"label\":%3,\"color\":%4,"
                            "\"axis\":%5,\"visible\":%6}")
                        .arg(jsString(meta.name), jsString(meta.unit),
                             jsString(label), jsString(src.ch->color),
                             QString::number(std::clamp(src.ch->axisIndex, 0, axisMax)),
                             src.ch->visible ? "true" : "false");

        QString col;
        col.reserve(static_cast<int>(grid.size() * 8 + 8));
        col += '[';
        for (std::size_t i = 0; i < grid.size(); ++i) {
            if (i) col += ',';
            if (src.cursor < src.count && src.ts[src.cursor] == grid[i]) {
                appendJsNumber(col, src.vals[src.cursor], digits);
                ++src.cursor;
            } else {
                col += QLatin1String("null");
            }
        }
        col += ']';
        colsJs << col;
    }

    *nonEmpty = true;
    return QString("{\"xLabel\":%1,\"origin\":\"%2\",\"axes\":[%3],"
                   "\"series\":[%4],\"t\":%5,\"cols\":[%6]}")
        .arg(jsString(xLabel),
             QString::number(static_cast<qlonglong>(merged.origin)),
             axesJs.join(','), seriesJs.join(','), tArr, colsJs.join(','));
}

}  // namespace

bool exportStorableHtml(const QString& path, const SignalStore& store,
                        QString* errorOut) {
    HtmlExportView view;
    view.time      = defaultChartView(store, Signal::Domain::Time);
    view.frequency = defaultChartView(store, Signal::Domain::Frequency);
    view.initialView = view.time.channels.isEmpty() ? "frequency" : "time";
    return exportStorableHtml(path, store, view, errorOut);
}

namespace {

// The id the page's data block carries. loadStorableHtml() keys on
// "scope-data", so a chart-only file gets a different one and is refused on
// re-open — the honest outcome, since its values are rounded for display.
constexpr char kStorableIslandId[] = "scope-data";
constexpr char kChartOnlyIslandId[] = "scope-chart";

// Shared body of exportStorableHtml() / exportChartOnlyHtml(): identical
// page, differing only in how precisely the values are written and whether
// the block is marked as re-importable.
bool writeIslandHtml(const QString& path, const SignalStore& store,
                     const HtmlExportView& view, bool chartOnly, int digits,
                     QString* errorOut) {
    initUplotResource();   // static-lib resources need an explicit pull-in

    QString err;
    const QString css     = readResource(":/uplot/uPlot.min.css", &err);
    const QString js      = readResource(":/uplot/uPlot.iife.min.js", &err);
    const QString pageJs  = readResource(":/uplot/scope_page.js", &err);
    const QString fflate  = readResource(":/uplot/fflate.min.js", &err);
    if (css.isEmpty() || js.isEmpty() || pageJs.isEmpty() || fflate.isEmpty()) {
        if (errorOut) *errorOut = err;
        return false;
    }

    bool haveTime = false, haveFreq = false;
    const QString timeDom = buildIslandDomain(store, view.time,
                                              /*frequencyDomain=*/false,
                                              "t [s]", digits, &haveTime);
    const QString freqDom = buildIslandDomain(store, view.frequency,
                                              /*frequencyDomain=*/true,
                                              "f [Hz]", digits, &haveFreq);
    if (!haveTime && !haveFreq) {
        if (errorOut) *errorOut = "The signal store is empty — nothing to export.";
        return false;
    }

    int totalChannels = 0;
    for (const auto& c : view.time.channels) {
        auto s = store.get(c.name);
        if (s && s->sampleCount()) ++totalChannels;
    }
    for (const auto& c : view.frequency.channels) {
        auto s = store.get(c.name);
        if (s && s->sampleCount()) ++totalChannels;
    }

    QStringList domainParts;
    if (haveTime) domainParts << QString("\"time\":%1").arg(timeDom);
    if (haveFreq) domainParts << QString("\"frequency\":%1").arg(freqDom);
    // The embedded layout exists purely so a re-open restores axes / view;
    // a chart-only page can never be re-opened, so it just costs bytes.
    const QString layoutPart = (chartOnly || view.layoutJson.isEmpty())
        ? QStringLiteral("null") : view.layoutJson;

    const QString islandJson =
        QString("{\"version\":1,\"view\":%1,\"xyChannel\":%2,\"layout\":%3,"
                "\"domains\":{%4}}")
            .arg(jsString(view.initialView), jsString(view.xyChannel),
                 layoutPart, domainParts.join(','));

    // gzip + base64 the island: ~5x smaller on disk than raw JSON text, the
    // single biggest contributor to file size. base64's alphabet is
    // script-tag-safe (no '<'), so no escaping is needed. The browser
    // gunzips it with the embedded fflate; loadStorableHtml() inflates it
    // on re-open.
    const QByteArray islandGz = gzipCompress(islandJson.toUtf8());
    if (islandGz.isEmpty()) {
        if (errorOut) *errorOut = "Failed to compress chart data.";
        return false;
    }
    const QString island = QString::fromLatin1(islandGz.toBase64());

    QString html = QString(R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ScopeAnalyser — interactive chart</title>
<style>%CSS%
body { font-family: system-ui, sans-serif; margin: 24px; color: #20242a; background: #f4f5f7; }
h1 { font-size: 20px; margin: 0 0 4px 0; }
.meta { color: #5d6570; font-size: 13px; margin: 0 0 20px 0; }
.block { background: #ffffff; border: 1px solid #d4d8dd; border-radius: 8px;
         margin-bottom: 24px; overflow: hidden; }
.toolbar { display: flex; align-items: center; gap: 6px; padding: 8px 12px;
           background: #eceef1; border-bottom: 1px solid #d4d8dd; flex-wrap: wrap; }
.tbgroup { display: flex; gap: 2px; border: 1px solid #d4d8dd; border-radius: 6px;
           padding: 2px; background: transparent; }
.tbcap { color: #5d6570; font-size: 12px; margin: 0 8px 0 3px; }
.tbtn { font: inherit; font-size: 13px; border: 1px solid transparent; border-radius: 4px;
        background: transparent; color: #20242a; padding: 3px 9px; cursor: pointer; }
.tbtn:hover { background: rgba(0,0,0,0.07); }
.tbtn.on { background: rgba(46,111,214,0.18); color: #265fba;
           border-color: rgba(46,111,214,0.45); }
select.tbtn, select.psel { border: 1px solid #d4d8dd; background: #ffffff;
        font: inherit; font-size: 13px; border-radius: 4px; padding: 3px 6px; }
select.psel { width: 100%; box-sizing: border-box; margin-bottom: 8px; }
.body { display: flex; align-items: stretch; }
.panel { width: 250px; min-width: 250px; flex: none;
         padding: 8px 10px; box-sizing: border-box; }
/* Divider between the channel list and the plot: a 1px rule inside a 7px
   grab strip, so it reads as a hairline but is still easy to hit. */
.pgrip { flex: none; width: 7px; cursor: col-resize; background: #e6e8eb;
         border-left: 3px solid #ffffff; border-right: 3px solid #ffffff;
         background-clip: padding-box; box-sizing: border-box; }
.pgrip:hover, .pgrip.on { background: #2e6fd6; }
/* Same strip under the chart: drag to set the chart height, double-click to
   go back to filling the window. */
.vgrip { height: 7px; cursor: row-resize; background: #e6e8eb;
         border-top: 3px solid #ffffff; border-bottom: 3px solid #ffffff;
         background-clip: padding-box; box-sizing: border-box; }
.vgrip:hover, .vgrip.on { background: #2e6fd6; }
.ptitle { color: #5d6570; font-size: 11px; font-weight: 700; letter-spacing: 1px;
          margin: 6px 0 6px 0; }
.crow { display: flex; align-items: center; gap: 7px; padding: 3px 0; font-size: 13px; }
.crow input { margin: 0; }
.swatch { width: 11px; height: 11px; border-radius: 3px; flex: none; }
.cname { flex: 1; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.cval { color: #5d6570; font-variant-numeric: tabular-nums; font-size: 12px; }
.plotwrap { flex: 1; padding: 8px 10px; min-width: 0; }
.u-legend { display: none; }
</style>
<script>%JS%</script>
</head>
<body>
<h1>ScopeAnalyser — interactive chart</h1>
<p class="meta">Exported %DATE% · %NCH% channel(s) · %KIND% ·
scroll = zoom X and Y at the cursor · Ctrl+scroll = X only · Shift+scroll = Y only ·
scroll over an axis = that axis · drag = pan · double-click = fit · Δ Measure: click
two points, right-click clears · drag the divider left of the plot to widen the
channel list · drag the strip under the chart to resize it</p>
<div id="charts"></div>
<script id="%ISLANDID%" type="application/octet-stream" data-encoding="gzip+base64">%ISLAND%</script>
<script>%FFLATE%</script>
<script>
%PAGEJS%
(function () {
  // The data lives once, gzip+base64 in the island above. Decode it with
  // the embedded fflate, then adapt to the shape makeApp() consumes
  // (display-seconds X grid + aligned value columns); ScopeAnalyser
  // re-imports the same island via loadStorableHtml().
  var b64 = document.getElementById("%ISLANDID%").textContent.trim();
  var bin = atob(b64);
  var bytes = new Uint8Array(bin.length);
  for (var i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
  var d = JSON.parse(fflate.strFromU8(fflate.gunzipSync(bytes)));
  function spec(name) {
    var b = d.domains[name];
    if (!b) return null;
    var origin = Number(b.origin);
    var xs = b.t.map(function (s) { return (Number(s) - origin) / 1e9; });
    return { xLabel: b.xLabel, axes: b.axes, series: b.series,
             data: [xs].concat(b.cols) };
  }
  makeApp({ view: d.view, xyChannel: d.xyChannel,
            time: spec("time"), frequency: spec("frequency") });
})();
</script>
</body>
</html>
)HTML");
    html.replace("%CSS%", css);
    html.replace("%JS%", js);
    html.replace("%FFLATE%", fflate);
    html.replace("%PAGEJS%", pageJs);
    html.replace("%DATE%", QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm"));
    html.replace("%NCH%", QString::number(totalChannels));
    html.replace("%KIND%", chartOnly
        ? QString("chart only, values rounded to %1 significant figures")
              .arg(digits)
        : QString("re-openable in ScopeAnalyser"));
    html.replace("%ISLANDID%",
                 chartOnly ? kChartOnlyIslandId : kStorableIslandId);
    html.replace("%ISLAND%", island);

    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut) *errorOut = QString("Couldn't write %1: %2")
                                      .arg(path, out.errorString());
        return false;
    }
    out.write(html.toUtf8());
    return true;
}

}  // namespace

bool exportStorableHtml(const QString& path, const SignalStore& store,
                        const HtmlExportView& view, QString* errorOut) {
    return writeIslandHtml(path, store, view, /*chartOnly=*/false,
                           /*digits=*/0, errorOut);
}

bool exportChartOnlyHtml(const QString& path, const SignalStore& store,
                         const HtmlExportView& view, int digits,
                         QString* errorOut) {
    return writeIslandHtml(path, store, view, /*chartOnly=*/true,
                           digits > 0 ? digits : kChartOnlyDefaultDigits,
                           errorOut);
}

bool exportChartOnlyHtml(const QString& path, const SignalStore& store,
                         int digits, QString* errorOut) {
    HtmlExportView view;
    view.time      = defaultChartView(store, Signal::Domain::Time);
    view.frequency = defaultChartView(store, Signal::Domain::Frequency);
    view.initialView = view.time.channels.isEmpty() ? "frequency" : "time";
    return exportChartOnlyHtml(path, store, view, digits, errorOut);
}

bool loadStorableHtml(const QString& path,
                      std::vector<std::shared_ptr<Signal>>* channelsOut,
                      QString* layoutJsonOut, QString* errorOut) {
    if (!channelsOut) return false;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (errorOut) *errorOut = QString("Couldn't open %1: %2")
                                      .arg(path, f.errorString());
        return false;
    }
    const QString htmlText = QString::fromUtf8(f.readAll());
    f.close();

    // Extract the <script id="scope-data" …>…</script> payload. Every "</"
    // inside the island was escaped to "<\/" on write, so the first
    // "</script>" after the opening tag is the genuine close.
    const int idPos = htmlText.indexOf(QLatin1String("id=\"scope-data\""));
    if (idPos < 0) {
        if (errorOut) *errorOut =
            "This HTML carries no embedded scope data — it isn't a "
            "re-loadable ScopeAnalyser export.";
        return false;
    }
    const int tagEnd = htmlText.indexOf('>', idPos);
    const int close  = (tagEnd < 0) ? -1
                     : htmlText.indexOf(QLatin1String("</script>"), tagEnd);
    if (tagEnd < 0 || close < 0) {
        if (errorOut) *errorOut = "Malformed embedded scope-data block.";
        return false;
    }
    const QString openTag  = htmlText.mid(idPos, tagEnd - idPos);
    const QString islandText =
        htmlText.mid(tagEnd + 1, close - tagEnd - 1).trimmed();

    // Newer exports gzip+base64 the island; the very first storable files
    // embedded raw JSON. Honour both.
    QByteArray jsonBytes;
    if (openTag.contains(QLatin1String("gzip+base64"))) {
        bool ok = false;
        jsonBytes = gzipDecompress(
            QByteArray::fromBase64(islandText.toLatin1()), &ok);
        if (!ok) {
            if (errorOut) *errorOut = "Couldn't decompress embedded scope data.";
            return false;
        }
    } else {
        jsonBytes = islandText.toUtf8();
    }

    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonBytes, &perr);
    if (doc.isNull() || !doc.isObject()) {
        if (errorOut) *errorOut =
            QString("Couldn't parse embedded scope data: %1").arg(perr.errorString());
        return false;
    }
    const QJsonObject root = doc.object();

    if (layoutJsonOut) {
        const QJsonValue lay = root.value("layout");
        *layoutJsonOut = lay.isObject()
            ? QString::fromUtf8(QJsonDocument(lay.toObject()).toJson(QJsonDocument::Compact))
            : QString();
    }

    const QJsonObject domains = root.value("domains").toObject();
    auto loadDomain = [&](const QString& key, Signal::Domain domain) {
        const QJsonValue dv = domains.value(key);
        if (!dv.isObject()) return;
        const QJsonObject d = dv.toObject();
        const QJsonArray t      = d.value("t").toArray();
        const QJsonArray series = d.value("series").toArray();
        const QJsonArray cols   = d.value("cols").toArray();
        const int n = t.size();

        std::vector<TimestampNs> ts(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i)
            ts[i] = static_cast<TimestampNs>(t.at(i).toString().toLongLong());

        const int sCount = std::min(series.size(), cols.size());
        for (int s = 0; s < sCount; ++s) {
            const QJsonObject so = series.at(s).toObject();
            const QJsonArray col = cols.at(s).toArray();

            Signal::Meta m;
            m.name     = so.value("name").toString();
            m.unit     = so.value("unit").toString();
            m.dataType = scope::core::DataType::Float64;
            m.domain   = domain;

            std::vector<TimestampNs> sts;
            std::vector<double>      svs;
            sts.reserve(static_cast<std::size_t>(n));
            svs.reserve(static_cast<std::size_t>(n));
            const int m2 = std::min(n, static_cast<int>(col.size()));
            for (int i = 0; i < m2; ++i) {
                const QJsonValue v = col.at(i);
                if (!v.isDouble()) continue;   // null = gap → channel has no sample here
                sts.push_back(ts[static_cast<std::size_t>(i)]);
                svs.push_back(v.toDouble());
            }
            if (sts.empty()) continue;
            auto sig = std::make_shared<Signal>(m);
            sig->append(sts.data(),
                        reinterpret_cast<const std::byte*>(svs.data()),
                        sts.size());
            channelsOut->push_back(std::move(sig));
        }
    };
    loadDomain("time", Signal::Domain::Time);
    loadDomain("frequency", Signal::Domain::Frequency);

    if (channelsOut->empty()) {
        if (errorOut) *errorOut = "No channels found in the embedded scope data.";
        return false;
    }
    return true;
}

}  // namespace scope::converter
