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
            u.over.style.cursor = measure.on ? "crosshair" : "grab";
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
          // t[a] === t[b] with a !== b happens when several samples share one
          // timestamp: no span to interpolate across, so take the neighbour.
          y = (a === b || t[a] === t[b]) ? yv[a]
              : yv[a] + (t[k] - t[a]) / (t[b] - t[a]) * (yv[b] - yv[a]);
        }
      }
      xs.push(xv[k]); ys.push(y);
    }
    return [xs, ys];
  }

  // ---------- XY drawing (uPlot mode 2 needs custom paths) ----------
  // The built-in line renderer only handles aligned (mode 1) data, so XY
  // series draw themselves: lines between consecutive points in time order
  // (pen up at null gaps) and/or sample dots, per the display mode.
  function xyPaths(u, seriesIdx) {
    uPlot.orient(u, seriesIdx, (series, dataX, dataY, scaleX, scaleY,
                                valToPosX, valToPosY, xOff, yOff, xDim, yDim,
                                moveTo, lineTo, rect, arc) => {
      const d = u.data[seriesIdx];          // [xs, ys]
      const ctx = u.ctx;
      const dpr = devicePixelRatio;
      ctx.save();
      ctx.beginPath();
      ctx.rect(u.bbox.left, u.bbox.top, u.bbox.width, u.bbox.height);
      ctx.clip();
      ctx.strokeStyle = series.stroke();
      ctx.fillStyle = series.stroke();
      ctx.lineWidth = 1.25 * dpr;
      if (mode !== 1) {                     // lines
        const p = new Path2D();
        let pen = false;
        for (let i = 0; i < d[0].length; i++) {
          const xv = d[0][i], yv = d[1][i];
          if (xv == null || yv == null) { pen = false; continue; }   // gap
          const cx = valToPosX(xv, scaleX, xDim, xOff);
          const cy = valToPosY(yv, scaleY, yDim, yOff);
          if (pen) lineTo(p, cx, cy);
          else { moveTo(p, cx, cy); pen = true; }
        }
        ctx.stroke(p);
      }
      if (mode >= 1) {                      // points
        const r = 2.5 * dpr;
        const p = new Path2D();
        for (let i = 0; i < d[0].length; i++) {
          const xv = d[0][i], yv = d[1][i];
          if (xv == null || yv == null) continue;
          const cx = valToPosX(xv, scaleX, xDim, xOff);
          const cy = valToPosY(yv, scaleY, yDim, yOff);
          p.moveTo(cx + r, cy);
          arc(p, cx, cy, r, 0, 2 * Math.PI);
        }
        ctx.fill(p);
      }
      ctx.restore();
    });
    return null;                            // drawn above; nothing to cache
  }

  // ---------- uPlot ----------
  function plotWidth() { return Math.max(plotwrap.clientWidth - 20, 480); }

  function scalesAxes(spec) {
    const scales = { x: { time: false } };
    if (curView === "xy")
      scales.x.range = (uu, dmin, dmax) =>
          (dmin == null) ? [0, 1] : [dmin, dmax];
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
      if (xyMode) {
        return {
          label: r.series.label,
          stroke: r.series.color,
          facets: [{ scale: "x", auto: true },
                   { scale: sKey(r.series.axis), auto: true }],
          paths: xyPaths,
          points: { show: false },
        };
      }
      const o = seriesOpts(r.series);
      o.spanGaps = true;                // union-grid nulls are artifacts
      return o;
    });
    return {
      mode: xyMode ? 2 : 1,
      width: plotWidth(), height: 430,
      scales, axes,
      series: [{}].concat(sList),
      legend: { show: false },
      // No selection rect: the app has no box zoom — left-drag pans there,
      // and onReady() wires the same here.
      cursor: { drag: { x: false, y: false, setScale: false } },
      hooks: {
        setCursor: [updateValues],
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
  // Scale one range about a centre — QCPAxis::scaleRange, which is what the
  // app's zoomAt() calls.
  const scaleAbout = (min, max, c, f) => [c - (c - min) * f, c + (max - c) * f];

  // Stage a Y axis's new range. Y is only ever re-read through the scale's
  // range callback, so staging into manual[] and then re-setting X is how a
  // Y change gets applied — hence applyPending() below.
  function stageY(k, min, max) {
    autoY[k] = false;
    manual[k] = [min, max];
  }
  // One setScale call flushes both: X directly, staged Y via its range cb.
  function applyPending(xr) {
    const s = u.scales.x;
    u.setScale("x", xr || { min: s.min, max: s.max });
  }

  function zoomX(f, atVal) {
    const s = u.scales.x;
    const c = atVal != null ? atVal : (s.min + s.max) / 2;
    const [min, max] = scaleAbout(s.min, s.max, c, f);
    applyPending({ min, max });
  }
  function zoomYAll(f, atPosTop) {
    zoomY(f, atPosTop, -1);
  }
  // axisIndex < 0 → every Y axis together, as the app's zoomAt(-1) does.
  function zoomY(f, atPosTop, axisIndex) {
    activeSpec().axes.forEach((ax, k) => {
      if (axisIndex >= 0 && k !== axisIndex) return;
      const s = u.scales[sKey(k)];
      const c = atPosTop != null ? u.posToVal(atPosTop, sKey(k))
                                 : (s.min + s.max) / 2;
      const [min, max] = scaleAbout(s.min, s.max, c, f);
      stageY(k, min, max);
    });
    applyPending(null);
  }

  // The app's ScopePlot::zoomAt: X and/or Y scaled about the cursor, applied
  // together so one wheel notch is a single redraw.
  function zoomAt(fx, fy, posLeft, posTop, axisIndex) {
    let xr = null;
    if (fx) {
      const s = u.scales.x;
      const c = u.posToVal(posLeft, "x");
      const [min, max] = scaleAbout(s.min, s.max, c, fx);
      xr = { min, max };
    }
    if (fy) {
      activeSpec().axes.forEach((ax, k) => {
        if (axisIndex >= 0 && k !== axisIndex) return;
        const s = u.scales[sKey(k)];
        const c = u.posToVal(posTop, sKey(k));
        const [min, max] = scaleAbout(s.min, s.max, c, fy);
        stageY(k, min, max);
      });
    }
    applyPending(xr);
  }

  const fmt = v => Number(v.toPrecision(6)).toString();

  // ---------- cursor read-out ----------
  // Every channel of a domain is emitted against the union of all their
  // timestamps, so a channel sampled at 10 ms is null at each grid point a
  // 5 ms channel contributed — and reading the raw column at the cursor
  // would blank the slower channel out at half the positions it is hovered.
  // Report what the drawn line reads instead: the sample where there is
  // one, otherwise a linear interpolation between the channel's own
  // neighbouring samples, which is exactly the segment spanGaps draws
  // through those holes. An interpolated read-out is prefixed "≈" so it is
  // never mistaken for a recorded sample. Before a channel's first sample
  // or after its last there is no line, hence no value.
  //
  // Each column's real sample positions are indexed once, lazily, so a
  // hover costs a binary search rather than a walk over millions of points.
  const samplePos = new WeakMap();
  function samplePositions(col) {
    let pos = samplePos.get(col);
    if (!pos) {
      let n = 0;
      for (let i = 0; i < col.length; i++) if (col[i] != null) n++;
      pos = new Int32Array(n);
      for (let i = 0, k = 0; i < col.length; i++) if (col[i] != null) pos[k++] = i;
      samplePos.set(col, pos);
    }
    return pos;
  }
  // → {v, exact} for grid point idx, or null where the channel has no line.
  function readAt(xs, col, idx) {
    if (col[idx] != null) return { v: col[idx], exact: true };
    const pos = samplePositions(col);
    let lo = 0, hi = pos.length;               // → first sample after idx
    while (lo < hi) {
      const m = (lo + hi) >> 1;
      if (pos[m] < idx) lo = m + 1; else hi = m;
    }
    if (lo === 0 || lo === pos.length) return null;   // outside this channel
    const a = pos[lo - 1], b = pos[lo];
    // Samples sharing one timestamp leave no span to interpolate across.
    const v = xs[a] === xs[b]
                  ? col[a]
                  : col[a] + (xs[idx] - xs[a]) / (xs[b] - xs[a]) * (col[b] - col[a]);
    return { v, exact: false };
  }

  function updateValues(uu) {
    if (curView !== "xy") {
      const idx = uu.cursor.idx;
      const xs = uu.data[0];
      rows.forEach(r => {
        const s = idx == null ? null : readAt(xs, uu.data[r.chartIdx], idx);
        r.valEl.textContent = !s ? "" : (s.exact ? fmt(s.v) : "≈ " + fmt(s.v));
      });
    } else {
      // The app's XY view has no per-channel cursor read-out either.
      rows.forEach(r => { r.valEl.textContent = ""; });
    }
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
  // Deliberately the same model as the app's ScopePlot::eventFilter, so the
  // exported page and the Analyser feel identical:
  //   wheel, no modifier, over the plot   → zoom X and Y about the cursor
  //   Ctrl + wheel                        → X only
  //   Shift / Alt + wheel                 → all Y (or one, over a Y gutter)
  //   wheel over a Y axis                 → that axis only
  //   wheel over the X axis               → X only
  //   left-drag                           → pan (the app has no box zoom)

  // The app steps 0.85 per wheel notch and reads partial notches from
  // high-resolution devices, so the factor is continuous rather than a fixed
  // step. Browsers report the delta in px / lines / pages — normalise to px
  // first, then to notches at the ~100px per notch browsers use.
  const ZOOM_STEP = 0.85;
  function wheelFactor(e) {
    let dy = e.deltaY || e.deltaX;
    if (e.deltaMode === 1) dy *= 16;        // lines  → px
    else if (e.deltaMode === 2) dy *= 400;  // pages  → px
    if (!dy) return 0;
    return Math.pow(ZOOM_STEP, -dy / 100);
  }

  // Where the pointer is relative to the plot rect, in the same terms the
  // app uses against its axis rect.
  function hit(uu, e) {
    const r = uu.over.getBoundingClientRect();
    return {
      left: e.clientX - r.left,
      top:  e.clientY - r.top,
      overY: e.clientX < r.left || e.clientX > r.right,
      overX: e.clientY < r.top  || e.clientY > r.bottom,
    };
  }

  // Mirror of ScopePlot::closestYAxisToPos: which Y axis owns the gutter the
  // pointer is in. uPlot stacks each axis outward from the plot edge in a
  // band of its own width, so walk out from that edge; anything unexpected
  // falls back to the axis nearest the edge on that side.
  function yAxisAt(uu, clientX) {
    const r = uu.over.getBoundingClientRect();
    const onLeft = clientX < r.left;
    let edge = onLeft ? r.left : r.right;
    let fallback = null;
    for (let k = 0; k < activeSpec().axes.length; k++) {
      const ax = uu.axes[k + 1];                    // index 0 is the X axis
      if (!ax) continue;
      if (onLeft ? ax.side !== 3 : ax.side !== 1) continue;
      if (fallback === null) fallback = k;
      const size = ax._size || 40;
      const lo = onLeft ? edge - size : edge;
      const hi = onLeft ? edge : edge + size;
      if (clientX >= lo && clientX <= hi) return k;
      edge = onLeft ? lo : hi;
    }
    return fallback === null ? 0 : fallback;
  }

  function onWheel(uu, e) {
    e.preventDefault();
    const f = wheelFactor(e);
    if (!f) return;
    const h = hit(uu, e);

    // Same routing ladder as the app, first match wins.
    if (e.ctrlKey || e.metaKey) {
      zoomAt(f, 0, h.left, h.top, -1);
    } else if (e.shiftKey || e.altKey) {
      zoomAt(0, f, h.left, h.top, h.overY ? yAxisAt(uu, e.clientX) : -1);
    } else if (h.overY) {
      zoomAt(0, f, h.left, h.top, yAxisAt(uu, e.clientX));
    } else if (h.overX) {
      zoomAt(f, 0, h.left, h.top, -1);
    } else {
      zoomAt(f, f, h.left, h.top, -1);
    }
  }

  // Left-drag pans, as QCustomPlot's iRangeDrag does in the app. Listening on
  // the document means a drag that leaves the plot keeps tracking, and still
  // ends on mouseup.
  function startPan(uu, e) {
    if (e.button !== 0 || measure.on) return;   // measure mode owns left-click
    e.preventDefault();
    const rect = uu.over.getBoundingClientRect();
    if (!rect.width || !rect.height) return;

    const x0 = { min: uu.scales.x.min, max: uu.scales.x.max };
    const y0 = activeSpec().axes.map((ax, k) => {
      const sc = uu.scales[sKey(k)];
      return { min: sc.min, max: sc.max };
    });
    const startX = e.clientX, startY = e.clientY;
    const prevCursor = uu.over.style.cursor;
    uu.over.style.cursor = "grabbing";

    const move = ev => {
      // Content follows the pointer: dragging right shows earlier X.
      const dx = (ev.clientX - startX) / rect.width  * (x0.max - x0.min);
      const dy = (ev.clientY - startY) / rect.height;
      y0.forEach((s, k) => {
        const shift = dy * (s.max - s.min);
        stageY(k, s.min + shift, s.max + shift);
      });
      applyPending({ min: x0.min - dx, max: x0.max - dx });
    };
    const up = () => {
      uu.over.style.cursor = prevCursor;
      document.removeEventListener("mousemove", move);
      document.removeEventListener("mouseup", up);
    };
    document.addEventListener("mousemove", move);
    document.addEventListener("mouseup", up);
  }

  function onReady(uu) {
    // On the root, not the plot area, so the axis gutters route too.
    uu.root.addEventListener("wheel", e => onWheel(uu, e), { passive: false });
    uu.over.addEventListener("mousedown", e => startPan(uu, e));
    if (!measure.on) uu.over.style.cursor = "grab";
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

