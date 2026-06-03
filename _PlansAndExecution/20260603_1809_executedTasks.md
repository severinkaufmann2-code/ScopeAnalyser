# ScopeAnalyser — Executed Tasks (2026-06-03 18:09)

Session corresponds to plan `20260603_1809_Plans.md`.

## Done in this session

- Created project repo at `/home/admin/Desktop/Projects/ScopeAnalyser/`.
- Scaffolded folder layout: `core/`, `ads/`, `recorder/`, `analyser/`,
  `converter/`, `app/`, `tests/`, `cmake/`, `_ClaudeMemory/`,
  `_PlansAndExecution/`.
- Wrote root `CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json`,
  `.gitignore`, `README.md`.
- Wrote `cmake/scope_helpers.cmake` (`scope_add_library` helper).
- Wrote per-module `CMakeLists.txt` declaring sources, headers, public
  includes, and library dependencies.
- Saved plan and execution log to `_PlansAndExecution/`.
- Initialized local git repo, pushed to
  `https://github.com/severinkaufmann2-code/ScopeAnalyser` (public).
- Wrote Phase 1 source files:
  - `core/`: `Types.h`, `Signal.{h,cpp}`, `SignalStore.{h,cpp}`,
    `IAdsClient.h`, `Hdf5Session.{h,cpp}`.
  - `ads/`: `BeckhoffOpenAdsClient.{h,cpp}` wrapping the Beckhoff/ADS
    open-source library (symbol upload + notification subscriptions).
    `taskCycleForSymbol()` is a Phase-1b TODO pending real-PLC validation.
  - `recorder/`: `NotifyChannel.{h,cpp}` (lock-free queue → drain),
    `OversampledChannel.{h,cpp}` (Phase-2 stub),
    `RecordingSession.{h,cpp}` (Qt-driven 20 Hz drain timer + HDF5
    append + overrun surfacing), `RecorderWidget.{h,cpp}`,
    `SymbolBrowserWidget.{h,cpp}`, `ChannelTableWidget.{h,cpp}`,
    `LivePreviewPlot.{h,cpp}`, `standalone_main.cpp`.
  - `analyser/` + `converter/`: Phase-3/4 placeholder widgets, function
    registry shell, profile JSON round-trip, source plug-in skeletons.
  - `app/`: `main.cpp` + `ShellWindow.{h,cpp}` with three tabs sharing
    one `SignalStore`.
  - `tests/`: gtest cases for `SignalStore`, `Hdf5Session` round-trip,
    `FormulaEngine` stub, `ConverterProfile` round-trip.

## Pending (next session)

- Install build deps on this Linux box (cmake, ninja, vcpkg, Qt6 dev
  packages) and verify the project configures + builds end-to-end.
- Wire up `taskCycleForSymbol()` against a real TwinCAT System Service
  (port 10000) — needs an actual PLC to validate.
- Phase 2: `OversampledChannel` real implementation + plot polish
  (adaptive sampling, linked cursors).
- Phase 3: `FormulaEngine` real implementation with exprtk + autocomplete.
- Phase 4: Converter (Excel + CSV + drag-mapping UI).
