// Zoom / pan behaviour of the exported HTML chart (converter/resources/
// scope_page.js), which has to match the Analyser's ScopePlot exactly:
//
//   wheel, no modifier, over the plot  -> zoom X and every Y about the cursor
//   Ctrl + wheel                       -> X only
//   Shift / Alt + wheel                -> all Y (one axis over a Y gutter)
//   wheel over a Y / X axis gutter     -> that axis only
//   left-drag                          -> pan (there is no box zoom)
//   step                               -> 0.85 per notch, continuous
//
// The reference is ScopePlot::eventFilter / ScopePlot::zoomAt in
// plot/src/ScopePlot.cpp; kZoomStep there is the 0.85 asserted below.
//
// It also covers the panel's cursor read-out on a mixed-rate union grid,
// where a slower channel is null at the grid points a faster one put there.
//
// Run: node tests/js/scope_page_interaction.test.js  (ctest name: scope_page_js)
//
// The DOM and uPlot here are stubs — just enough surface for makeApp() to run
// headlessly. The stub models the one uPlot contract this code leans on:
// committing a scale re-runs every y scale's range() callback, which is how a
// staged manual[] range reaches the chart.
const fs = require('fs');
const path = require('path');

const PLOT = { left: 100, top: 50, width: 800, height: 430 };
PLOT.right = PLOT.left + PLOT.width;
PLOT.bottom = PLOT.top + PLOT.height;

function mkEl(tag) {
  const el = {
    tagName: tag, className: '', title: '', value: '',
    type: '', checked: false, disabled: false, children: [],
    style: { cssText: '' }, _events: {},
    classList: { toggle() {}, add() {}, remove() {} },
    appendChild(c) { this.children.push(c); return c; },
    append(...c) { this.children.push(...c); },
    insertBefore(c) { this.children.unshift(c); return c; },
    addEventListener(t, fn) { (this._events[t] = this._events[t] || []).push(fn); },
    removeEventListener(t, fn) {
      const a = this._events[t] || []; const i = a.indexOf(fn); if (i >= 0) a.splice(i, 1);
    },
    dispatch(t, ev) { (this._events[t] || []).forEach(fn => fn(ev)); },
    getBoundingClientRect() {
      // Just enough vertical layout for the chart's fit-to-window sizing:
      // the plot area starts below the page header and toolbar, the canvas
      // sits inside it between 8px of padding, and the block adds the grab
      // strip and its border underneath.
      if (this.className === 'plotwrap') {
        const top = LAYOUT.wrapTop;
        const h = LAYOUT.padTop + LAYOUT.chartH + LAYOUT.padBottom;
        return { ...PLOT, top, bottom: top + h, height: h };
      }
      if (this.className === 'block') {
        const top = LAYOUT.blockTop;
        const bottom = LAYOUT.wrapTop + LAYOUT.padTop + LAYOUT.chartH
                     + LAYOUT.padBottom + LAYOUT.belowWrap;
        return { ...PLOT, top, bottom, height: bottom - top };
      }
      return { ...PLOT };
    },
    querySelector() { return null; },
    get clientWidth() { return PLOT.width + 20; },
  };
  // Assigning textContent replaces the element's contents — code that empties
  // a container with `el.textContent = ""` depends on the children going with
  // it, and a stub that keeps them hides exactly the bugs worth catching.
  let text = '';
  Object.defineProperty(el, 'textContent', {
    get() { return text; },
    set(v) { text = String(v); el.children.length = 0; },
  });
  return el;
}

// Vertical layout the stub reports. chartH is what uPlot's setSize writes back.
const LAYOUT = { blockTop: 90, wrapTop: 140, padTop: 8, padBottom: 8,
                 belowWrap: 8, chartH: 430 };
let VIEW_H = 1000;
global.getComputedStyle = () => ({ paddingTop: LAYOUT.padTop + 'px',
                                   paddingBottom: LAYOUT.padBottom + 'px' });

global.devicePixelRatio = 1;
const docEvents = {};
const byId = {};
global.document = {
  createElement: mkEl,
  getElementById: (id) => (byId[id] = byId[id] || mkEl('div')),
  addEventListener(t, fn) { (docEvents[t] = docEvents[t] || []).push(fn); },
  removeEventListener(t, fn) {
    const a = docEvents[t] || []; const i = a.indexOf(fn); if (i >= 0) a.splice(i, 1);
  },
  dispatch(t, ev) { (docEvents[t] || []).slice().forEach(fn => fn(ev)); },
  documentElement: { get clientHeight() { return VIEW_H; } },
};
const winEvents = {};
global.window = {
  addEventListener(t, fn) { (winEvents[t] = winEvents[t] || []).push(fn); },
  dispatch(t, ev) { (winEvents[t] || []).slice().forEach(fn => fn(ev)); },
  scrollY: 0,
};
global.WheelEvent = class { constructor(t, o) { Object.assign(this, o); this.type = t; } preventDefault() {} };
global.MouseEvent = class { constructor(t, o) { Object.assign(this, o); this.type = t; } preventDefault() {} };
global.KeyboardEvent = class { constructor(t, o) { Object.assign(this, o); this.type = t; } preventDefault() {} };

