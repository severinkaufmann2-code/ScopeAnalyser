---
name: ScopeAnalyser UI style system
description: Where the theme lives, the QSS/property contract, and how to verify UI changes visually
type: project
---

Since 2026-06-12 all look & feel lives in `style/` (`scope_style` lib):

- `scope::style::applySavedTheme()` is called by every main()
  (shell + standalones). Theme mode (System/Light/Dark) persists in
  QSettings `ui/themeMode`; View → Appearance switches it live.
- Widgets opt into styling via dynamic properties, not setStyleSheet:
  `scopeRole` (`sectionLabel`, `dim`, `emptyHint`, `pill`,
  `toolbarStrip`) and `pillTone`; buttons via `accent="true"` /
  `danger="true"`. Helpers in `scope/style/StyleKit.h`
  (`sectionLabel`, `makePill`/`setPill`, `applyToolbarStrip`,
  `installEmptyHint`, painted `icon(Glyph)` — no icon asset files).
- `plot/` deliberately does NOT link scope_style: ScopePlot derives all
  colours from QPalette, restyles on `QEvent::PaletteChange`, and emits
  `themePaletteChanged()` → hosts re-run their `recolorChannels()`.
  Channel colours come from `deriveChannelColor` (neutral Y1 base →
  distinct hues; coloured axes → shades of the axis hue).

**Why:** keeps theme switching live without widget-by-widget wiring, and
keeps the plot lib reusable without the style dependency.

**How to apply:** new UI should use the property hooks + StyleKit helpers
instead of hardcoded colours or per-widget stylesheets. To verify UI work
visually: build, then run the shell with
`QT_QPA_PLATFORM=offscreen SCOPE_SHELL_SCREENSHOT_DIR=<dir>` — it seeds
demo signals, saves a PNG per tab, and exits. Force a theme without
touching real settings by pointing `XDG_CONFIG_HOME` at a temp dir
containing `ScopeAnalyser/ScopeAnalyser.conf` with `[ui]\nthemeMode=1|2`.
