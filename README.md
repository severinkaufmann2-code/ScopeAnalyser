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
`⤢ Fit`, `↔ +` / `↔ −`, `↕ +` / `↕ −`, **`↕ → Y`** (fit each Y axis
to the data inside the current X window — X is left alone),
**`Δ Measure`** (checkable: click two points to get Δx / Δy /
1/|Δx|, right-click to clear), **`Display:` combo** (Line / Points
/ Line + points — *Points* puts a scatter dot at every sample so
you can see the sample rate directly), `Y+` (add a Y axis), `PNG…`
(save image). The Recorder's plot also has `⏸ Pause` / `▶ Resume`
to freeze the display while recording continues in the background.

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

The Analyser is the multi-Y plot plus its Channels sidebar. There is
no separate formula window — derived channels are added from the
sidebar via the **`+ Add channel…`** button.

In the Add-channel dialog:
- **Name**: the channel's name in the store.
- **Formula**: any expression — channel names, functions, arithmetic.
  Press **Tab** to autocomplete channel names and function
  signatures. The right pane lists every available function.

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

`Derivative(signal)` is plain `Δy/Δx` — central difference on the
interior, forward at the first sample, backward at the last. Same
shape as `numpy.gradient`. For a staircase-quantised signal
(a PLC value that stays flat for several samples then jumps), this
produces a comb pattern by design: fix the data shape at import
with the Converter's *Value plateaus* option (one sample per
value run). For noisy signals, compose with `Filter`:
`D = Derivative(Filter(s, tau))`.

**Different sample rates per channel work out of the box.** When two channels
of different rates appear in the same expression (e.g. `Hi + Lo` where `Hi`
is 1 kHz and `Lo` is 100 Hz), the engine linear-interpolates onto the
intersection of their time ranges using the higher-rate signal's grid. For
explicit control use `Resample(signal, 1000)` (to 1 kHz) or
`Resample(signal, OtherChannel)` (to another signal's timestamps).

**Saving the chart** — the **`Save chart…`** button below the channel
table writes every channel in the store to disk. The dialog asks for:
- *Format* — `.h5` (lossless) or `.csv`.
- *Time range* — *All data* or a *Custom* range in seconds (defaults
  to whatever you have visible on the X axis).
- *CSV options* — header on/off, column / row / decimal separators,
  and time mode (shared column vs per-signal pairs). Same controls as
  the Converter's *Save CSV…*.
Each signal is trimmed to the chosen range before being written.

`+ Add channel… / Edit channel… / − Remove channel` in the channels
sidebar add a derived channel via the formula dialog, edit an
existing formula channel (also via double-click on its row), or
remove the selected one from the store. Only formula-derived
channels are editable — recorded / imported channels have no
formula to change.

**Click a graph to highlight it** — clicking on a trace in the
chart selects the matching row in the Channels sidebar so you can
see at a glance which channel you just hit.

**Stable view on visibility toggles** — checking/unchecking a
channel in the sidebar table no longer refits the X axis. The
chart stays where you have it zoomed; only the data on the
graphs updates. Same goes for adding / removing channels and
switching a channel between Y axes. Use the toolbar `↕ → Y` to
re-centre the Y axes around what's in the current X window, or
`⤢ Fit` to fit everything.

### Converter

The Converter is a multi-file workspace. Every file you open (CSV or
.h5) stays in the workspace until you remove it, and each file keeps
its own per-file state — mappings for CSVs, channel selection for
.h5 — so you can switch back and forth and edit each one
independently.

**Layout**: a file list on the left, a preview in the middle, and the
right panel which switches between the CSV mapping panel and the H5
channel selector based on which file is active.

**Workflow:**

1. **Open CSV…** → adds the file to the list and makes it active. The
   preview appears in the middle.
2. In the mapping panel (right), set each column's role: `Ignore`,
   `X-axis (time)`, or `Signal`. Fill in the signal name and unit.
3. Set the header row index and decimal separator if needed.
4. **Each Y signal has its own X source.** In the Add channel dialog,
   either pick "from X-axis column [letter]" or "from sample rate
   [value][unit]". Different Y channels can use different sources,
   including different sample rates — every imported signal carries
   its own timeline.
5. Click **Apply (import signals)** → the channels flow into the
   SignalStore and appear in the Analyser. Re-applying replaces this
   file's previous import (so re-editing mappings doesn't pile up
   duplicates).