let INST = null;
const DRAWN = [];        // text the overlay painted, for the measure read-out
global.uPlot = function (opts, data) {
  const self = {
    opts, data, over: mkEl('div'), root: mkEl('div'),
    width: opts.width, height: opts.height,   // uPlot exposes both; the page
    axes: opts.axes, scales: {}, select: { width: 0, height: 0 },
  };
  // seed scales from data extents
  const ext = (arr) => {
    const v = arr.filter(x => x != null);
    return [Math.min(...v), Math.max(...v)];
  };
  const xs = data[0];
  self.scales.x = { min: xs[0], max: xs[xs.length - 1] };
  Object.keys(opts.scales).forEach(k => {
    if (k === 'x') return;
    self.scales[k] = { min: 0, max: 1, _opts: opts.scales[k] };
  });
  // which series feed which y scale
  const seriesFor = {};
  opts.series.forEach((s, i) => {
    if (!s.scale) return;
    (seriesFor[s.scale] = seriesFor[s.scale] || []).push(i);
  });
  function recomputeY() {
    Object.keys(self.scales).forEach(k => {
      if (k === 'x') return;
      const sc = self.scales[k];
      let dmin = null, dmax = null;
      (seriesFor[k] || []).forEach(i => {
        const col = data[i]; if (!col) return;
        const [lo, hi] = ext(col);
        dmin = dmin == null ? lo : Math.min(dmin, lo);
        dmax = dmax == null ? hi : Math.max(dmax, hi);
      });
      const r = sc._opts.range(self, dmin, dmax);
      sc.min = r[0]; sc.max = r[1];
    });
  }
  self.setScale = (key, o) => {
    if (key === 'x') { self.scales.x.min = o.min; self.scales.x.max = o.max; }
    recomputeY();                       // uPlot re-runs range() on any commit
  };
  self.posToVal = (pos, key) => {
    const s = self.scales[key];
    if (key === 'x') return s.min + (pos / PLOT.width) * (s.max - s.min);
    return s.max - (pos / PLOT.height) * (s.max - s.min);   // y is inverted
  };
  // uPlot's valToPos: value → position inside the plot area, in CSS pixels or
  // (with the third argument) canvas pixels. The measure tool's snapping, its
  // Shift lock and its right-click hit test are all pixel judgements, so a
  // stub returning 0 would make them meaningless.
  self.valToPos = (val, key, canvasPixels) => {
    const s = self.scales[key];
    const p = key === 'x'
      ? (val - s.min) / (s.max - s.min) * PLOT.width
      : (s.max - val) / (s.max - s.min) * PLOT.height;
    return canvasPixels ? p * devicePixelRatio : p;
  };
  self.setSeries = () => {};
  self.setSelect = () => {};
  self.redraw = () => {};
  self.destroy = () => {};
  self.setSize = (o) => {                     // reads u.height when dragging
    if (!o) return;
    if (o.width)  self.width  = o.width;
    if (o.height) { self.height = o.height; LAYOUT.chartH = o.height; }
  };
  self.cursor = { left: 0, top: 0, idx: null };
  self.ctx = { save(){}, restore(){}, beginPath(){}, arc(){}, fill(){}, stroke(){},
               moveTo(){}, lineTo(){}, fillRect(){}, strokeRect(){},
               fillText(t) { DRAWN.push(t); },
               setLineDash(){}, measureText: () => ({ width: 40 }) };
  recomputeY();
  // uPlot lays axes out in gutters; give each a band width like the real one
  self.axes.forEach((a, i) => { if (i > 0) a._size = 50; });
  if (opts.hooks && opts.hooks.ready) opts.hooks.ready.forEach(fn => fn(self));
  INST = self;
  return self;
};

// "use strict" in the page makes eval's declarations local, so run it in
// the global scope the way a <script> tag would.
const pageSrc = fs.readFileSync(path.join(__dirname, '..', '..', 'converter', 'resources', 'scope_page.js'), 'utf8');
const vm = require('vm');
vm.runInThisContext(pageSrc);
const makeApp = global.makeApp || globalThis.makeApp;

// --- build a two-axis time chart -------------------------------------------
const N = 2000;
const t = [], a = [], b = [];
for (let i = 0; i < N; i++) { t.push(i / 1000); a.push(Math.sin(i / 100)); b.push(200 + 50 * Math.sin(i / 100)); }
makeApp({
  view: 'time', xyChannel: '',
  time: {
    xLabel: 't [s]',
    axes: [{ label: 'Y1', right: false, color: '#1e1e1e' },
           { label: 'Y2', right: true,  color: '#d95319' }],
    series: [{ name: 'a', label: 'a [mm]', color: '#1e1e1e', axis: 0, visible: true },
             { name: 'b', label: 'b [bar]', color: '#d95319', axis: 1, visible: true }],
    data: [t, a, b],
  },
  frequency: null,
});

// fitAll() rebuilds the chart, so this is rebound after any check that
// fits (Home / double-click) — a stale instance would silently stop
// tracking the page.
let u = INST;
const snap = () => ({ x: [u.scales.x.min, u.scales.x.max],
                      y0: [u.scales.y0.min, u.scales.y0.max],
                      y1: [u.scales.y1.min, u.scales.y1.max] });
const W = r => r[1] - r[0];
const near = (p, q) => Math.abs(W(p) - W(q)) < 1e-9 && Math.abs(p[0] - q[0]) < 1e-9;
const shrank = (p, q) => W(q) < W(p) * 0.999;
const grew   = (p, q) => W(q) > W(p) * 1.001;

let fails = 0, total = 0;
function check(name, ok, detail) {
  total++;
  if (!ok) fails++;
  console.log((ok ? 'PASS  ' : 'FAIL  ') + name + (detail ? '   [' + detail + ']' : ''));
}
function wheel(o) {
  const cx = o.clientX != null ? o.clientX : PLOT.left + PLOT.width / 2;
  const cy = o.clientY != null ? o.clientY : PLOT.top + PLOT.height / 2;
  u.root.dispatch('wheel', new WheelEvent('wheel', {
    deltaY: o.deltaY, deltaMode: 0, clientX: cx, clientY: cy,
    ctrlKey: !!o.ctrl, shiftKey: !!o.shift, altKey: !!o.alt,
    preventDefault() {},
  }));
}

let p, q;
p = snap(); wheel({ deltaY: -100 }); q = snap();
check('wheel (no modifier) zooms X and BOTH Y — the app default',
      shrank(p.x, q.x) && shrank(p.y0, q.y0) && shrank(p.y1, q.y1),
      `x ${W(p.x).toFixed(3)}→${W(q.x).toFixed(3)}, y0 ${W(p.y0).toFixed(3)}→${W(q.y0).toFixed(3)}`);

p = snap(); wheel({ deltaY: -100, ctrl: true }); q = snap();
check('Ctrl+wheel → X only', shrank(p.x, q.x) && near(p.y0, q.y0) && near(p.y1, q.y1));

