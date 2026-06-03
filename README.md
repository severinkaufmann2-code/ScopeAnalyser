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

Requires CMake >= 3.24 and vcpkg.

```bash
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset linux-release        # or windows-release
cmake --build --preset linux-release
```

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