6. **Open .h5…** → adds the recording to the list and switches the
   right panel to a channel selector with checkboxes per signal,
   plus *Select all* / *Select none* and the per-channel sample
   count / duration. Tick what you want and click *Apply (import
   selected)*.
7. **Click a file in the list** to switch active. Its mappings (or
   selection) are restored exactly as you left them.
8. **Remove file** drops the file from the list *and* removes the
   signals it imported from the SignalStore (after a confirmation).

**Workspace save / load**. *Save workspace…* writes the file list +
each CSV's mapping profile + each .h5's checked-channel list into a
single `.scaws` JSON file. *Load workspace…* re-opens every file
from disk and restores the per-file state. Missing source files are
reported in the status bar but don't block loading the rest.

**Name collisions**. If two files produce a signal with the same
name, the second one is automatically uniquified to `name (2)`,
`name (3)`, etc. Removing one file doesn't affect signals owned by
others.

**Per-channel time controls**. Both panels expose two import-time
knobs per channel:

- **Relative** — subtract the channel's first timestamp on import,
  so it starts at `t = 0`. For CSV this only applies when X comes
  from a column (sample-rate mode is already relative-from-0).
- **Time offset [s]** — a constant shift added to every timestamp
  after the reset step. `Relative + offset = 5` puts the first
  sample at exactly `t = 5 s`.

Both knobs are per-channel by default. Above the channel table each
panel has a small *Apply to all* row — a Relative checkbox, an
Offset spinbox, and a button — that bulk-sets the same values onto
every channel in one click. Per-channel edits afterwards still
stick.

CSV values live inside the saved `.scaconv` profile. H5 values live
inside the workspace `.scaws`. Both round-trip cleanly.

**Duplicate-timestamp handling (CSV)**. If your CSV repeats the same
X value for several rows (oversampled logs, redundant exports,
wide-to-long reshapes), every row produces a sample by default —
no warning, no dedup. In the Add channel dialog the *Duplicate
timestamps* combo lets you collapse each run of equal X into one
sample: *first* keeps the first Y, *last* keeps the last, *mean*
averages the run. The choice is per-channel and persisted in
`.scaconv`. After each Apply the Converter scans the imported
signals and warns once if any channel still has >10 % duplicates,
so you can fix the mapping if it was unintended.

**Value-plateau handling (CSV)**. Distinct timestamps but the same
Y for several consecutive rows is the staircase pattern of a CSV
logged faster than the underlying value updates (integer encoder
read at 4 kHz, for example). In the same dialog the *Value
plateaus* combo collapses each run of equal Y into one sample:
*Collapse → first timestamp* uses the time the value first
appeared, *Collapse → last timestamp* uses the last confirmation
before it changed. After Apply the Converter warns if any channel
has >30 % consecutive-equal-value pairs and the mode is still
*Keep all* — this is the fix to apply before running `Derivative`
on the channel (otherwise central diff produces a comb pattern).

### Export and multi-source import (Converter)

The Converter top bar has four buttons:

- **Open CSV…** / **Open .h5…** — add a file to the workspace (see the
  Converter workflow above for the per-file editing).
- **Save workspace… / Load workspace…** — `.scaws` JSON of the full
  file list + per-file state.
- **Apply all (import signals)** — runs Apply on every file in the
  workspace using each one's saved mappings / channel selection.
  Use this after Load workspace… to push everything into the
  SignalStore (and the Analyser) in one click. Files that produce
  no signals are reported in a summary dialog and skipped — the
  others still import.
- **Save .h5…** — write every channel currently in the SignalStore to
  a recording file (same format the Recorder writes).
- **Save CSV…** — write every channel to CSV. A small Options dialog
  appears first; all fields have defaults so clicking OK twice
  (Options then Save) produces a sensible standard CSV (`,` columns,
  `\n` rows, `.` decimal, single time column resampled to the union
  of all signals' timestamps).

Per-file *Save profile…* / *Load profile…* (`.scaconv`) is still there
in the CSV mapping panel for when you want to apply the same mapping
to many similarly-shaped CSV files without saving the whole
workspace.

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