p = snap(); wheel({ deltaY: -100, shift: true }); q = snap();
check('Shift+wheel → all Y, X untouched',
      near(p.x, q.x) && shrank(p.y0, q.y0) && shrank(p.y1, q.y1));

p = snap(); wheel({ deltaY: -100, alt: true }); q = snap();
check('Alt+wheel → same as Shift (app accepts either)',
      near(p.x, q.x) && shrank(p.y0, q.y0) && shrank(p.y1, q.y1));

p = snap(); wheel({ deltaY: -100, clientX: PLOT.left - 20 }); q = snap();
check('wheel over the LEFT axis gutter → that axis only',
      near(p.x, q.x) && shrank(p.y0, q.y0) && near(p.y1, q.y1));

p = snap(); wheel({ deltaY: -100, clientX: PLOT.right + 20 }); q = snap();
check('wheel over the RIGHT axis gutter → that axis only',
      near(p.x, q.x) && near(p.y0, q.y0) && shrank(p.y1, q.y1));

p = snap(); wheel({ deltaY: -100, clientY: PLOT.bottom + 20 }); q = snap();
check('wheel over the X axis gutter → X only',
      shrank(p.x, q.x) && near(p.y0, q.y0) && near(p.y1, q.y1));

p = snap(); wheel({ deltaY: +100 }); q = snap();
check('wheel down zooms OUT', grew(p.x, q.x) && grew(p.y0, q.y0));

// anchored at the cursor: zoom at the far left edge barely moves xmin
p = snap(); wheel({ deltaY: -100, clientX: PLOT.left }); q = snap();
check('zoom is anchored at the cursor, not the centre',
      Math.abs(q.x[0] - p.x[0]) < W(p.x) * 1e-9,
      `xmin ${p.x[0].toFixed(6)} → ${q.x[0].toFixed(6)}`);

// factor matches the app: 0.85 per 100px notch
p = snap(); wheel({ deltaY: -100 }); q = snap();
check('one notch scales by 0.85 (app kZoomStep)',
      Math.abs(W(q.x) / W(p.x) - 0.85) < 1e-9,
      `ratio ${(W(q.x) / W(p.x)).toFixed(6)}`);
p = snap(); wheel({ deltaY: -50 }); q = snap();
check('half a notch scales by sqrt(0.85) — continuous like the app',
      Math.abs(W(q.x) / W(p.x) - Math.pow(0.85, 0.5)) < 1e-9,
      `ratio ${(W(q.x) / W(p.x)).toFixed(6)}`);

// drag pans
p = snap();
const cx = PLOT.left + PLOT.width / 2, cy = PLOT.top + PLOT.height / 2;
u.over.dispatch('mousedown', new MouseEvent('mousedown', { button: 0, clientX: cx, clientY: cy, preventDefault() {} }));
document.dispatch('mousemove', new MouseEvent('mousemove', { clientX: cx + 80, clientY: cy + 43 }));
document.dispatch('mouseup', new MouseEvent('mouseup', {}));
q = snap();
check('left-drag PANS: widths unchanged',
      Math.abs(W(q.x) - W(p.x)) < 1e-9 && Math.abs(W(q.y0) - W(p.y0)) < 1e-9);
check('drag right shows earlier X (content follows the pointer)',
      q.x[0] < p.x[0] - 1e-12,
      `xmin ${p.x[0].toFixed(4)} → ${q.x[0].toFixed(4)} (expect −${(80 / PLOT.width * W(p.x)).toFixed(4)})`);
check('drag down shows higher Y (content follows the pointer)',
      q.y0[0] > p.y0[0] + 1e-12,
      `y0min ${p.y0[0].toFixed(4)} → ${q.y0[0].toFixed(4)}`);
check('pan moves every Y axis, not just Y1', Math.abs(q.y1[0] - p.y1[0]) > 1e-12);
check('pan distance matches the pointer exactly',
      Math.abs((p.x[0] - q.x[0]) - (80 / PLOT.width) * W(p.x)) < 1e-9);

// ---------------------------------------------------------------------------
// Arrow keys pan, mirroring ScopePlot's shortcuts: one PAN_STEP per press,
// growing while the key repeats (KEY_BOOST per repeat inside KEY_RAMP_GAP,
// capped at HOLD_BOOST_MAX), reset by a pause or a change of direction.
const KEY_BOOST = 1.08, KEY_CAP = 8, PAN = 0.10;
let keyClock = 0;
function key(name, dtMs) {
  keyClock += (dtMs == null ? 1000 : dtMs);
  window.dispatch('keydown', new KeyboardEvent('keydown', {
    key: name, timeStamp: keyClock, target: { tagName: 'DIV' },
    preventDefault() {},
  }));
}

p = snap(); key('ArrowRight'); q = snap();
check('→ key pans one step right, exactly like the Move button',
      Math.abs(W(q.x) - W(p.x)) < 1e-9
      && Math.abs((q.x[0] - p.x[0]) - W(p.x) * PAN) < 1e-9,
      `moved ${(q.x[0] - p.x[0]).toFixed(6)}, one step is ${(W(p.x) * PAN).toFixed(6)}`);

p = snap(); key('ArrowLeft'); q = snap();
check('← key pans one step left', Math.abs((q.x[0] - p.x[0]) + W(p.x) * PAN) < 1e-9);

p = snap(); key('ArrowUp'); q = snap();
check('↑ key shows higher Y on every axis, X untouched — the ↑ button\'s way',
      near(p.x, q.x)
      && Math.abs((q.y0[0] - p.y0[0]) - W(p.y0) * PAN) < 1e-9
      && Math.abs((q.y1[0] - p.y1[0]) - W(p.y1) * PAN) < 1e-9,
      `y0 ${p.y0[0].toFixed(4)} → ${q.y0[0].toFixed(4)}`);

p = snap(); key('ArrowDown'); q = snap();
check('↓ key shows lower Y on every axis',
      near(p.x, q.x) && Math.abs((q.y0[0] - p.y0[0]) + W(p.y0) * PAN) < 1e-9);

