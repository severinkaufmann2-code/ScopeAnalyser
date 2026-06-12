# 2026-06-12 08:43 — UI redesign: executed

Everything from the plan landed. 168/168 tests pass; all targets (app,
3 standalone exes, tests) build clean on linux-release.

## New
- `style/` → `scope_style` lib:
  - `Theme.h/cpp` — Fusion + light/dark QPalette + one token-driven QSS.
    `ThemeMode {System, Light, Dark}` persisted under `ui/themeMode`
    (QSettings); System follows `QStyleHints::colorSchemeChanged`.
    QSS hooks: `scopeRole` = `sectionLabel | dim | emptyHint | pill |
    toolbarStrip`, `pillTone` = `neutral|ok|busy|warn|error|rec`,
    `QPushButton[accent]` / `[danger]`.
  - `StyleKit.h/cpp` — QPainter-drawn icons (Glyph enum; no asset files),
    `colorSwatch`/`hollowSwatch`, `sectionLabel()`, `makePill()/setPill()`,
    `applyToolbarStrip()`, `installEmptyHint()` (overlay label tracking
    model row count; watches view *and* viewport for Resize/Show).

## Changed
- **app**: ShellWindow — menu bar (File / View→Appearance / Help with a
  plot-controls cheat sheet + About), tab icons, status bar with live
  "Signal store: N channels", window icon, geometry+tab persistence.
  main.cpp applies saved theme; env-guarded screenshot mode
  (`SCOPE_SHELL_SCREENSHOT_DIR`, seeds 3 demo signals, grabs each tab,
  quits) + `ShellWindow::seedDemoData()`.
- **plot**: ScopePlot fully palette-driven (`applyThemeColors()`:
  background, axes, grid, legend, crosshair, region rect, measure label);
  separate light/dark axis palettes; restyles on `QEvent::PaletteChange`
  and emits `themePaletteChanged()`. `deriveChannelColor`: on a neutral
  (gray) axis base, channels ≥1 now take distinct hues from the axis
  palette — fixes all-channels-look-identical on the default Y1.
  Toolbar buttons auto-raise inside a `toolbarStrip`.
- **analyser**: AnalyserPlot — top action bar (Open/Save chart,
  Save/Load layout, accent Redraw), sectioned left panel (VIEW /
  CHANNELS with +/✎/− icon buttons / Y AXES with +/−), real splitter
  (no 320px cap), per-row trace-colour swatches, empty hint,
  recolor on theme flip.
- **recorder**: RecorderWidget — two-row header strip: connection row
  (source, ADS fields, accent Connect, Disconnect, status pill) and
  capture row (red Record + Stop + dim stats label). Record gated on
  connected. Pill states: Disconnected / Connected — X / ● Recording.
  SymbolBrowser + ChannelTable got section labels, icons, alternating
  rows, empty hints. LivePreviewPlot sidebar sectioned + splitter +
  swatches + theme hookup.
- **converter**: toolbar strip with icons + right-aligned accent
  "Apply all", sectioned panes (Opened files / File preview / HDF5
  selector), empty hint on file list, accent Apply in H5 panel.
- Standalone mains apply the saved theme; module CMakeLists link
  `scope_style` (PRIVATE); root adds `style/`.

## Verified
- `cmake --build --preset linux-release` clean; `ctest` 168/168 (twice —
  once after layout work, once after the colour-derivation change).
- Offscreen screenshots of all 3 tabs × dark+light reviewed
  (`/tmp/scope-shots/...`, theme forced via `XDG_CONFIG_HOME` pointing at
  a prepared `ScopeAnalyser/ScopeAnalyser.conf` so user settings stay
  untouched). Two defects found and fixed this way: identical trace
  colours on Y1; converter file-list empty hint not appearing (resize
  events arrive at the viewport, not the view, when the page starts
  hidden).

## Not done (deliberate)
- AddChannelDialog / SaveChartDialog / MappingPanel internals: inherit
  the global theme only.
- No menu duplication of per-tab actions; no nav-paradigm change.
- Windows build not exercised locally (CI builds it — windows.yml).
