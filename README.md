# ScopeAnalyser

Signal recording, analysis, and data conversion for TwinCAT ADS.

Three tools in one shell, also usable standalone:

1. **Recorder** — captures channels from a TwinCAT PLC over ADS, with per-channel
   auto-rate (parent-task cycle) or oversampled-array mode. Lossless capture into
   HDF5 sessions.
2. **Analyser** — typed formula language with autocomplete (`Filter`, `Integral`,
   `Derivative`, `Mean`, …) over recorded channels, vectorized via `exprtk`.
3. **Converter** — file-to-signal translator with drag-mapping UI and saveable
   `.scaconv` profiles. Excel and CSV in v1.

## Build

### Linux (Ubuntu 24.04+ / 25.10)

System packages:

```bash
sudo apt install -y \
    build-essential cmake ninja-build git pkg-config \
    qt6-base-dev qt6-tools-dev qt6-base-dev-tools \
    libqt6test6t64 libqt6printsupport6t64 \
    libhdf5-dev hdf5-helpers \
    libspdlog-dev nlohmann-json3-dev \
    libgtest-dev libgmock-dev
```

Configure + build:

```bash
cmake --preset linux-release
cmake --build --preset linux-release
ctest --preset linux-release   # optional
```

The small headers-only / source-bundle deps (HighFive, exprtk, QCustomPlot,
QXlsx, moodycamel ConcurrentQueue, Beckhoff/ADS) are fetched at configure time
via `FetchContent`, so no vcpkg is required on Linux.

### Windows

Use vcpkg to provide the system-package equivalents (Qt6, HDF5, spdlog,
nlohmann_json, gtest), and `cmake --preset windows-release`. The
`FetchContent` deps remain the same. `VCPKG_ROOT` must be set.

## Repository layout

```
core/        shared data model, SignalStore, HDF5 sessions, IAdsClient
ads/         Beckhoff/ADS open-source client implementation
recorder/    Recorder library + UI + standalone exe
analyser/    Analyser library + UI + standalone exe
converter/   Converter library + UI + standalone exe
app/         integrated shell (3 tabs)
tests/       GoogleTest suite
```

## Status

Phase 1 in progress. See `_PlansAndExecution/` for the active plan and
execution log.