// taps spaced out past the ramp gap never accelerate
p = snap(); key('ArrowRight', 1000); q = snap();
check('a lone tap after a pause is one plain step, never a boosted one',
      Math.abs((q.x[0] - p.x[0]) - W(p.x) * PAN) < 1e-9,
      `moved ${(q.x[0] - p.x[0]).toFixed(6)}`);

// repeats inside the gap grow the step
p = snap(); key('ArrowRight', 30); q = snap();
check('a repeat inside the ramp gap moves further than the press before it',
      Math.abs((q.x[0] - p.x[0]) - W(p.x) * PAN * KEY_BOOST) < 1e-9,
      `moved ${(q.x[0] - p.x[0]).toFixed(6)}, expected ` +
      `${(W(p.x) * PAN * KEY_BOOST).toFixed(6)}`);
p = snap(); key('ArrowRight', 30); q = snap();
check('and the next one further still',
      Math.abs((q.x[0] - p.x[0]) - W(p.x) * PAN * KEY_BOOST * KEY_BOOST) < 1e-9);

// a gap longer than KEY_RAMP_GAP starts over
p = snap(); key('ArrowRight', 400); q = snap();
check('letting go (a gap past 300 ms) resets to one step',
      Math.abs((q.x[0] - p.x[0]) - W(p.x) * PAN) < 1e-9,
      `moved ${(q.x[0] - p.x[0]).toFixed(6)}`);

// changing direction mid-ramp starts over too
key('ArrowRight', 30); key('ArrowRight', 30);
p = snap(); key('ArrowLeft', 30); q = snap();
check('turning around starts the ramp over rather than inheriting it',
      Math.abs((p.x[0] - q.x[0]) - W(p.x) * PAN) < 1e-9,
      `moved ${(p.x[0] - q.x[0]).toFixed(6)}`);

// held long enough, the step tops out at the same cap the buttons use
for (let i = 0; i < 200; i++) key('ArrowRight', 30);
p = snap(); key('ArrowRight', 30); q = snap();
check('a held arrow tops out at the ×8 cap, not at runaway speed',
      Math.abs((q.x[0] - p.x[0]) - W(p.x) * PAN * KEY_CAP) < 1e-9,
      `moved ${(q.x[0] - p.x[0]).toFixed(6)}, cap is ` +
      `${(W(p.x) * PAN * KEY_CAP).toFixed(6)}`);

// keys that belong to the page (or to a text box) are left alone
p = snap();
key('ArrowRight', 1000);            // re-arm: this one is a real pan
const oneTap = snap().x[0] - p.x[0];
p = snap();
window.dispatch('keydown', new KeyboardEvent('keydown', {
  key: 'ArrowRight', timeStamp: (keyClock += 30),
  target: { tagName: 'INPUT' }, preventDefault() {} }));
q = snap();
check('arrows typed into an input box do not pan the chart', near(p.x, q.x));
window.dispatch('keydown', new KeyboardEvent('keydown', {
  key: 'ArrowRight', timeStamp: (keyClock += 30), ctrlKey: true,
  target: { tagName: 'DIV' }, preventDefault() {} }));
check('Ctrl+arrow is left to the browser', near(p.x, snap().x));
check('the ignored presses did not feed the ramp either',
      Math.abs((snap().x[0] - p.x[0])) < 1e-12, `${oneTap.toFixed(6)}`);

// Home / +/− match the app's other shortcuts
p = snap(); key('-', 1000); q = snap();
check('the − key zooms both axes out', grew(p.x, q.x) && grew(p.y0, q.y0));
p = snap(); key('+', 1000); q = snap();
check('the + key zooms both axes in', shrank(p.x, q.x) && shrank(p.y0, q.y0));
key('Home', 1000);
u = INST;                                  // fitAll() rebuilt the chart
check('Home fits the data again',
      Math.abs(u.scales.x.min - t[0]) < 1e-9
      && Math.abs(u.scales.x.max - t[t.length - 1]) < 1e-9,
      `x ${u.scales.x.min.toFixed(4)} … ${u.scales.x.max.toFixed(4)}`);

