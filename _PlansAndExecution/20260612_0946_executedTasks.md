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
