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

## Build verification on Linux (Ubuntu 25.10)

- Installed system deps via apt (cmake 3.31, ninja, Qt6 6.9, HDF5 1.14,
  spdlog, nlohmann_json, gtest).
- Configured + built the full project end-to-end. All five binaries link:
  `ScopeAnalyser`, `ScopeRecorder`, `ScopeAnalyserStandalone`,
  `ScopeConverter`, `scope_tests`.
- All four Qt apps launch cleanly under `QT_QPA_PLATFORM=offscreen`.
- All 7 gtest cases pass (SignalStore signals, HDF5 round-trip,
  FormulaEngine stub, ConverterProfile round-trip).

### Fixes applied during build verification

- Beckhoff/ADS uses `master` branch, not `main`.
- QXlsx 1.4–1.5 requires `Qt6::GuiPrivate` (not packaged on Ubuntu 25.10);
  dropped from build until Phase 4 (where we'll re-evaluate the lib).
- `std::unordered_map<QString, ...>` needs `#include <QHash>` for
  `std::hash<QString>`; added to SignalStore, Hdf5Session, LivePreviewPlot.
- BeckhoffOpenAdsClient rewritten for the real Beckhoff/ADS C++ API:
  `AdsDevice::GetHandle(...)` (returns RAII `AdsHandle`) instead of the
  imagined `AddNotification` / `DeleteNotification`.
- `AdsHandle`'s deleter has no default ctor → `Subscription` struct given
  a concrete constructor and constructed in-place.
- `QLineEdit` etc. forward declarations moved to global scope (had been
  inside `scope::recorder`).
- `signals` is a Qt macro for `public`; renamed the local variable in
  `test_hdf5_session.cpp` to `loaded`.
- HighFive `DataSpace{{0}, {UNLIMITED}}` resolved to the wrong constructor
  overload; switched to explicit `std::vector` args.

## Pending (next session)

- Wire up `taskCycleForSymbol()` against a real TwinCAT System Service
  (port 10000) — needs an actual PLC to validate.
- Phase 2: `OversampledChannel` real implementation + plot polish
  (adaptive sampling, linked cursors).
- Phase 3: `FormulaEngine` real implementation with exprtk + autocomplete.
- Phase 4: Converter (Excel + CSV + drag-mapping UI).