// ---------------------------------------------------------------------------
// The measure tool reads every Y axis, not just the first. A click stores a y
// per axis, so one pair of clicks reports a Δy for each — the ScopePlot rule.
// Δx from the read-out. The chart's samples are 1 ms apart, so a snapped pair
// lands on that grid and a freely placed one almost never does.
function readDx() {
  const line = measureText().find(t => /^(#\d+  )?Δx = /.test(t));
  return line ? parseFloat(line.split('= ')[1]) : NaN;
}
const onSampleGrid = v => Math.abs(v / 0.001 - Math.round(v / 0.001)) < 1e-6;
const fmtNum = v => Number(v.toPrecision(6)).toString();   // the page's fmt()
function measureText() {
  DRAWN.length = 0;
  u.opts.hooks.draw.forEach(fn => fn(u));
  return DRAWN.slice();
}
const mrows = () => collect(byId.charts, 'mtable')[0];
const cellsOf = (rowIdx) => {
  const tbl = mrows().children[0];
  return (tbl.children[rowIdx].children || []).map(td => td.textContent);
};
function clickPlot(fx, fy) {
  u.over.dispatch('click', new MouseEvent('click', {
    clientX: PLOT.left + fx * PLOT.width,
    clientY: PLOT.top + fy * PLOT.height,
  }));
}

key('Home', 1000);                          // known ranges to measure against
u = INST;
const mBtn = findBtn('Δ Measure');
check('the Measure toggle is on the toolbar', !!mBtn);
mBtn.dispatch('click', {});
const yA = [u.scales.y0.min, u.scales.y0.max];
clickPlot(0.25, 0.75);
check('one point alone draws no read-out', measureText().length === 0);
clickPlot(0.75, 0.25);
// Aiming at a sample: 5px of x here is a dozen samples wide, so nudging x
// would snap to a different (equally correct) sample and prove nothing, while
// at a peak of the sine the neighbours are level and the nearest sample is
// unambiguous.
const PEAK_A = 157, PEAK_B = 785;            // sin(i/100) crests
const lift = 5 / PLOT.height;
const fxOf = x => (x - u.scales.x.min) / (u.scales.x.max - u.scales.x.min);
const fyOf = v => (u.scales.y0.max - v) / (u.scales.y0.max - u.scales.y0.min);

let out = measureText();
check('the read-out carries exactly one Δy, in the anchor axis',
      out.filter(t => /^Δy/.test(t)).length === 1
      && out.some(t => t.startsWith('Δy Y1 =')),
      out.join(' | '));
// The label carries 6 significant digits, so compare at that precision.
const near6 = (got, want) => Math.abs(got - want) <= Math.abs(want) * 1e-5;
const dyLine = () => {
  const line = measureText().find(t => /^Δy/.test(t));
  return line ? parseFloat(line.split('= ')[1]) : NaN;
};
check('freely placed, it reads in the first axis units',
      near6(dyLine(), W(yA) * 0.5), `${dyLine()} vs ${W(yA) * 0.5}`);

// There is no axis picker any more: a measurement is drawn in the axis of
// the channel it snapped to, and the table lists every channel on the chart.
check('the axis picker is gone',
      !collect(byId.charts, 'tbtn').some(
          b => (b.textContent || '').startsWith('Δy:'))
      && !collect(byId.charts, 'ddmenu').length);
check('the table lists every visible channel, not a picked subset',
      mrows().children[0].children.length === 3
      && cellsOf(1)[0] === 'a [mm]' && cellsOf(2)[0] === 'b [bar]',
      cellsOf(1)[0] + ' / ' + cellsOf(2)[0]);

// Unticking a channel takes it out of the table — the channel list is the
// only selection there is.
const crows = collect(byId.charts, 'crow');
const tick = (i, on) => { const cb = crows[i].children[0];
                          cb.checked = on; cb.dispatch('change', {}); };
tick(0, false);
check('hiding a channel drops its row from the table',
      mrows().children[0].children.length === 2 && cellsOf(1)[0] === 'b [bar]',
      cellsOf(1)[0]);

// With only the Y2 channel left to snap to, a measurement started on it is
// read in Y2's units. (The two channels are the same sine, so while both are
// drawn they sit on top of each other and either could legitimately win.)
const fy1Of = v => (u.scales.y1.max - v) / (u.scales.y1.max - u.scales.y1.min);
mBtn.dispatch('click', {}); mBtn.dispatch('click', {});   // clear
clickAt(fxOf(t[PEAK_A]), fy1Of(b[PEAK_A]) - lift);
clickAt(fxOf(t[PEAK_B]), fy1Of(b[PEAK_B]) - lift);
out = measureText();
check('a measurement started on a Y2 channel is read in Y2 units',
      out.some(t => t.startsWith('Δy Y2 =')), out.join(' | '));
check('and its Δy is that channel\'s own difference',
      near6(dyLine(), b[PEAK_B] - b[PEAK_A]),
      `${dyLine()} vs ${b[PEAK_B] - b[PEAK_A]}`);

tick(0, true);
check('showing the channel again brings its row back',
      mrows().children[0].children.length === 3);

// The table folds away, giving its space back to the chart.
const foldBtn = collect(byId.charts, 'tbtn').find(
    b => (b.textContent || '').includes('Measurement'));
check('the measurement table has a fold strip', !!foldBtn,
      foldBtn && foldBtn.textContent);
foldBtn.dispatch('click', {});
check('folding hides the table but keeps the strip',
      mrows().style.display === 'none' && foldBtn.textContent.startsWith('▸'),
      foldBtn.textContent);
foldBtn.dispatch('click', {});
check('unfolding brings the numbers back',
      mrows().style.display !== 'none' && cellsOf(1)[0] === 'a [mm]',
      foldBtn.textContent);
check('Δx and the frequency ride along',
      measureText().some(t => t.startsWith('Δx = '))
      && measureText().some(t => t.startsWith('1/|Δx| = ')),
      measureText().join(' | '));
// --- snapping, locking, several at once, and the per-channel table ---------
// The data is a ramp so every number below is checkable by hand: t goes 0,
// 0.001, ... and 'a'/'b' are the sine pair the chart was built from, so these
// checks use explicit clicks on known x positions instead.
function clickAt(fx, fy, opts) {
  const o = opts || {};
  u.over.dispatch('click', new MouseEvent('click', {
    clientX: PLOT.left + fx * PLOT.width,
    clientY: PLOT.top + fy * PLOT.height,
    altKey: !!o.alt, shiftKey: !!o.shift,
  }));
}
function rightClickAt(fx, fy) {
  u.over.dispatch('contextmenu', {
    clientX: PLOT.left + fx * PLOT.width,
    clientY: PLOT.top + fy * PLOT.height,
    preventDefault() {},
  });
}

mBtn.dispatch('click', {});                 // leave measure mode
check('leaving measure mode clears the markers', measureText().length === 0);

mBtn.dispatch('click', {});                 // and back in, for the rest
clickAt(0.25, 0.5, { alt: true });
clickAt(0.75, 0.3, { alt: true });
let table = mrows();
check('a finished measurement fills the table under the chart',
      !!table && table.style.display !== 'none'
      && table.children.length === 1, table && table.style.display);
check('the table has a header and one row per visible channel',
      mrows().children[0].children.length === 3,
      `${mrows().children[0].children.length} rows`);
check('the header names every column',
      cellsOf(0).join(',') === 'Channel,@x1,@x2,Δ,Δ/Δx,ratio,dB,min,max,p-p,mean,RMS,σ,∫,n',
      cellsOf(0).join(','));
check('each row starts with the channel and ends with a sample count',
      cellsOf(1)[0] === 'a [mm]' && +cellsOf(1)[14] > 0, cellsOf(1).join(' | '));

// Alt placed the points freely; without it a click near the trace snaps onto
// the sample itself. Aim a few pixels off two known samples of channel 'a'.
// Aim 5px above two peaks of the sine (see the helpers above).
mBtn.dispatch('click', {}); mBtn.dispatch('click', {});   // clear
clickAt(fxOf(t[PEAK_A]), fyOf(a[PEAK_A]) - lift);
clickAt(fxOf(t[PEAK_B]), fyOf(a[PEAK_B]) - lift);
check('a click near the trace snaps onto the sample itself',
      onSampleGrid(readDx())
      && Math.abs(readDx() - (t[PEAK_B] - t[PEAK_A])) < 1e-9,
      `Δx = ${readDx()}, samples are ${(t[PEAK_B] - t[PEAK_A]).toFixed(4)} apart`);
check('so the value read at the marker is the sample, not an interpolation',
      cellsOf(1)[1] === fmtNum(a[PEAK_A]) && cellsOf(1)[2] === fmtNum(a[PEAK_B]),
      `${cellsOf(1)[1]}/${cellsOf(1)[2]} vs ${fmtNum(a[PEAK_A])}/${fmtNum(a[PEAK_B])}`);
mBtn.dispatch('click', {}); mBtn.dispatch('click', {});
clickAt(0.2503, 0.5, { alt: true });         // Alt → wherever the mouse is
clickAt(0.7517, 0.3, { alt: true });
check('Alt places freely, between samples', !onSampleGrid(readDx()),
      `Δx = ${readDx()}`);

// Shift locks the second point: a mostly-horizontal pair has Δy = 0.
mBtn.dispatch('click', {}); mBtn.dispatch('click', {});
clickAt(0.2, 0.5, { alt: true });
clickAt(0.8, 0.55, { alt: true, shift: true });
check('Shift locks a mostly-horizontal pair to a pure Δx',
      measureText().some(t => /^Δy Y1 = 0$/.test(t)), measureText().join(' | '));
mBtn.dispatch('click', {}); mBtn.dispatch('click', {});
clickAt(0.5, 0.2, { alt: true });
clickAt(0.53, 0.9, { alt: true, shift: true });
check('and a mostly-vertical pair to a pure Δy',
      measureText().some(t => /^Δx = 0$/.test(t)), measureText().join(' | '));

// Several measurements at once, and right-click taking back just one.
mBtn.dispatch('click', {}); mBtn.dispatch('click', {});
clickAt(0.1, 0.4, { alt: true }); clickAt(0.3, 0.6, { alt: true });
clickAt(0.6, 0.4, { alt: true }); clickAt(0.9, 0.6, { alt: true });
check('a click past a finished pair starts another measurement',
      measureText().filter(t => t.startsWith('#')).length === 2,
      measureText().join(' | '));
rightClickAt(0.1, 0.4);
check('right-click takes back only the measurement under the cursor',
      measureText().filter(t => /^Δx = /.test(t)).length === 1,
      measureText().join(' | '));
rightClickAt(0.02, 0.02);
check('right-click away from any of them clears the lot',
      measureText().length === 0, measureText().join(' | '));
check('the table keeps its rows, blank — the chart must not resize under the '
      + 'cursor between two clicks',
      mrows().style.display !== 'none' && cellsOf(1)[1] === '—',
      cellsOf(1).join(' | '));

// The copied report carries the same numbers.
clickAt(0.25, 0.5, { alt: true }); clickAt(0.75, 0.3, { alt: true });
let copied = '';
global.navigator = { clipboard: { writeText(t) { copied = t; return Promise.resolve(); } } };
findBtn('⧉ Copy').dispatch('click', {});
check('Copy puts the deltas and the table on the clipboard',
      copied.includes('Δx = ') && copied.includes('Channel\t@x1')
      && copied.split('\n').length > 4,
      JSON.stringify(copied.slice(0, 60)));
mBtn.dispatch('click', {});                 // done measuring

// ---------------------------------------------------------------------------
// Holding an arrow takes the wheel over: a notch moves the view along that
// arrow instead of zooming, backwards goes back, and the notches feed the same
// ramp — ScopePlot::eventFilter routes it identically.
function keyUp(name) {
  window.dispatch('keyup', new KeyboardEvent('keyup', { key: name }));
}
function wheelAt(deltaY, opts) {
  const o = opts || {};
  keyClock += (o.dt == null ? 30 : o.dt);
  u.root.dispatch('wheel', new WheelEvent('wheel', {
    deltaY, deltaMode: 0,
    clientX: PLOT.left + PLOT.width / 2, clientY: PLOT.top + PLOT.height / 2,
    ctrlKey: !!o.ctrl, shiftKey: !!o.shift, timeStamp: keyClock,
    preventDefault() {},
  }));
}

key('ArrowRight', 1000);                    // and hold it
p = snap(); wheelAt(-100); q = snap();      // one notch up
check('scrolling with → held moves the view instead of zooming',
      Math.abs(W(q.x) - W(p.x)) < 1e-9 && Math.abs(W(q.y0) - W(p.y0)) < 1e-9
      && q.x[0] > p.x[0] + 1e-12,
      `width ${W(p.x).toFixed(4)} → ${W(q.x).toFixed(4)}, ` +
      `xmin ${p.x[0].toFixed(4)} → ${q.x[0].toFixed(4)}`);

p = snap(); wheelAt(+100); q = snap();      // one notch down
check('rolling the wheel backwards moves back the other way',
      Math.abs(W(q.x) - W(p.x)) < 1e-9 && q.x[0] < p.x[0] - 1e-12);

p = snap(); wheelAt(-100, { ctrl: true }); wheelAt(-100, { shift: true }); q = snap();
check('a held arrow beats Ctrl and Shift — nothing zooms while it is down',
      Math.abs(W(q.x) - W(p.x)) < 1e-9 && Math.abs(W(q.y0) - W(p.y0)) < 1e-9,
      `x ${W(p.x).toFixed(4)} → ${W(q.x).toFixed(4)}`);

p = snap(); wheelAt(-100); const n1 = snap().x[0] - p.x[0];
p = snap(); wheelAt(-100); const n2 = snap().x[0] - p.x[0];
check('each notch covers more ground than the one before',
      n2 > n1 * 1.0001, `${n1.toFixed(5)} then ${n2.toFixed(5)}`);

keyUp('ArrowRight');
p = snap(); wheelAt(-100, { dt: 1000 }); q = snap();
check('letting the arrow go makes the wheel a zoom again',
      shrank(p.x, q.x) && shrank(p.y0, q.y0),
      `x ${W(p.x).toFixed(4)} → ${W(q.x).toFixed(4)}`);

key('ArrowUp', 1000);                       // hold ↑ instead
p = snap(); wheelAt(-100); q = snap();
check('scrolling with ↑ held moves every Y axis, X untouched',
      near(p.x, q.x) && Math.abs(W(q.y0) - W(p.y0)) < 1e-9
      && q.y0[0] > p.y0[0] + 1e-12 && q.y1[0] > p.y1[0] + 1e-12,
      `y0 ${p.y0[0].toFixed(4)} → ${q.y0[0].toFixed(4)}`);
keyUp('ArrowUp');

// window.blur clears it too — a release that lands on another window
key('ArrowRight', 1000);
window.dispatch('blur', {});
p = snap(); wheelAt(-100, { dt: 1000 }); q = snap();
check('losing the window with a key down does not leave the wheel stuck',
      shrank(p.x, q.x) && shrank(p.y0, q.y0));

// mouseup detaches the listeners — a later move must not keep panning
p = snap();
document.dispatch('mousemove', new MouseEvent('mousemove', { clientX: cx + 300, clientY: cy }));
q = snap();
check('pan stops on mouseup (listeners detached)', near(p.x, q.x));

// ---------------------------------------------------------------------------
// The chart fills the window the way the app's plot fills its own, instead of
// sitting in a fixed 430px band with the rest of a tall screen left empty.
function findByClass(cls) {
  const hit = [];
  (function walk(el) {
    if (!el || !el.children) return;
    if (el.className === cls) hit.push(el);
    el.children.forEach(walk);
  })(byId.charts);
  return hit[0];
}
// canvasTop = wrapTop + padTop; below = padBottom + belowWrap; margin = 24.
const fitted = (viewH) => viewH - 24 - (LAYOUT.wrapTop + LAYOUT.padTop)
                        - LAYOUT.padBottom - LAYOUT.belowWrap;

check('the chart is sized to the window, not to a fixed 430',
      LAYOUT.chartH === fitted(VIEW_H),
      `${LAYOUT.chartH}, expected ${fitted(VIEW_H)}`);
check('that is taller than the old fixed band', LAYOUT.chartH > 430,
      `${LAYOUT.chartH}`);

VIEW_H = 1400;
window.dispatch('resize', {});
check('a taller window gives a taller chart', LAYOUT.chartH === fitted(1400),
      `${LAYOUT.chartH}, expected ${fitted(1400)}`);

// The height is the window's, always — there is nothing to drag and so
// nothing that can pin a stale height.
check('the chart has no manual resize strip left', !findByClass('vgrip'));
VIEW_H = 900;
window.dispatch('resize', {});
check('every window change re-fits the chart', LAYOUT.chartH === fitted(900),
      `${LAYOUT.chartH}, expected ${fitted(900)}`);

VIEW_H = 300;
window.dispatch('resize', {});
check('a tiny window still leaves a usable chart, not a sliver',
      LAYOUT.chartH === 220, `${LAYOUT.chartH}`);
VIEW_H = 1000;
window.dispatch('resize', {});

// Everything below adds a second chart to the page, and only a lone chart can
// have the window to itself — so the sizing checks above must run first.

// --- cursor read-out over a mixed-rate union grid ---------------------------
// "slow" samples every 10 ms and only over 0.02 s .. 0.08 s; "fast" samples
// every 5 ms across the whole window. On their union grid slow is null at
// every odd index — the read-out must still report the value the drawn line
// carries there (interpolated, marked "≈") instead of going blank.
const GRID = 21;                                   // 0 .. 100 ms, 5 ms apart
const gt = [], slow = [], fast = [];
for (let i = 0; i < GRID; i++) {
  gt.push(i / 200);
  slow.push(i % 2 === 0 && i >= 4 && i <= 16 ? 100 * (i / 200) : null);
  fast.push(i);
}
const chartsHost = document.getElementById('charts');
const nBlocks = chartsHost.children.length;
makeApp({
  view: 'time', xyChannel: '',
  time: {
    xLabel: 't [s]',
    axes: [{ label: 'Y1', right: false, color: '#1e1e1e' }],
    series: [{ name: 'slow', label: 'slow [mm]', color: '#1e1e1e', axis: 0, visible: true },
             { name: 'fast', label: 'fast [mm]', color: '#d95319', axis: 0, visible: true }],
    data: [gt, slow, fast],
  },
  frequency: null,
});
const u2 = INST;
function collect(el, cls, out) {
  out = out || [];
  if (el.className === cls) out.push(el);
  (el.children || []).forEach(c => collect(c, cls, out));
  return out;
}
const cells = collect(chartsHost.children[nBlocks], 'cval');
check('read-out has one value cell per channel', cells.length === 2,
      `${cells.length} cells`);
const readAt = i => {
  u2.cursor.idx = i;
  u2.opts.hooks.setCursor.forEach(fn => fn(u2));
  return cells.map(c => c.textContent);
};

let r = readAt(6);                                  // slow has a sample here
check('sampled point reads the sample itself, unmarked',
      r[0] === '3' && r[1] === '6', `[${r.join(' | ')}]`);

r = readAt(5);                                      // slow is null here
check('gap between the slow channel\'s samples reads the interpolated value',
      r[0] === '≈ 2.5' && r[1] === '5', `[${r.join(' | ')}]`);

r = readAt(2);                                      // before slow's first sample
check('before a channel\'s first sample it stays blank, others still read',
      r[0] === '' && r[1] === '2', `[${r.join(' | ')}]`);

r = readAt(18);                                     // after slow's last sample
check('after a channel\'s last sample it stays blank',
      r[0] === '' && r[1] === '18', `[${r.join(' | ')}]`);

r = readAt(null);                                   // pointer off the plot
check('no cursor → every cell blank', r[0] === '' && r[1] === '',
      `[${r.join(' | ')}]`);

// ---------------------------------------------------------------------------
// Press-and-hold on a Zoom / Move button repeats and accelerates, matching the
// kHoldDelayMs / kHoldStartMs / kHoldMinMs / kHoldDecay ramp in ScopePlot.cpp.
// Timers are faked so the ramp is asserted exactly, not raced against.
const realSetTimeout = global.setTimeout, realClearTimeout = global.clearTimeout;
let clock = 0, timerId = 1, timers = [];
const gaps = [];
global.setTimeout = (fn, ms) => {
  gaps.push(ms);
  const id = timerId++;
  timers.push({ id, at: clock + ms, fn });
  return id;
};
global.clearTimeout = (id) => {
  const i = timers.findIndex(t => t.id === id);
  if (i >= 0) timers.splice(i, 1);
};
function advance(ms) {
  const end = clock + ms;
  for (;;) {
    timers.sort((a, b) => a.at - b.at);
    if (!timers.length || timers[0].at > end) break;
    const t = timers.shift();
    clock = t.at;
    t.fn();
  }
  clock = end;
}

function findBtn(text) {
  const hit = [];
  (function walk(el) {
    if (!el || !el.children) return;
    if (el.className === 'tbtn' && el.textContent === text) hit.push(el);
    el.children.forEach(walk);
  })(byId.charts);
  return hit[0];
}
const press = (b) => b.dispatch('pointerdown',
                                { button: 0, preventDefault() {} });
const release = (b) => b.dispatch('pointerup', {});

const right = findBtn('\u2192'), zoomIn = findBtn('\u2194 +');
const fit = findBtn('\u2922');
check('the Move and Zoom buttons are on the toolbar', !!right && !!zoomIn);

// a plain click acts exactly once
gaps.length = 0;
p = snap(); press(right); release(right); advance(5000); q = snap();
const oneStep = W(p.x) * 0.10;
check('a click pans one step and stops — the hold never starts',
      Math.abs((q.x[0] - p.x[0]) - oneStep) < 1e-9,
      `moved ${(q.x[0] - p.x[0]).toFixed(6)}, one step is ${oneStep.toFixed(6)}`);

// held, but let go before the delay is up
gaps.length = 0;
p = snap(); press(right); advance(400); release(right); advance(5000); q = snap();
check('releasing before 500 ms still pans only once',
      Math.abs((q.x[0] - p.x[0]) - W(p.x) * 0.10) < 1e-9,
      `moved ${(q.x[0] - p.x[0]).toFixed(6)}`);
check('the first wait is the 500 ms delay', gaps[0] === 500, `got ${gaps[0]}`);

// held past the delay: it repeats, and each gap is shorter than the last
gaps.length = 0;
p = snap(); press(right); advance(500 + 800);   // at the floor, boost still climbing
const ramp = gaps.slice(1);
check('holding past 500 ms keeps moving', ramp.length > 1,
      `${ramp.length} repeats`);
check('the first repeat waits 140 ms', ramp[0] === 140, `got ${ramp[0]}`);
check('every repeat is faster than the one before, down to the floor',
      ramp.every((g, i) => i === 0 || g < ramp[i - 1] || g === 16),
      ramp.slice(0, 8).join(','));
check('the ramp bottoms out at one frame (16 ms) and stays there',
      Math.min(...ramp) === 16 && ramp[ramp.length - 1] === 16,
      `min ${Math.min(...ramp)}, last ${ramp[ramp.length - 1]}`);
check('a held button covers far more ground than a click',
      (snap().x[0] - p.x[0]) > oneStep * 5,
      `moved ${(snap().x[0] - p.x[0]).toFixed(4)} vs one step ${oneStep.toFixed(4)}`);

// past the floor the interval can't shrink further, so the step grows instead:
// the same 250 ms of holding must cover more ground later than earlier
const farA = snap().x[0];
advance(250);
const farB = snap().x[0];
advance(250);
const farC = snap().x[0];
check('past the interval floor the step keeps growing',
      (farC - farB) > (farB - farA) * 1.2,
      `${(farB - farA).toFixed(3)} then ${(farC - farB).toFixed(3)}`);
// and it stops growing at the cap rather than running away
advance(4000);
const capA = snap().x[0];
advance(250);
const capB = snap().x[0];
advance(250);
const capC = snap().x[0];
// 250 ms is 15.6 repeats at the 16 ms floor, so consecutive windows differ by
// at most the one tick that quantisation moves across the boundary.
check('the boost tops out instead of growing without bound',
      Math.abs((capC - capB) - (capB - capA)) <= oneStep * 8 * 1.000001,
      `${(capB - capA).toFixed(3)} then ${(capC - capB).toFixed(3)}, ` +
      `one capped step is ${(oneStep * 8).toFixed(3)}`);
check('top speed is the capped step at one frame per repeat',
      Math.abs((capC - capB) - oneStep * 8 * (250 / 16)) < oneStep * 8,
      `${(capC - capB).toFixed(3)} in 250 ms, cap predicts ` +
      `${(oneStep * 8 * (250 / 16)).toFixed(3)}`);

// release stops it dead
release(right);
p = snap(); advance(10000); q = snap();
check('release stops the repeat — no drift afterwards',
      Math.abs(q.x[0] - p.x[0]) < 1e-12);

// the ramp resets, so the next hold starts slow again
gaps.length = 0;
press(right); advance(1);
check('the next hold starts from the 500 ms delay again', gaps[0] === 500,
      `got ${gaps[0]}`);
release(right);

// zoom holds the same way, and compounds
gaps.length = 0;
p = snap(); press(zoomIn); advance(500 + 140); q = snap();
check('holding Zoom repeats too', W(q.x) < W(p.x) * 0.85 * 0.999,
      `width ${W(p.x).toFixed(4)} → ${W(q.x).toFixed(4)}`);
release(zoomIn);

// Fit is a one-shot action and must not repeat
gaps.length = 0;
if (fit) { press(fit); advance(5000); release(fit); }
check('Fit does not repeat on hold', gaps.length === 0,
      `${gaps.length} timers armed`);

global.setTimeout = realSetTimeout;
global.clearTimeout = realClearTimeout;

console.log('\n' + (fails ? fails + ' FAILING' : 'all ' + total + ' checks passed'));
process.exit(fails ? 1 : 0);
