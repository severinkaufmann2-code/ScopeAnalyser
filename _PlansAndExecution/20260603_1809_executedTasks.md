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
- Wrote per-module `CMakeLists.txt` stubs declaring sources, headers, public
  includes, and library dependencies.
- Saved plan and execution log to `_PlansAndExecution/`.

## Pending (next session)

- Write Phase 1 source files:
  - `core/include/scope/core/Types.h`, `Signal.h`, `SignalStore.h`,
    `IAdsClient.h`, `Hdf5Session.h`, plus their `.cpp` files.
  - `ads/include/scope/ads/BeckhoffOpenAdsClient.h` + impl.
  - `recorder/` headers + impls for `RecordingSession`, `NotifyChannel`,
    `OversampledChannel` (stub), `RecorderWidget`, `SymbolBrowserWidget`,
    `ChannelTableWidget`, `LivePreviewPlot`.
  - `app/main.cpp` + `ShellWindow` shell.
  - gtest cases for `SignalStore` and `Hdf5Session`.
- Verify build on Linux (install cmake, vcpkg, Qt6).
- Init local git repo; create GitHub repo on confirmation from user.
