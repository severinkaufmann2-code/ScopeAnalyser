#include "scope/converter/HtmlExport.h"

#include "scope/core/Signal.h"
#include "scope/core/SignalStore.h"

#include <QDateTime>
#include <QFile>
#include <QStringList>

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
void appendJsNumber(QString& out, double v) {
    if (std::isnan(v) || std::isinf(v)) {
        out += QLatin1String("null");
        return;
    }
    char buf[32];
    const auto res = std::to_chars(buf, buf + sizeof(buf), v);
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

// Merge the view's channels onto their union timestamp grid and emit the
// per-domain spec. Where a channel has no sample at a grid point it gets
// null; spanGaps in the page draws through those (grid-alignment
// artifacts, not real gaps) while real NaN samples also become null. The
// page's XY view re-pairs channels from this same data.
ChartJs buildChart(const SignalStore& store, const HtmlChartView& view,
                   bool frequencyDomain, const QString& xLabel) {
    ChartJs out;

    struct Src {
        std::shared_ptr<Signal> sig;
        const HtmlChannel*      ch;
        Signal::ReadView        view;
        std::vector<double>     vals;
        std::size_t             cursor{0};
    };
    std::vector<Src> srcs;
    srcs.reserve(view.channels.size());

    std::vector<TimestampNs> grid;
    TimestampNs origin = 0;
    bool haveOrigin = false;
    for (const auto& ch : view.channels) {
        auto s = store.get(ch.name);
        if (!s || s->sampleCount() == 0) continue;
        Src src;
        src.sig = s;
        src.ch = &ch;
        src.view = s->snapshotForRead();
        src.vals = s->readAsDouble();
        grid.insert(grid.end(), src.view.timestamps,
                    src.view.timestamps + src.view.count);
        if (!frequencyDomain) {
            // Same origin rule as the app's time axis (Slice/Gate outputs
            // anchor at their source's start).
            const TimestampNs o = s->displayOriginNs();
            if (!haveOrigin || o < origin) { origin = o; haveOrigin = true; }
        }
        srcs.push_back(std::move(src));
    }
    if (srcs.empty()) return out;
    std::sort(grid.begin(), grid.end());
    grid.erase(std::unique(grid.begin(), grid.end()), grid.end());

    QStringList axesJs;
    for (const auto& ax : view.axes) {
        axesJs << QString("{label:%1,right:%2,color:%3}")
                      .arg(jsString(ax.label),
                           ax.right ? "true" : "false",
                           jsString(ax.color));
    }

    QString data;
    data.reserve(static_cast<int>(grid.size() * (srcs.size() + 1) * 14 + 64));
    data += '[';
    data += '[';
    for (std::size_t i = 0; i < grid.size(); ++i) {
        if (i) data += ',';
        appendJsNumber(data, (grid[i] - origin) / 1e9);
    }
    data += ']';

    QStringList seriesJs;
    const int axisMax = static_cast<int>(view.axes.size()) - 1;
    for (auto& src : srcs) {
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
            // The union grid contains every channel timestamp, so an exact
            // match either sits at the cursor or the channel has no sample
            // here.
            if (src.cursor < src.view.count
                && src.view.timestamps[src.cursor] == grid[i]) {
                appendJsNumber(data, src.cursor < src.vals.size()
                                         ? src.vals[src.cursor]
                                         : std::nan(""));
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
    out.seriesCount = static_cast<int>(srcs.size());
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
.panel { width: 250px; min-width: 250px; border-right: 1px solid #e6e8eb;
         padding: 8px 10px; box-sizing: border-box; }
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
<p class="meta">Exported %DATE% · %NCH% channel(s) · scroll = zoom X at cursor ·
Shift+scroll = zoom Y · drag = box zoom · double-click = fit · Δ Measure: click two
points, right-click clears</p>
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

}  // namespace scope::converter
