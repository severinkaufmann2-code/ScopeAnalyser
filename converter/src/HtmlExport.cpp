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
    const QString css = readResource(":/uplot/uPlot.min.css", &err);
    const QString js  = readResource(":/uplot/uPlot.iife.min.js", &err);
    if (css.isEmpty() || js.isEmpty()) {
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
"use strict";

function makeApp(app) {
  // ---------- DOM scaffold ----------
  const block = document.createElement("div"); block.className = "block";
  const toolbar = document.createElement("div"); toolbar.className = "toolbar";
  const body = document.createElement("div"); body.className = "body";
  const panel = document.createElement("div"); panel.className = "panel";
  const plotwrap = document.createElement("div"); plotwrap.className = "plotwrap";
  block.append(toolbar, body); body.append(panel, plotwrap);
  document.getElementById("charts").appendChild(block);

  const charts = { time: app.time, frequency: app.frequency };
  const sKey = k => "y" + k;

  // ---------- view state (mirrors the app) ----------
  // X-channel candidates for XY: every channel of both domains.
  const xCands = [];
  ["time", "frequency"].forEach(dom => {
    const spec = charts[dom];
    if (!spec) return;
    spec.series.forEach((s, i) => xCands.push({
      dom, i, label: s.label + (dom === "frequency" ? "  (freq)" : "") }));
  });
  let xSel = xCands.length ? xCands[0] : null;
  if (app.xyChannel) {
    const m = xCands.find(c => charts[c.dom].series[c.i].name === app.xyChannel);
    if (m) xSel = m;
  }
  let curView = app.view;
  if (curView === "xy" && !xSel) curView = null;
  if (curView !== "xy" && (!curView || !charts[curView]))
    curView = charts.time ? "time" : "frequency";

  function activeSpec() {                 // domain spec backing the view
    return curView === "xy" ? charts[xSel.dom] : charts[curView];
  }

  let mode = 0;                           // 0 Line, 1 Points, 2 Both
  let autoY = [], manual = [];
  function resetYState() {
    const n = activeSpec().axes.length;
    autoY  = Array(n).fill(true);
    manual = Array(n).fill(null);
  }
  const measure = { on: false, p1: null, p2: null };
  let u = null;
  let rows = [];                          // {series, chartIdx, valEl}

  // ---------- toolbar ----------
  function group(parent) {
    const g = document.createElement("div"); g.className = "tbgroup";
    parent.appendChild(g); return g;
  }
  function btn(parent, text, tip, fn) {
    const b = document.createElement("button"); b.className = "tbtn";
    b.textContent = text; b.title = tip;
    b.addEventListener("click", fn);
    parent.appendChild(b); return b;
  }
  function caption(parent, text) {
    const c = document.createElement("span"); c.className = "tbcap";
    c.textContent = text; parent.appendChild(c);
  }
  const gFit = group(toolbar);
  btn(gFit, "⤢", "Fit all data into view — X and all Y axes", fitAll);
  btn(gFit, "↕", "Fit each Y axis to the data inside the current X window", fitY);
  caption(toolbar, "Fit");
  const gZoom = group(toolbar);
  btn(gZoom, "↔ +", "Zoom X in",  () => zoomX(0.85));
  btn(gZoom, "↔ −", "Zoom X out", () => zoomX(1 / 0.85));
  btn(gZoom, "↕ +", "Zoom all Y in",  () => zoomYAll(0.85));
  btn(gZoom, "↕ −", "Zoom all Y out", () => zoomYAll(1 / 0.85));
  caption(toolbar, "Zoom");
  const mBtn = btn(toolbar, "Δ Measure",
    "Click two points to mark Δx / Δy / 1/|Δx|. Right-click clears.",
    () => { measure.on = !measure.on; measure.p1 = measure.p2 = null;
            mBtn.classList.toggle("on", measure.on);
            u.over.style.cursor = measure.on ? "crosshair" : "default";
            u.redraw(); });
  const modeSel = document.createElement("select"); modeSel.className = "tbtn";
  ["Line", "Points", "Line + points"].forEach((t, i) => {
    const o = document.createElement("option"); o.value = i; o.textContent = t;
    modeSel.appendChild(o);
  });
  modeSel.addEventListener("change", () => { mode = +modeSel.value; buildChart(true); });
  toolbar.appendChild(modeSel);

  // ---------- left panel: view / X axis / channels ----------
  function ptitle(text) {
    const t = document.createElement("div"); t.className = "ptitle";
    t.textContent = text; panel.appendChild(t); return t;
  }
  ptitle("VIEW");
  const viewSel = document.createElement("select"); viewSel.className = "psel";
  [["time", "Time  (t [s])"], ["frequency", "Frequency  (f [Hz])"],
   ["xy", "XY  (channel vs channel)"]].forEach(([v, t]) => {
    const o = document.createElement("option"); o.value = v; o.textContent = t;
    o.disabled = (v === "xy") ? !xCands.length : !charts[v];
    viewSel.appendChild(o);
  });
  viewSel.value = curView;
  viewSel.addEventListener("change", () => {
    curView = viewSel.value;
    xTitle.style.display = xRow.style.display =
        curView === "xy" ? "" : "none";
    measure.p1 = measure.p2 = null;
    resetYState();
    rebuildPanelRows();
    buildChart(false);
  });
  panel.appendChild(viewSel);

  const xTitle = ptitle("X AXIS");
  const xRow = document.createElement("select"); xRow.className = "psel";
  xCands.forEach((c, i) => {
    const o = document.createElement("option"); o.value = i; o.textContent = c.label;
    xRow.appendChild(o);
  });
  if (xSel) xRow.value = xCands.indexOf(xSel);
  xRow.addEventListener("change", () => {
    xSel = xCands[+xRow.value];
    measure.p1 = measure.p2 = null;
    resetYState();
    rebuildPanelRows();
    buildChart(false);
  });
  panel.appendChild(xRow);
  xTitle.style.display = xRow.style.display = curView === "xy" ? "" : "none";

  ptitle("CHANNELS");
  const rowsHost = document.createElement("div");
  panel.appendChild(rowsHost);

  // Visible channels of the current view: whole domain for Time/Frequency;
  // for XY the same-domain channels minus the X channel (the app's rule).
  function viewSeries() {
    const spec = activeSpec();
    const idxs = spec.series.map((s, i) => i);
    return (curView === "xy" ? idxs.filter(i => i !== xSel.i) : idxs)
        .map((i, j) => ({ series: spec.series[i], specIdx: i, chartIdx: j + 1 }));
  }

  function rebuildPanelRows() {
    rowsHost.textContent = "";
    rows = viewSeries();
    rows.forEach(r => {
      const row = document.createElement("div"); row.className = "crow";
      const cb = document.createElement("input"); cb.type = "checkbox";
      cb.checked = r.series.visible;
      cb.addEventListener("change", () => {
        r.series.visible = cb.checked;             // remembered per channel
        u.setSeries(r.chartIdx, { show: cb.checked });
      });
      const sw = document.createElement("span"); sw.className = "swatch";
      sw.style.background = r.series.color;
      const nm = document.createElement("span"); nm.className = "cname";
      nm.textContent = r.series.label; nm.title = r.series.label;
      const vl = document.createElement("span"); vl.className = "cval";
      r.valEl = vl;
      row.append(cb, sw, nm, vl); rowsHost.appendChild(row);
    });
  }

  // ---------- XY pairing (mirrors AnalyserPlot's XY path) ----------
  // X and Y live on the domain's union time grid. Where Y has no sample at
  // one of X's times, interpolate Y linearly IN TIME between its
  // neighbouring samples; outside Y's recorded range the point becomes a
  // gap (null) — never a fabricated value. Points keep time order, so
  // loops draw like the app's curves.
  function pairXY(spec, xi, yi) {
    const t = spec.data[0], xv = spec.data[xi + 1], yv = spec.data[yi + 1];
    const n = t.length;
    const prev = new Array(n), next = new Array(n);
    let p = -1;
    for (let k = 0; k < n; k++) { if (yv[k] != null) p = k; prev[k] = p; }
    p = -1;
    for (let k = n - 1; k >= 0; k--) { if (yv[k] != null) p = k; next[k] = p; }
    const xs = [], ys = [];
    for (let k = 0; k < n; k++) {
      if (xv[k] == null) continue;                 // X has no sample here
      let y = null;
      if (yv[k] != null) y = yv[k];
      else {
        const a = prev[k], b = next[k];
        if (a >= 0 && b >= 0) {
          y = (a === b) ? yv[a]
              : yv[a] + (t[k] - t[a]) / (t[b] - t[a]) * (yv[b] - yv[a]);
        }
      }
      xs.push(xv[k]); ys.push(y);
    }
    return [xs, ys];
  }

  // ---------- uPlot ----------
  function plotWidth() { return Math.max(plotwrap.clientWidth - 20, 480); }

  function scalesAxes(spec) {
    const scales = { x: { time: false } };
    spec.axes.forEach((ax, k) => {
      scales[sKey(k)] = {
        auto: true,
        range: (uu, dmin, dmax) => {
          if (!autoY[k]) return manual[k];
          if (dmin == null || dmax == null) return [0, 1];
          if (dmin === dmax) { dmin -= 0.5; dmax += 0.5; }
          const m = 0.05 * (dmax - dmin);          // app's 5% margin
          return [dmin - m, dmax + m];
        },
      };
    });
    const xLabel = curView === "xy" ? spec.series[xSel.i].label : spec.xLabel;
    const axes = [{ scale: "x", label: xLabel, stroke: "#5d6570",
                    grid: { show: true, stroke: "#e6e8eb" },
                    ticks: { stroke: "#b4bac2" } }];
    spec.axes.forEach((ax, k) => {
      axes.push({ scale: sKey(k), side: ax.right ? 1 : 3, label: ax.label,
                  stroke: ax.color, ticks: { stroke: ax.color },
                  grid: { show: k === 0, stroke: "#e6e8eb" } });
    });
    return { scales, axes };
  }

  function seriesOpts(s) {
    return {
      label: s.label,
      stroke: s.color,
      scale: sKey(s.axis),
      width: 1.25,
      spanGaps: false,
      points: { show: mode >= 1, size: 5 },
      paths: mode === 1 ? (() => null) : undefined,
    };
  }

  function mkOpts(spec, xyMode) {
    const { scales, axes } = scalesAxes(spec);
    const sList = rows.map(r => {
      const o = seriesOpts(r.series);
      if (!xyMode) o.spanGaps = true;   // union-grid nulls are artifacts
      return o;
    });
    return {
      mode: xyMode ? 2 : 1,
      width: plotWidth(), height: 430,
      scales, axes,
      series: [{}].concat(sList),
      legend: { show: false },
      cursor: { drag: { x: true, y: true, uni: 30, setScale: false } },
      hooks: {
        setCursor: [updateValues],
        setSelect: [onSelect],
        ready: [onReady],
        draw: [drawMeasure],
      },
    };
  }

  function buildChart(keepX) {
    const spec = activeSpec();
    const xr = (keepX && u && curView !== "xy")
                   ? { min: u.scales.x.min, max: u.scales.x.max } : null;
    if (u) { u.destroy(); u = null; }
    let data;
    if (curView === "xy") {
      data = [null].concat(rows.map(r => pairXY(spec, xSel.i, r.specIdx)));
    } else {
      data = [spec.data[0]].concat(rows.map(r => spec.data[r.specIdx + 1]));
    }
    u = new uPlot(mkOpts(spec, curView === "xy"), data, plotwrap);
    rows.forEach(r => { if (!r.series.visible) u.setSeries(r.chartIdx, { show: false }); });
    if (curView !== "xy") {
      const xs = spec.data[0];
      u.setScale("x", xr ? xr : { min: xs[0], max: xs[xs.length - 1] });
    }
  }

  // ---------- actions (same semantics as the app) ----------
  function fitAll() {
    measure.p1 = measure.p2 = null;
    resetYState();
    buildChart(false);
  }
  function fitY() {
    autoY.fill(true);
    u.setScale("x", { min: u.scales.x.min, max: u.scales.x.max });   // re-range Y
  }
  function zoomX(f, atVal) {
    const s = u.scales.x;
    const c = atVal != null ? atVal : (s.min + s.max) / 2;
    u.setScale("x", { min: c - (c - s.min) * f, max: c + (s.max - c) * f });
  }
  function zoomYAll(f, atPosTop) {
    activeSpec().axes.forEach((ax, k) => {
      const s = u.scales[sKey(k)];
      const c = atPosTop != null ? u.posToVal(atPosTop, sKey(k))
                                 : (s.min + s.max) / 2;
      autoY[k] = false;
      manual[k] = [c - (c - s.min) * f, c + (s.max - c) * f];
    });
    u.setScale("x", { min: u.scales.x.min, max: u.scales.x.max });
  }

  const fmt = v => Number(v.toPrecision(6)).toString();
  function updateValues(uu) {
    if (curView !== "xy") {
      const idx = uu.cursor.idx;
      rows.forEach(r => {
        const v = idx == null ? null : uu.data[r.chartIdx][idx];
        r.valEl.textContent = (v == null) ? "" : fmt(v);
      });
    } else {
      const idxs = uu.cursor.idxs || [];
      rows.forEach(r => {
        const idx = idxs[r.chartIdx];
        const v = idx == null ? null : uu.data[r.chartIdx][1][idx];
        r.valEl.textContent = (v == null) ? "" : fmt(v);
      });
    }
  }

  function onSelect(uu) {
    const sel = uu.select;
    if (sel.width > 8) {
      const x0 = uu.posToVal(sel.left, "x");
      const x1 = uu.posToVal(sel.left + sel.width, "x");
      if (sel.height > 8) {
        activeSpec().axes.forEach((ax, k) => {
          const yTop = uu.posToVal(sel.top, sKey(k));
          const yBot = uu.posToVal(sel.top + sel.height, sKey(k));
          autoY[k] = false;
          manual[k] = [Math.min(yTop, yBot), Math.max(yTop, yBot)];
        });
      }
      uu.setScale("x", { min: Math.min(x0, x1), max: Math.max(x0, x1) });
    }
    uu.setSelect({ left: 0, top: 0, width: 0, height: 0 }, false);
  }

  // ---------- measure tool ----------
  function drawMeasure(uu) {
    if (!measure.p1) return;
    const ctx = uu.ctx;
    const px = p => [uu.valToPos(p.x, "x", true), uu.valToPos(p.y, sKey(0), true)];
    ctx.save();
    ctx.fillStyle = "#dc6400"; ctx.strokeStyle = "#dc6400";
    const dot = p => { const [cx, cy] = px(p);
      ctx.beginPath(); ctx.arc(cx, cy, 4 * devicePixelRatio, 0, 2 * Math.PI); ctx.fill(); };
    dot(measure.p1);
    if (measure.p2) {
      dot(measure.p2);
      const [x1, y1] = px(measure.p1), [x2, y2] = px(measure.p2);
      ctx.setLineDash([5 * devicePixelRatio, 4 * devicePixelRatio]);
      ctx.lineWidth = devicePixelRatio;
      ctx.beginPath(); ctx.moveTo(x1, y1); ctx.lineTo(x2, y2); ctx.stroke();
      ctx.setLineDash([]);
      const dx = measure.p2.x - measure.p1.x;
      const dy = measure.p2.y - measure.p1.y;
      const lines = ["Δx = " + fmt(dx), "Δy = " + fmt(dy)];
      if (dx !== 0 && curView === "time")
        lines.push("1/|Δx| = " + fmt(1 / Math.abs(dx)) + " Hz");
      ctx.font = (12 * devicePixelRatio) + "px system-ui, sans-serif";
      const w = Math.max(...lines.map(t => ctx.measureText(t).width)) + 12 * devicePixelRatio;
      const lh = 15 * devicePixelRatio;
      const bx = Math.min(x1, x2), by = Math.min(y1, y2) - lines.length * lh - 10 * devicePixelRatio;
      ctx.fillStyle = "rgba(255,255,255,0.88)";
      ctx.fillRect(bx, by, w, lines.length * lh + 6 * devicePixelRatio);
      ctx.strokeStyle = "#dc6400";
      ctx.strokeRect(bx, by, w, lines.length * lh + 6 * devicePixelRatio);
      ctx.fillStyle = "#b35000";
      lines.forEach((t, i) => ctx.fillText(t, bx + 6 * devicePixelRatio,
                                           by + (i + 1) * lh));
    }
    ctx.restore();
  }

  // ---------- mouse wiring ----------
  function onReady(uu) {
    uu.over.addEventListener("wheel", e => {
      e.preventDefault();
      const f = e.deltaY < 0 ? 0.85 : 1 / 0.85;
      if (e.shiftKey) zoomYAll(f, uu.cursor.top);
      else            zoomX(f, uu.posToVal(uu.cursor.left, "x"));
    }, { passive: false });
    uu.over.addEventListener("dblclick", fitAll);
    uu.over.addEventListener("click", e => {
      if (!measure.on) return;
      const rect = uu.over.getBoundingClientRect();
      const p = { x: uu.posToVal(e.clientX - rect.left, "x"),
                  y: uu.posToVal(e.clientY - rect.top, sKey(0)) };
      if (!measure.p1 || measure.p2) { measure.p1 = p; measure.p2 = null; }
      else                           { measure.p2 = p; }
      uu.redraw();
    });
    uu.over.addEventListener("contextmenu", e => {
      if (measure.on) {
        e.preventDefault();
        measure.p1 = measure.p2 = null;
        uu.redraw();
      }
    });
  }

  window.addEventListener("resize",
      () => u && u.setSize({ width: plotWidth(), height: 430 }));

  resetYState();
  rebuildPanelRows();
  buildChart(false);
}

%APP%
</script>
</body>
</html>
)HTML");
    html.replace("%CSS%", css);
    html.replace("%JS%", js);
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
