#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

class QAbstractItemView;
class QLabel;
class QWidget;

namespace scope::style {

// Crisp, DPI-aware icons painted with QPainter — no asset files. The default
// colour is a mid gray that stays readable on both themes; pass an explicit
// colour for accent/record uses.
enum class Glyph {
    AppLogo,      // accent rounded square + white waveform (window/taskbar)
    RecordTab,    // ring with centre dot
    AnalyseTab,   // waveform
    ConvertTab,   // opposing arrows
    RecordDot,    // filled circle (record button)
    Stop,         // filled square
    Plus,
    Minus,
    Pencil,
    FolderOpen,
    Save,
    Refresh,
    Undo,         // curved arrow pointing back (left)
    Redo,         // curved arrow pointing forward (right)
    Plug,         // connect
    Unplug,       // disconnect
    SendToAnalyser,    // record dot → waveform (Recorder "Send to Analyser")
    ConvertToAnalyser, // opposing-arrows → waveform (Converter "Apply …")
};

QIcon icon(Glyph glyph, const QColor& color = QColor());

// The application icon: the AppLogo glyph rendered at every size a title
// bar, taskbar, dock or .ico entry may ask for. icon(Glyph::AppLogo) stops
// at 48 px — fine for toolbars, too small to feed a 256 px app icon, and
// QIcon never upscales.
QIcon appIcon();

// A solid rounded colour square (channel ↔ trace colour swatches).
QIcon colorSwatch(const QColor& color);
// Outline-only variant for channels that are currently hidden.
QIcon hollowSwatch(const QColor& color);

// Small bold uppercase heading above a panel section.
QLabel* sectionLabel(const QString& text, QWidget* parent);

// Status chips. Tones map to the theme's semantic colours.
enum class PillTone { Neutral, Ok, Busy, Warn, Error, Rec };
QLabel* makePill(QWidget* parent);
void setPill(QLabel* pill, PillTone tone, const QString& text);

// Mark a container as a toolbar zone (strip background + bottom hairline).
void applyToolbarStrip(QWidget* w);

// Centred placeholder text shown over `view` whenever its model is empty.
void installEmptyHint(QAbstractItemView* view, const QString& text);

}  // namespace scope::style
