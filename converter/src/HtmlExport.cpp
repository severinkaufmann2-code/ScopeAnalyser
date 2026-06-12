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
    QString specJs;      // the makeChart({...}) argument
    int     seriesCount{0};
};

// Merge the view's channels onto their union timestamp grid and emit the
// makeChart spec. Where a channel has no sample at a grid point it gets
// null; spanGaps in the page draws through those (grid-alignment
// artifacts, not real gaps) while real NaN samples also become null.
ChartJs buildChart(const SignalStore& store, const HtmlChartView& view,
                   bool frequencyDomain, const QString& title,
                   const QString& xLabel) {
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

    // ---- axes ----
    QStringList axesJs;
    for (const auto& ax : view.axes) {
        axesJs << QString("{label:%1,right:%2,color:%3}")
                      .arg(jsString(ax.label),
                           ax.right ? "true" : "false",
                           jsString(ax.color));
    }

    // ---- series + data ----
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
        seriesJs << QString("{label:%1,color:%2,axis:%3,visible:%4}")
                        .arg(jsString(label),
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

    out.specJs = QString("{title:%1,xLabel:%2,axes:[%3],series:[%4],data:%5}")
                     .arg(jsString(title), jsString(xLabel),
                          axesJs.join(','), seriesJs.join(','), data);
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

    QString charts;
    int totalChannels = 0;
    if (!view.time.channels.isEmpty()) {
        const auto c = buildChart(store, view.time, /*frequencyDomain=*/false,
                                  "Time", "t [s]");
        if (c.seriesCount > 0) {
            charts += QString("makeChart(%1);\n").arg(c.specJs);
            totalChannels += c.seriesCount;
        }
    }
    if (!view.frequency.channels.isEmpty()) {
        const auto c = buildChart(store, view.frequency, /*frequencyDomain=*/true,
                                  "Frequency", "f [Hz]");
        if (c.seriesCount > 0) {
            charts += QString("makeChart(%1);\n").arg(c.specJs);
            totalChannels += c.seriesCount;
        }
    }
    if (totalChannels == 0) {
        if (errorOut) *errorOut = "The signal store is empty — nothing to export.";
        return false;
    }

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
.block h2 { font-size: 14px; margin: 0; padding: 8px 12px 0 12px; color: #20242a; }
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
select.tbtn { border-color: #d4d8dd; background: #ffffff; }
.body { display: flex; align-items: stretch; }
.panel { width: 240px; min-width: 240px; border-right: 1px solid #e6e8eb;
         padding: 8px 10px; box-sizing: border-box; }
.ptitle { color: #5d6570; font-size: 11px; font-weight: 700; letter-spacing: 1px;
          margin: 2px 0 8px 0; }
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

function makeChart(spec) {
  // ---------- DOM scaffold ----------
  const block = document.createElement("div"); block.className = "block";
  const h2 = document.createElement("h2"); h2.textContent = spec.title;
  const toolbar = document.createElement("div"); toolbar.className = "toolbar";
  const body = document.createElement("div"); body.className = "body";
  const panel = document.createElement("div"); panel.className = "panel";
  const plotwrap = document.createElement("div"); plotwrap.className = "plotwrap";
  block.append(h2, toolbar, body); body.append(panel, plotwrap);
  document.getElementById("charts").appendChild(block);

  const xs = spec.data[0];
  const xMin = xs[0], xMax = xs[xs.length - 1];
  const sKey = k => "y" + k;

  // ---------- state (mirrors the app) ----------
  let mode = 0;                                  // 0 Line, 1 Points, 2 Both
  const autoY  = spec.axes.map(() => true);      // per-axis auto-fit flag
  const manual = spec.axes.map(() => null);      // manual [min,max] when !autoY
  const measure = { on: false, p1: null, p2: null };
  let u = null;

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
  modeSel.addEventListener("change", () => { mode = +modeSel.value; build(true); });
  toolbar.appendChild(modeSel);

  // ---------- channel panel ----------
  const pt = document.createElement("div"); pt.className = "ptitle";
  pt.textContent = "CHANNELS"; panel.appendChild(pt);
  const valEls = [];
  spec.series.forEach((s, i) => {
    const row = document.createElement("div"); row.className = "crow";
    const cb = document.createElement("input"); cb.type = "checkbox";
    cb.checked = s.visible;
    cb.addEventListener("change", () => {
      s.visible = cb.checked;
      u.setSeries(i + 1, { show: cb.checked });
    });
    const sw = document.createElement("span"); sw.className = "swatch";
    sw.style.background = s.color;
    const nm = document.createElement("span"); nm.className = "cname";
    nm.textContent = s.label; nm.title = s.label;
    const vl = document.createElement("span"); vl.className = "cval";
    valEls.push(vl);
    row.append(cb, sw, nm, vl); panel.appendChild(row);
  });

  // ---------- uPlot ----------
  function plotWidth() { return Math.max(plotwrap.clientWidth - 20, 480); }

  function mkOpts() {
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
    const axes = [{ scale: "x", label: spec.xLabel, stroke: "#5d6570",
                    grid: { show: true, stroke: "#e6e8eb" },
                    ticks: { stroke: "#b4bac2" } }];
    spec.axes.forEach((ax, k) => {
      axes.push({ scale: sKey(k), side: ax.right ? 1 : 3, label: ax.label,
                  stroke: ax.color, ticks: { stroke: ax.color },
                  grid: { show: k === 0, stroke: "#e6e8eb" } });
    });
    const series = [{}].concat(spec.series.map(s => ({
      label: s.label,
      stroke: s.color,
      scale: sKey(s.axis),
      width: 1.25,
      spanGaps: true,
      points: { show: mode >= 1, size: 5 },
      paths: mode === 1 ? (() => null) : undefined,
    })));
    return {
      width: plotWidth(), height: 430,
      scales, axes, series,
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

  function build(keepView) {
    const xr = (keepView && u) ? { min: u.scales.x.min, max: u.scales.x.max } : null;
    if (u) u.destroy();
    u = new uPlot(mkOpts(), spec.data, plotwrap);
    spec.series.forEach((s, i) => { if (!s.visible) u.setSeries(i + 1, { show: false }); });
    u.setScale("x", xr ? xr : { min: xMin, max: xMax });
  }

  // ---------- actions (same semantics as the app) ----------
  function fitAll() {
    autoY.fill(true);
    measureClear();
    u.setScale("x", { min: xMin, max: xMax });
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
    spec.axes.forEach((ax, k) => {
      const s = u.scales[sKey(k)];
      const c = atPosTop != null ? u.posToVal(atPosTop, sKey(k))
                                 : (s.min + s.max) / 2;
      autoY[k] = false;
      manual[k] = [c - (c - s.min) * f, c + (s.max - c) * f];
    });
    u.redraw(false);
    u.setScale("x", { min: u.scales.x.min, max: u.scales.x.max });
  }

  function updateValues(uu) {
    const idx = uu.cursor.idx;
    spec.series.forEach((s, i) => {
      const v = idx == null ? null : uu.data[i + 1][idx];
      valEls[i].textContent = (v == null) ? "" : Number(v.toPrecision(6)).toString();
    });
  }

  function onSelect(uu) {
    const sel = uu.select;
    if (sel.width > 8) {
      const x0 = uu.posToVal(sel.left, "x");
      const x1 = uu.posToVal(sel.left + sel.width, "x");
      if (sel.height > 8) {
        spec.axes.forEach((ax, k) => {
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
  function measureClear() { measure.p1 = measure.p2 = null; if (u) u.redraw(); }
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
      const fmt = v => Number(v.toPrecision(6)).toString();
      const lines = ["Δx = " + fmt(dx), "Δy = " + fmt(dy)];
      if (dx !== 0) lines.push("1/|Δx| = " + fmt(1 / Math.abs(dx)) + " Hz");
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
      if (measure.on) { e.preventDefault(); measureClear(); }
    });
  }

  window.addEventListener("resize",
      () => u.setSize({ width: plotWidth(), height: 430 }));

  build(false);
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
