# 2026-06-12 09:46 — executed

- Read/inventoried: FunctionRegistry.cpp (all 52 functions), FormulaEngine
  parser/operators, AnalyserPlot display pipeline, SignalIO save/load,
  CsvWriter time-origin handling, all 11 existing test files.
- **New:** `tests/test_function_correctness.cpp` (19 tests): Integral/
  Derivative vs closed forms, rolling Mean/RMS exact window semantics,
  trig/exp/log identities, Shift, Power/Mod, parser precedence +
  scientific literals, FFT DC + exact bin frequencies, RFFT phase
  (cos→0, sin→−π/2), FFTKeep amplitude isolation, division-by-zero
  characterization, 3 DISABLED bug-pinning tests (Slice, BandKeep,
  revertFFT).
- **Extended:** `tests/test_analyser_layout.cpp` (+6): time-view
  ns→relative-seconds mapping (incl. inter-channel offset), frequency-view
  Hz axis, XY same-grid pairing, XY cross-grid interpolation, visibility
  filtering, DISABLED XY-fabrication test. Fixture gained friend-access
  display helpers.
- **Extended:** `tests/test_csv_writer.cpp` (+2): export custom range on
  0-based data (passes), DISABLED epoch-based range test.
- tests/CMakeLists.txt registers the new file.
- Suite: **192/192 enabled tests pass**; all 5 DISABLED tests fail exactly
  as their findings predict (reproduced; output in session log).
- One test iteration: the FFT bin-exactness test initially used fs=1024 Hz,
  whose period isn't integer-ns-representable — rewritten on a 1 kHz grid
  and the nuance recorded as finding B8.
- Findings B1–B9 + fix plan written into `20260612_0946_Plans.md`.
  **No production code changed** — fixes awaited user approval.

## Fix execution (same session, after user approval "ok do it")

- Committed + pushed the UI redesign and the test suite first
  (a48169e..5c70129), as requested.
- **B1** `SignalIO.cpp`: custom export range resolves against the earliest
  first sample of the exported time channels.
- **B2** `FunctionRegistry.cpp`: Slice + BandZero/BandKeep select with
  seconds relative to the signal's first sample on time-domain signals
  (Hz on spectra unchanged); help texts updated.
- **B3** `Signal::Meta::sourceStartNs` (in-memory, not persisted): set by
  RFFTmag/RFFTphase/FFT/FFTWelch, propagated through spectrum edits,
  consumed by revertFFT → reconstruction starts on the original axis.
- **B4** `AnalyserPlot.cpp` interpAt: NaN (gap) outside Y's range instead
  of clamped fabricated points.
- **B5** division and Mod by zero → NaN (decision: NaN over silent 0).
- **B6** same-grid fast path now requires full timestamp memcmp (engine +
  registry).
- **B7** crosshair read-out picks the nearest sample, not the next-right.
- **B8** spectrum bin timestamps via llround (FFT/FFTWelch/RFFT).
- **B9** H5 selector shows "… Hz span" for frequency-domain channels.
- **B10 (new, found during implementation)**: elementwise/shape-preserving
  functions dropped Meta.domain (spectrum → Time silently). Domain +
  sourceStartNs now propagate through elementwiseUnary, rolling stats,
  Limit, Shift, Resample, Reverse, ForwardFill, and both elementwiseBinary
  implementations.
- Tests: all 5 DISABLED tests enabled (regression guards), division test
  asserts NaN, +2 new tests (origin survives BandZero/arithmetic edits;
  domain preservation across 5 function classes). **199/199 green**;
  offscreen GUI smoke run OK.

## Follow-up (user report): sliced channel slid around depending on visibility

