---
name: ScopeAnalyser stack and library decisions
description: Confirmed language, framework, libraries, build system, and OS targets
type: project
---

Confirmed in planning on 2026-06-03:

- **Language**: C++20.
- **UI**: Qt 6.7+ (Widgets).
- **Build**: CMake >= 3.24, Ninja, vcpkg manifest mode. Presets:
  `linux-{debug,release}`, `windows-{debug,release}`.
- **OS targets**: Linux AND Windows. Single ADS code path via the
  open-source `github.com/Beckhoff/ADS` (MIT). `IAdsClient` abstraction
  remains so `TcAdsDll` can drop in later if needed.
- **Plotting**: QCustomPlot 2.x.
- **Math parser**: exprtk (vectorized).
- **Storage**: HDF5 via HighFive, chunked + compressed.
- **Excel**: QXlsx (LGPL).
- **Threading**: moodycamel ConcurrentQueue for lock-free SPSC paths.
- **Tests**: GoogleTest.
- **Logging**: spdlog.
- **JSON**: nlohmann_json.

**Why:** Performance for 100 channels at sub-millisecond rates, native ADS
integration, and cross-platform desktop UI ruled out C# / Python / Rust.
LGPL deps were explicitly accepted.

**How to apply:** Don't propose replacements without a concrete reason
(measured bottleneck, license problem, missing capability). Keep the
`IAdsClient` abstraction even if only one impl exists.
