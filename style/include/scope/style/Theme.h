#pragma once

namespace scope::style {

// Application-wide look & feel. One call at startup themes every window
// (Fusion base style + palette + stylesheet); switching at runtime re-themes
// live widgets. Plot internals follow along via QEvent::PaletteChange.
enum class ThemeMode { System = 0, Light = 1, Dark = 2 };

// Apply the persisted theme. Call once right after QApplication is
// constructed (needs organization/application names set for QSettings).
// While the mode is System, OS scheme changes re-apply automatically.
void applySavedTheme();

// The persisted user choice (not the resolved light/dark).
ThemeMode themeMode();

// Persist + apply immediately.
void setThemeMode(ThemeMode mode);

// Effective darkness after resolving System against the OS scheme.
bool isDarkTheme();

}  // namespace scope::style
