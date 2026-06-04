# ScopeAnalyser

Signal recording, analysis, and data conversion for TwinCAT ADS.

Three tools in one desktop application, also usable standalone:

1. **Recorder** — captures channels from a source (TwinCAT ADS, or a built-in
   Demo source for development) into lossless HDF5 sessions.
2. **Analyser** — formula language with autocomplete (`Filter`, `Integral`,
   `Derivative`, `Mean`, …) over recorded channels, plus a multi-channel plot
   with toggleable visibility.
3. **Converter** — file-to-signal translator with column-role mapping and
   saveable `.scaconv` profiles. CSV in v1; Excel planned.

The three tools share one in-memory `SignalStore`, so recordings and imports
become available everywhere instantly.

## Build

### Linux (Ubuntu 24.04+ / 25.10)

System packages:

```bash
sudo apt install -y \
    build-essential cmake ninja-build git pkg-config \
    qt6-base-dev qt6-tools-dev qt6-base-dev-tools \
    libqt6test6 libqt6printsupport6 \
    libhdf5-dev hdf5-helpers \
    libspdlog-dev nlohmann-json3-dev \
    libgtest-dev libgmock-dev
```

Configure + build:

```bash
cmake --preset linux-release
cmake --build --preset linux-release
./build/linux-release/bin/scope_tests       # 18 tests
```

The small headers-only / source-bundle deps (HighFive, exprtk, QCustomPlot,
moodycamel ConcurrentQueue, Beckhoff/ADS) are fetched at configure time via
`FetchContent`. No vcpkg required on Linux.

### Windows

Use vcpkg for the system packages and `cmake --preset windows-release`.
`VCPKG_ROOT` must be set.

## Running

Integrated shell (all three tools as tabs):

```bash
./build/linux-release/bin/ScopeAnalyser
```

Standalone tools:

```bash
./build/linux-release/bin/ScopeRecorder
./build/linux-release/bin/ScopeAnalyserStandalone
./build/linux-release/bin/ScopeConverter
```

## Quick tour

### Scope plot controls (Recorder + Analyser)

Both plots share the same `ScopePlot` widget with these conventions
(matches TwinCAT Scope / LabVIEW / Audacity):

| Action | What it does |
|---|---|
| Scroll (over plot area) | Zoom both axes, centred on mouse |
| **Scroll (over a Y-axis label area)** | **Zoom that Y axis only** |
| **Scroll (over the X-axis label area)** | **Zoom X axis only** |
| **Ctrl+Scroll** | Zoom X axis only (anywhere) |
| **Shift+Scroll** or **Alt+Scroll** over a Y-axis area | Zoom that Y axis only |
| **Shift+Scroll** or **Alt+Scroll** over the plot interior | Zoom **all** Y axes together |
| Left-drag | Pan |
| **Ctrl+Left-drag** | Zoom to drawn rectangle |
| `Home` | Fit all data |
| `+` / `-` | Zoom in / out |
| Arrow keys | Pan |
| Mouse hover | Crosshair + X / Y readout per channel |

Note: some Linux window managers (i3, KWin, etc.) intercept
Shift+Wheel for window operations. If your `Shift+Scroll` does nothing,
either use **`Alt+Scroll`** or simply **hover over the axis label area
and scroll** — both produce the same axis-specific zoom.

A toolbar above each plot exposes the same actions as buttons:
`⤢ Fit`, `↔ +` / `↔ −`, `↕ +` / `↕ −`, `Y+` (add a Y axis), `PNG…`
(save image). The Recorder's plot also has `⏸ Pause` / `▶ Resume` to
freeze the display while recording continues in the background.

**Multiple Y axes** — both plots support an arbitrary number of Y
axes, alternating between the left and right side. To use them:

1. Click **`Y+`** in the toolbar to add a new axis. It appears with
   default label `Y2`, `Y3`, …
2. In the channels sidebar to the left, each channel has an **Axis**
   dropdown. Pick the axis you want this channel scaled against.
3. **Shift+Scroll** zooms the axis nearest the mouse cursor, so you
   can scale each axis independently.
4. **Right-click an axis label** for a context menu: rename, set
   range manually, auto-scale this axis only, or remove it (only if
   empty).
5. When a new channel arrives whose unit text matches an existing
   axis's label, it auto-assigns to that axis.
