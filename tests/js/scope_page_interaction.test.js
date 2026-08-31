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
    tagName: tag, className: '', textContent: '', title: '', value: '',
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
    getBoundingClientRect() { return { ...PLOT }; },
    querySelector() { return null; },
    get clientWidth() { return PLOT.width + 20; },
  };
  return el;
}

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
};
global.window = { addEventListener() {} };
global.WheelEvent = class { constructor(t, o) { Object.assign(this, o); this.type = t; } preventDefault() {} };
global.MouseEvent = class { constructor(t, o) { Object.assign(this, o); this.type = t; } preventDefault() {} };

let INST = null;
global.uPlot = function (opts, data) {
  const self = {
    opts, data, over: mkEl('div'), root: mkEl('div'),
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
  self.valToPos = () => 0;
  self.setSeries = () => {};
  self.setSelect = () => {};
  self.redraw = () => {};
  self.destroy = () => {};
  self.setSize = () => {};
  self.cursor = { left: 0, top: 0, idx: null };
  self.ctx = { save(){}, restore(){}, beginPath(){}, arc(){}, fill(){}, stroke(){},
               moveTo(){}, lineTo(){}, fillRect(){}, strokeRect(){}, fillText(){},
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

const u = INST;
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

// mouseup detaches the listeners — a later move must not keep panning
p = snap();
document.dispatch('mousemove', new MouseEvent('mousemove', { clientX: cx + 300, clientY: cy }));
q = snap();
check('pan stops on mouseup (listeners detached)', near(p.x, q.x));

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

console.log('\n' + (fails ? fails + ' FAILING' : 'all ' + total + ' checks passed'));
process.exit(fails ? 1 : 0);
