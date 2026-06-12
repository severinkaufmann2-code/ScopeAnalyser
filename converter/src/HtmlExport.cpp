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

// The same trace palette the app's light theme uses (the page is light).
const char* kStrokes[] = {"#1e1e1e", "#d95319", "#3c781e", "#7e2f8e",
                          "#0072bd", "#edb120", "#a2142f", "#4dbeee"};

struct ChartJs {
    QString labelsJs;   // ["name [unit]", ...]
    QString dataJs;     // [[x...],[s1...],...]
    int     seriesCount{0};
    std::size_t points{0};
};

// Merge the signals of one domain onto their union timestamp grid and emit
// uPlot's aligned-data layout. Where a channel has no sample at a grid
// point it gets null; spanGaps in the page draws through those (they're
// grid-alignment artifacts, not real gaps).
ChartJs buildChart(const std::vector<std::shared_ptr<Signal>>& sigs,
                   bool frequencyDomain) {
    ChartJs out;

    struct Src {
        Signal::ReadView view;
        std::vector<double> vals;
        std::size_t cursor{0};
    };
    std::vector<Src> srcs;
    srcs.reserve(sigs.size());

    std::vector<TimestampNs> grid;
    TimestampNs origin = 0;
    bool haveOrigin = false;
    for (const auto& s : sigs) {
        Src src;
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
    std::sort(grid.begin(), grid.end());
    grid.erase(std::unique(grid.begin(), grid.end()), grid.end());
    out.points = grid.size();

    // Rough pre-size: ~14 chars per number.
    QString data;
    data.reserve(static_cast<int>(grid.size() * (sigs.size() + 1) * 14 + 64));
    data += '[';

    data += '[';
    for (std::size_t i = 0; i < grid.size(); ++i) {
        if (i) data += ',';
        appendJsNumber(data, (grid[i] - origin) / 1e9);
    }
    data += ']';

    QStringList labels;
    for (std::size_t k = 0; k < srcs.size(); ++k) {
        auto& src = srcs[k];
        const auto& meta = sigs[k]->meta();
        QString label = meta.unit.isEmpty()
                            ? meta.name
                            : meta.name + " [" + meta.unit + "]";
        label.replace('\\', QLatin1String("\\\\"));
        label.replace('"', QLatin1String("\\\""));
        labels << QString("\"%1\"").arg(label);

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

    out.labelsJs = '[' + labels.join(',') + ']';
    out.dataJs = std::move(data);
    out.seriesCount = static_cast<int>(sigs.size());
    return out;
}

}  // namespace

bool exportInteractiveHtml(const QString& path,
                           const SignalStore& store,
                           QString* errorOut) {
    initUplotResource();   // static-lib resources need an explicit pull-in

    std::vector<std::shared_ptr<Signal>> timeSigs, freqSigs;
    for (const auto& name : store.channelNames()) {
        auto s = store.get(name);
        if (!s || s->sampleCount() == 0) continue;
        (s->meta().domain == Signal::Domain::Frequency ? freqSigs : timeSigs)
            .push_back(std::move(s));
    }
    if (timeSigs.empty() && freqSigs.empty()) {
        if (errorOut) *errorOut = "The signal store is empty — nothing to export.";
        return false;
    }

    QString err;
    const QString css = readResource(":/uplot/uPlot.min.css", &err);
    const QString js  = readResource(":/uplot/uPlot.iife.min.js", &err);
    if (css.isEmpty() || js.isEmpty()) {
        if (errorOut) *errorOut = err;
        return false;
    }

    QString strokes = "[";
    for (std::size_t i = 0; i < std::size(kStrokes); ++i) {
        if (i) strokes += ',';
        strokes += QString("\"%1\"").arg(kStrokes[i]);
    }
    strokes += ']';

    QString charts;
    int totalChannels = 0;
    if (!timeSigs.empty()) {
        const auto c = buildChart(timeSigs, /*frequencyDomain=*/false);
        charts += QString("makeChart(\"Time\", \"t [s]\", %1, %2);\n")
                      .arg(c.labelsJs, c.dataJs);
        totalChannels += c.seriesCount;
    }
    if (!freqSigs.empty()) {
        const auto c = buildChart(freqSigs, /*frequencyDomain=*/true);
        charts += QString("makeChart(\"Frequency\", \"f [Hz]\", %1, %2);\n")
                      .arg(c.labelsJs, c.dataJs);
        totalChannels += c.seriesCount;
    }

    QString html = QString(R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ScopeAnalyser — interactive chart</title>
<style>%CSS%
body { font-family: system-ui, sans-serif; margin: 24px; color: #20242a; background: #f7f8fa; }
h1 { font-size: 20px; margin: 0 0 4px 0; }
.meta { color: #5d6570; font-size: 13px; margin: 0 0 20px 0; }
.chart { background: #ffffff; border: 1px solid #d4d8dd; border-radius: 8px;
         padding: 12px; margin-bottom: 24px; display: inline-block; }
.chart h2 { font-size: 15px; margin: 2px 6px 8px; color: #20242a; }
.u-legend { font-size: 12px; }
</style>
<script>%JS%</script>
</head>
<body>
<h1>ScopeAnalyser — interactive chart</h1>
<p class="meta">Exported %DATE% · %NCH% channel(s) · scroll = zoom X at cursor ·
drag = box zoom · double-click = reset · click a legend entry to show/hide a channel</p>
<div id="charts"></div>
<script>
"use strict";
const STROKES = %STROKES%;

function wheelZoomPlugin(factor) {
  return { hooks: { ready: u => {
    u.over.addEventListener("wheel", e => {
      e.preventDefault();
      const rect = u.over.getBoundingClientRect();
      const leftPct = u.cursor.left / rect.width;
      const xVal = u.posToVal(u.cursor.left, "x");
      const oxRange = u.scales.x.max - u.scales.x.min;
      const nxRange = e.deltaY < 0 ? oxRange * factor : oxRange / factor;
      const nxMin = xVal - leftPct * nxRange;
      u.setScale("x", { min: nxMin, max: nxMin + nxRange });
    }, { passive: false });
  }}};
}

function makeChart(title, xLabel, labels, data) {
  const holder = document.createElement("div");
  holder.className = "chart";
  const h2 = document.createElement("h2");
  h2.textContent = title;
  holder.appendChild(h2);
  document.getElementById("charts").appendChild(holder);

  const series = [{ label: xLabel }].concat(labels.map((l, i) => ({
    label: l,
    stroke: STROKES[i % STROKES.length],
    width: 1.25,
    spanGaps: true,
    points: { show: false },
  })));

  const width = Math.min(Math.max(document.body.clientWidth - 80, 640), 1600);
  const u = new uPlot({
    title: "",
    width: width,
    height: 420,
    scales: { x: { time: false } },
    series: series,
    legend: { live: true },
    cursor: { drag: { x: true, y: true, uni: 30 } },
    plugins: [wheelZoomPlugin(0.85)],
  }, data, holder);

  window.addEventListener("resize", () => {
    const w = Math.min(Math.max(document.body.clientWidth - 80, 640), 1600);
    u.setSize({ width: w, height: 420 });
  });
  return u;
}

%CHARTS%
</script>
</body>
</html>
)HTML");
    html.replace("%CSS%", css);
    html.replace("%JS%", js);
    html.replace("%DATE%", QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm"));
    html.replace("%NCH%", QString::number(totalChannels));
    html.replace("%STROKES%", strokes);
    html.replace("%CHARTS%", charts);

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
