# ScopeAnalyser — Autonomous Session Executed (2026-06-03 21:19)

## Done

### Bug fix from earlier session
- `MockAdsClient` was stamping samples with `steady_clock` time, but
  `LivePreviewPlot` filtered the window using `system_clock`. Samples were
  always older than the window cutoff → no points drawn. Fixed by stamping
  with `nowNs()` to match the rest of the app and HDF5.
- Live preview X-axis now displays "seconds before now" (window
  `[-windowSeconds_, 0]`) instead of absolute epoch seconds. Y-range widens
  when min == max so BOOL toggles render correctly.

### Phase 3 — Analyser
- **FunctionRegistry** with 13 built-ins: `Filter`, `Integral`, `Derivative`,
  `Mean`, `RMS`, `Min`, `Max`, `Shift`, `Abs`, `Sqrt`, `Log`, `Sin`, `Cos`.
  Each carries signature, summary, help text, and an impl function pointer.
- **FormulaEngine**: hand-rolled tokenizer + recursive-descent parser
  (additive → multiplicative → unary → primary) producing derived signals
  via elementwise binary ops on Signal objects. Constants broadcast across
  signals. Identifiers with `.` (e.g. `Mock.sine_1hz`) are single tokens.
- **AnalyserWidget**: QPlainTextEdit subclass with QCompleter sourced from
  channel names + function names (Ctrl+Space). Function help panel rendered
  from the registry. Channel side-list (double-click to insert into editor).
- **AnalyserPlot**: separate widget with checkable channel list, multi-color
  graphs, legend, auto-range. Anchors X at first sample so the axis stays
  small.
- 8 new tests covering parser, every kind of function, errors. All pass.

### Phase 4 — Converter
- **CsvSource**: quote-aware parser, spreadsheet column labels
  (A, B, …, AA, AB, …), configurable delimiter + decimal separator,
  time-unit detection (`s`, `ms`, `µs`/`us`, `ns`).
- **MappingPanel**: per-column role combo (Ignore / X-time / Signal),
  signal name + unit, header-row spinner, decimal-separator field, profile
  Save/Load buttons.
- **ConverterWidget**: QTableView preview + MappingPanel split layout,
  Apply (import signals into the SignalStore), JSON profile round-trip.
- 2 new tests covering basic import + European decimals.

### Cross-tool wiring
- Recorder → Analyser flow validated by `MockPipeline.RecordThenAnalyseFlow`:
  record from Mock, then evaluate `Filter(Mock.sine_1hz, 0.05)` and verify
  the derived signal appears in the store with the right sample count.

### Polish
- AnalyserPlot auto-checks all channels on first render so newly opened
  data shows immediately.
- Mapping panel preserves existing profile values on `setProfile`.
- README rewritten to reflect the current feature set, with a quick tour.

## State at session end

- **Tests**: 18 / 18 passing.
- **Binaries**: `ScopeAnalyser`, `ScopeRecorder`, `ScopeAnalyserStandalone`,
  `ScopeConverter`, `scope_tests` all build and launch.
- **Commits this session**: `fa10295` (Phase 3), `3a3919c` (Phase 4),
  plus the QoL touches in this final commit.

## Open / deferred

- **OversampledChannel** real implementation — needs a real PLC with an
  oversampling terminal or fast-task `ARRAY` symbol. Cannot validate
  client-side.
- **Excel converter source** — QXlsx 1.4–1.5 require `Qt6::GuiPrivate`
  which isn't packaged on Ubuntu 25.10 without building Qt from source.
  Need to either (a) build Qt from source on Windows, (b) find a QXlsx
  fork that doesn't need GuiPrivate, or (c) use a different xlsx library
  (libxlsxio? openxlsx?).
- **Cross-rate math in formulas** — currently requires equal-length
  signals. A `Resample(signal, hz)` builtin or implicit auto-resample to
  the fastest common rate would let `MAIN.test + MAINFast.test` work.
- **Linux → Windows-VM ADS** — open-source Beckhoff/ADS lib's AMS
  handshake is rejected with error 18 by TwinCAT 3.1.4026.20. Workaround
  is the MockAdsClient or a native Windows ScopeAnalyser build.

## Questions for the user

See the end of the chat message — these are the only things I want to
confirm before continuing further autonomously.
