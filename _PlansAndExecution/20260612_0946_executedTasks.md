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
  **No production code changed** — fixes await user approval.