Cause: the chart's X origin was "earliest first sample among the VISIBLE
channels", so hiding the source made the slice itself the origin and it
slid from 30–50 s to 0–20 s. Fix: derived signals that drop leading
samples (Slice, Gate) record their source's origin in Meta::sourceStartNs
(sentinel kNoSourceStart = unset, since 0 is a real origin for 0-based
imports); the time-view axis anchors on Signal::displayOriginNs()
(origin if known, else own first sample); Slice/BandZero/BandKeep windows
select in those same chart coordinates — correct even when chained
(Slice of a Slice measures from the original origin); filters, calculus
and arithmetic propagate the anchor. +4 regression tests (chained slice
windows, Gate origin, 0-based anchor, widget-level "slice stays at
0.25–0.75 when shown alone"). 203/203 green. Note: one flaky SEGFAULT in
AnalyserPlotLayoutTest teardown reproduced once in a full ctest run —
the known offscreen QCustomPlot teardown issue already excluded on CI,
passes 5/5 isolated and on suite rerun.

## Follow-up (user requests): colour stability + plot toolbar regroup

- Channel colours are now reserved per table row (per axis), counted over
  ALL rows — toggling visibility no longer re-colours the other traces;
  hidden rows keep their reserved colour as a hollow swatch. Regression
  test ChannelColorsStableWhenTogglingVisibility.
- Plot toolbar: "⤢ Fit" and "↕→Y" (fit Y to current X window) now sit
  together; "Y−" added next to "Y+" (removes the last axis, same in-use
  guard); "PNG…" replaced by a painted camera icon (plot lib stays
  style-lib-free). The "Y axes + −" rows were removed from the Analyser
  panel and the Recorder live-preview sidebar.
- 204/204 tests green; dark-theme screenshots re-checked.

## Refinement (user): fit-button labels
Final form per user: "⤢ XY Fit" (diagonal arrow — fits X and all Y axes,
Home) and "↕ Y Fit" (vertical arrow — fits Y axes to the current X
window), adjacent at the left of the plot toolbar.

## Refinement 2 (user): fit buttons are arrows-only + "Fit"
Final final form: "⤢ Fit" (X + all Y axes) and "↕ Fit" (Y axes within the
current X window) — the arrow alone distinguishes them; tooltips explain.

## Refinement 3 (user, plan approved): boxed toolbar button groups
Plot toolbar groups related buttons in thin rounded boxes with captions:
[⤢ ↕] Fit   [↔+ ↔− ↕+ ↕−] Zoom   Δ Measure   Line ▾   [Y+ Y−]   📷.
New QSS role "btnGroup" in Theme.cpp; ScopePlot builds groups via a local
makeGroup/makeBtnInto helper (behaviour and tooltips unchanged).

## Refinement 4 (user, option chosen via question): spin-box steppers
All QSpinBox/QDoubleSpinBox are horizontal steppers now: [◀] value [▶] —
full-height side buttons, hover/pressed washes, crisp chevron SVGs
generated per theme into the app cache dir (QSS image: urls; the
border-triangle trick and base-style arrow primitives don't work once
the buttons are styled). Applies app-wide (Filter builder time constant,
Recorder port, Converter offsets, …).

## Release v0.2.0 (2026-06-12)
Pushed main (a836852: windows.yml gets the same flaky-offscreen-fixture
exclusion as linux.yml after intermittent 0xc0000005 crashes; project
VERSION → 0.2.0). Both workflows green; artifacts downloaded, Windows zip
repacked without .pdb/.ilk (39 MB vs 143 MB in v0.1.0, which had shipped
the debug symbols). gh release create v0.2.0 + uploaded
ScopeAnalyser-v0.2.0-{linux-x64.tar.gz, windows-x64.zip}.

## SVG/PDF screenshots + interactive HTML export (user-approved plan)

- Camera button → Save dialog with PNG / SVG / PDF (vector formats stay
  sharp at any zoom). Qt6Svg found optionally at configure time (CI's aqt
  Qt ships it; locally qt6-svg-dev). Testable core ScopePlot::saveImage().
- "Export HTML…" (Analyser action bar): one self-contained offline file —
  uPlot 1.6.32 vendored (MIT, converter/resources/uplot, embedded via
  qrc + Q_INIT_RESOURCE at global scope). Time + Frequency charts,
  wheel/box zoom, dbl-click reset, legend toggling. Union-grid merge with
  nulls for mixed rates, NaN → gap, shortest-round-trip doubles
  (std::to_chars). 5 unit tests; screenshot hook also writes
  demo_interactive.html.
- Answer to "interactive PDF": not feasible (layers/JS are Acrobat-only,
  Qt can't emit them); vector PDF gives crisp zoom, HTML gives the real
  interactivity.

## Root cause of the "offscreen widget flake": use-after-free (ASan)

The escalating intermittent SEGFAULTs in AnalyserPlotLayoutTest were NOT
runner flakiness: ASan (build/asan) caught a heap-use-after-free —
EmptyHintLabel's model-signal handler ran during view teardown after the
viewport was already freed (a view deletes its viewport before its model;
the model's destruction emits modelReset). Affected every app shutdown,
intermittent natively, deterministic under ASan. Fixed with QPointer
guards in StyleKit; 25/25 ASan iterations clean, 3× full native suite
green (210 tests, zero crashes). windows.yml fixture exclusion reverted
(it had masked this real bug); linux.yml exclusion kept (pre-existing,
different SIGBUS documented before this session).

## HTML export v2: mirrors the Analyser (user-approved plan)

The exported page now reproduces the on-screen view: channel panel on the
left (checkbox + swatch + live cursor value), toolbar with the same boxed
[⤢ ↕] Fit and [↔+ ↔− ↕+ ↕−] Zoom groups, Δ Measure (two clicks → Δx/Δy/
1/|Δx|, right-click clears), Line/Points/Line+points selector, and the
same multi-Y-axis arrangement (labels, left/right sides, axis colours,
per-channel assignment + visibility + the app's trace-colour derivation —
light palette, as the page is light). Mouse: wheel = X zoom at cursor,
Shift+wheel = Y, drag = box zoom, dbl-click = fit; manually zoomed Y axes
stop auto-fitting until Fit (the app's rule, incl. the 5% margin).
Plumbing: HtmlExportView structs; AnalyserPlot::htmlExportView() snapshots
the live widget; ScopePlot colour derivation exposed as theme-explicit
statics. +2 tests (spec content; widget-level view mirroring). 212/212
green; embedded JS syntax-checked with node. XY view has no HTML
counterpart (documented).