6. **Channel colours match their axis colour.** The first trace on
   each axis uses the axis's exact colour; additional traces on the
   same axis share the hue and vary in saturation/brightness so
   they're related but visually distinct.
7. **`Save layout…` / `Load layout…`** in the channels sidebar writes
   a `.scolayout` JSON file with the axes (labels, sides, ranges) and
   the channel→axis assignments. Loading a layout restores everything
   and remembers channel names that aren't currently in the store —
   when those channels later appear, they're placed on the saved axis
   automatically.

### Recorder

1. Pick a **Source**: `Demo (Mock)` to develop without a PLC, or
   `ADS over TCP` to talk to a real TwinCAT runtime.
2. Click **Connect**. With Demo, six synthetic channels appear in the symbol
   browser (`Mock.sine_1hz`, `Mock.cosine_10hz`, `Mock.sawtooth`,
   `Mock.counter`, `Mock.toggle`, `Mock.noisy_sine`).
3. Select channels, click **Add selected** → channel table fills in with
   auto-detected sample rates.
4. **Record** → choose `.h5` save path → samples stream into the live preview
   (lower pane, scope-style "seconds before now" axis) and into HDF5 on disk.
5. **Stop** when done.

### Analyser

1. The channel list (left) shows every signal currently in the store —
   recorded channels and derived ones.
2. Type a formula in the editor. Press **Ctrl+Space** for autocomplete on
   channel names and function names.
3. Click **Evaluate**.

Examples:

```
Smoothed = Filter(Mock.sine_1hz, 0.05)
Diff     = Derivative(Mock.sawtooth)
Sum      = Mock.sine_1hz + 2 * Mock.cosine_10hz - 1
Energy   = RMS(Mock.sine_1hz, 0.5)
```

Functions: `Filter`, `Integral`, `Derivative`, `Mean`, `RMS`, `Min`, `Max`,
`Shift`, `Abs`, `Sqrt`, `Log`, `Sin`, `Cos`, `Resample`. See the side panel
for the full reference.

**Different sample rates per channel work out of the box.** When two channels
of different rates appear in the same expression (e.g. `Hi + Lo` where `Hi`
is 1 kHz and `Lo` is 100 Hz), the engine linear-interpolates onto the
intersection of their time ranges using the higher-rate signal's grid. For
explicit control use `Resample(signal, 1000)` (to 1 kHz) or
`Resample(signal, OtherChannel)` (to another signal's timestamps).

The plot at the bottom toggles channels via checkboxes.

### Converter

1. **Open CSV** → preview appears with letter column headers.
2. In the mapping panel, set each column's role: `Ignore`, `X-axis (time)`,
   or `Signal`. Fill in the signal name and unit.
3. Set the header row index and decimal separator if needed.
4. **Each Y signal has its own X source.** In the Add channel dialog,
   either pick "from X-axis column [letter]" or "from sample rate
   [value][unit]". Different Y channels can use different sources, including
   different sample rates — every imported signal carries its own timeline.
5. **Apply (import signals)** → channels flow into the SignalStore and appear
   in the Analyser.
5. **Save profile…** → next time the same kind of file comes in, **Load
   profile…** → **Apply** is two clicks.

## Repository layout

```
core/        shared data model, SignalStore, HDF5 sessions, IAdsClient
plot/        ScopePlot widget (toolbar, modifier zoom, crosshair, pause)
ads/         BeckhoffOpenAdsClient (ADS over TCP) + MockAdsClient (synthetic)
recorder/    Recorder library + UI + standalone exe
analyser/    FormulaEngine + FunctionRegistry + UI + standalone exe
converter/   CsvSource + ConverterProfile + UI + standalone exe
app/         integrated shell with three tabs
tests/       33 GoogleTest cases
```

## Status

- **Phase 1** (Recorder core, HDF5, MockAdsClient) — **done**, 8 tests
- **Phase 2** (OversampledChannel, plot adaptive sampling) — partially done
  (live plot present; oversampled mode needs a real PLC to validate)
- **Phase 3** (Analyser engine + UI) — **done**, formula language with 13
  functions, autocomplete, multi-pane plot
- **Phase 4** (CSV converter + profiles) — **done**, Excel deferred until
  QXlsx / GuiPrivate situation is resolved on Ubuntu 25.10

See `_PlansAndExecution/` for active plans and execution logs, and
`_ClaudeMemory/` for project rules / decisions.
