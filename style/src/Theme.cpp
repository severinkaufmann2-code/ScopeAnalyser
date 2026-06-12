#include "scope/style/Theme.h"

#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QSettings>
#include <QString>
#include <QStyleFactory>
#include <QStyleHints>

namespace scope::style {

namespace {

// Every colour the stylesheet / palette needs, resolved per theme. The
// stylesheet below is written once against these tokens so light and dark
// can't drift apart structurally.
struct Tokens {
    QColor window;        // window / dialog background
    QColor base;          // input fields, item views, plot area
    QColor alt;           // alternating rows
    QColor strip;         // toolbar strips ([scopeRole="toolbarStrip"])
    QColor hover;         // generic hover wash
    QColor text;
    QColor text2;         // secondary text (section labels, headers, hints)
    QColor textDisabled;
    QColor border;
    QColor borderStrong;
    QColor grid;          // table gridlines
    QColor headerBg;
    QColor btn, btnHover, btnDown;
    QColor accent, accentHover, accentDown;
    QColor scroll, scrollHover;
    QColor ok, okStrong;
    QColor warn, warnStrong;
    QColor error, errorStrong;
    QColor record, recordStrong;   // recording = red family, kept distinct
                                   // from error so alarms still stand out
    QColor tipBg, tipText, tipBorder;
    QColor selBg;         // item-view selection (soft accent)
};

QColor withAlpha(QColor c, int a) { c.setAlpha(a); return c; }

Tokens darkTokens() {
    Tokens t;
    t.window       = QColor(0x20, 0x23, 0x27);
    t.base         = QColor(0x2a, 0x2e, 0x33);
    t.alt          = QColor(0x27, 0x2b, 0x30);
    t.strip        = QColor(0x24, 0x28, 0x2c);
    t.hover        = withAlpha(Qt::white, 18);
    t.text         = QColor(0xe7, 0xea, 0xee);
    t.text2        = QColor(0x9a, 0xa3, 0xad);
    t.textDisabled = QColor(0x5f, 0x67, 0x70);
    t.border       = QColor(0x3a, 0x40, 0x46);
    t.borderStrong = QColor(0x4d, 0x55, 0x5e);
    t.grid         = QColor(0x34, 0x3a, 0x40);
    t.headerBg     = QColor(0x24, 0x28, 0x2c);
    t.btn          = QColor(0x32, 0x37, 0x3d);
    t.btnHover     = QColor(0x3b, 0x41, 0x48);
    t.btnDown      = QColor(0x2c, 0x31, 0x36);
    t.accent       = QColor(0x4f, 0x9c, 0xf0);
    t.accentHover  = QColor(0x66, 0xab, 0xf5);
    t.accentDown   = QColor(0x3f, 0x8b, 0xe0);
    t.scroll       = withAlpha(t.text2, 90);
    t.scrollHover  = withAlpha(t.text2, 150);
    t.ok           = QColor(0x3f, 0xb9, 0x50);
    t.okStrong     = QColor(0x62, 0xd0, 0x72);
    t.warn         = QColor(0xd2, 0x99, 0x22);
    t.warnStrong   = QColor(0xe3, 0xb3, 0x41);
    t.error        = QColor(0xf8, 0x51, 0x49);
    t.errorStrong  = QColor(0xff, 0x7b, 0x72);
    t.record       = QColor(0xe5, 0x48, 0x4d);
    t.recordStrong = QColor(0xff, 0x70, 0x74);
    t.tipBg        = QColor(0x1a, 0x1d, 0x21);
    t.tipText      = QColor(0xe7, 0xea, 0xee);
    t.tipBorder    = QColor(0x3a, 0x40, 0x46);
    t.selBg        = withAlpha(t.accent, 70);
    return t;
}

Tokens lightTokens() {
    Tokens t;
    t.window       = QColor(0xf4, 0xf5, 0xf7);
    t.base         = QColor(0xff, 0xff, 0xff);
    t.alt          = QColor(0xf6, 0xf7, 0xf9);
    t.strip        = QColor(0xec, 0xee, 0xf1);
    t.hover        = withAlpha(Qt::black, 16);
    t.text         = QColor(0x20, 0x24, 0x2a);
    t.text2        = QColor(0x5d, 0x65, 0x70);
    t.textDisabled = QColor(0x9d, 0xa4, 0xac);
    t.border       = QColor(0xd4, 0xd8, 0xdd);
    t.borderStrong = QColor(0xb4, 0xba, 0xc2);
    t.grid         = QColor(0xe6, 0xe8, 0xeb);
    t.headerBg     = QColor(0xee, 0xf0, 0xf3);
    t.btn          = QColor(0xfb, 0xfc, 0xfd);
    t.btnHover     = QColor(0xf0, 0xf2, 0xf5);
    t.btnDown      = QColor(0xe5, 0xe8, 0xec);
    t.accent       = QColor(0x2e, 0x6f, 0xd6);
    t.accentHover  = QColor(0x3d, 0x7d, 0xe0);
    t.accentDown   = QColor(0x26, 0x5f, 0xba);
    t.scroll       = withAlpha(t.text2, 80);
    t.scrollHover  = withAlpha(t.text2, 140);
    t.ok           = QColor(0x1a, 0x7f, 0x37);
    t.okStrong     = QColor(0x11, 0x63, 0x29);
    t.warn         = QColor(0x9a, 0x67, 0x00);
    t.warnStrong   = QColor(0x7d, 0x53, 0x00);
    t.error        = QColor(0xcf, 0x22, 0x2e);
    t.errorStrong  = QColor(0xa4, 0x0e, 0x26);
    t.record       = QColor(0xd6, 0x33, 0x38);
    t.recordStrong = QColor(0xb3, 0x1d, 0x22);
    t.tipBg        = QColor(0x2b, 0x2f, 0x36);
    t.tipText      = QColor(0xf0, 0xf2, 0xf4);
    t.tipBorder    = QColor(0x2b, 0x2f, 0x36);
    t.selBg        = withAlpha(t.accent, 45);
    return t;
}

QString rgba(const QColor& c) {
    return QString("rgba(%1,%2,%3,%4)")
        .arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha());
}

// The single stylesheet, written against tokens. Custom widget hooks:
//   QLabel[scopeRole="sectionLabel"]  — small bold uppercase section heading
//   QLabel[scopeRole="dim"]           — secondary-colour text
//   QLabel[scopeRole="emptyHint"]     — placeholder text over empty views
//   QLabel[scopeRole="pill"][pillTone=...] — status chips (StyleKit::setPill)
//   QWidget[scopeRole="toolbarStrip"] — toolbar zones with bottom hairline
//   QPushButton[accent="true"]        — primary action
//   QPushButton[danger="true"]        — record / destructive action
QString buildSheet(const Tokens& t) {
    QString s = QString(R"qss(
/* ---------- menus ---------- */
QMenuBar {
    background: %WINDOW%;
    border-bottom: 1px solid %BORDER%;
    padding: 1px 6px;
}
QMenuBar::item { padding: 4px 10px; border-radius: 5px; background: transparent; }
QMenuBar::item:selected { background: %HOVER%; }
QMenu { background: %BASE%; border: 1px solid %BORDER%; border-radius: 6px; padding: 5px; }
QMenu::item { padding: 5px 28px 5px 12px; border-radius: 4px; }
QMenu::item:selected { background: %SELBG%; }
QMenu::item:disabled { color: %TEXT_DISABLED%; background: transparent; }
QMenu::separator { height: 1px; background: %BORDER%; margin: 5px 8px; }

/* ---------- tabs ---------- */
QTabWidget::pane { border: none; border-top: 1px solid %BORDER%; }
QTabBar { background: transparent; }
QTabBar::tab {
    background: transparent;
    color: %TEXT2%;
    padding: 9px 18px;
    margin: 2px 2px 0 2px;
    border: none;
    border-bottom: 2px solid transparent;
    font-weight: 600;
}
QTabBar::tab:hover { color: %TEXT%; }
QTabBar::tab:selected { color: %ACCENT%; border-bottom: 2px solid %ACCENT%; }

/* ---------- buttons ---------- */
QPushButton {
    background: %BTN%;
    border: 1px solid %BORDER%;
    border-radius: 6px;
    padding: 5px 14px;
    min-height: 17px;
}
QPushButton:hover { background: %BTN_HOVER%; border-color: %BORDER_STRONG%; }
QPushButton:pressed { background: %BTN_DOWN%; }
QPushButton:disabled { color: %TEXT_DISABLED%; background: transparent; }
QPushButton[accent="true"] {
    background: %ACCENT%; border-color: %ACCENT%; color: white; font-weight: 600;
}
QPushButton[accent="true"]:hover { background: %ACCENT_HOVER%; border-color: %ACCENT_HOVER%; }
QPushButton[accent="true"]:pressed { background: %ACCENT_DOWN%; border-color: %ACCENT_DOWN%; }
QPushButton[accent="true"]:disabled { background: %BTN%; border-color: %BORDER%; color: %TEXT_DISABLED%; }
QPushButton[danger="true"] {
    background: %RECORD%; border-color: %RECORD%; color: white; font-weight: 600;
}
QPushButton[danger="true"]:hover { background: %RECORD_STRONG%; border-color: %RECORD_STRONG%; }
QPushButton[danger="true"]:disabled { background: %BTN%; border-color: %BORDER%; color: %TEXT_DISABLED%; }

QToolButton {
    background: transparent;
    border: 1px solid transparent;
    border-radius: 5px;
    padding: 4px 9px;
}
QToolButton:hover { background: %HOVER%; }
QToolButton:pressed { background: %BTN_DOWN%; }
QToolButton:checked { background: %SELBG%; color: %ACCENT_TEXT%; border-color: %ACCENT_SOFT%; }
QToolButton:disabled { color: %TEXT_DISABLED%; }

/* ---------- inputs ---------- */
QLineEdit, QPlainTextEdit, QTextEdit, QSpinBox, QDoubleSpinBox, QComboBox {
    background: %BASE%;
    border: 1px solid %BORDER%;
    border-radius: 5px;
    padding: 3px 8px;
    selection-background-color: %ACCENT%;
    selection-color: white;
}
QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus,
QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus { border-color: %ACCENT%; }
QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled, QComboBox:disabled {
    color: %TEXT_DISABLED%; background: transparent;
}
QComboBox::drop-down { border: none; width: 20px; }
QComboBox QAbstractItemView {
    background: %BASE%;
    border: 1px solid %BORDER%;
    border-radius: 6px;
    padding: 3px;
    selection-background-color: %SELBG%;
    selection-color: %TEXT%;
}

/* ---------- item views ---------- */
QTableView, QTreeView, QListView {
    background: %BASE%;
    alternate-background-color: %ALT%;
    border: 1px solid %BORDER%;
    border-radius: 4px;
    gridline-color: %GRID%;
    selection-background-color: %SELBG%;
    selection-color: %TEXT%;
    outline: none;
}
QTableView::item:selected, QTreeView::item:selected, QListView::item:selected {
    background: %SELBG%; color: %TEXT%;
}
QListView::item { padding: 3px 4px; }
QTreeView::item { padding: 2px 2px; }
QHeaderView { background: transparent; }
QHeaderView::section {
    background: %HEADER_BG%;
    color: %TEXT2%;
    border: none;
    border-bottom: 1px solid %BORDER%;
    border-right: 1px solid %GRID%;
    padding: 4px 8px;
    font-weight: 600;
}
QTableCornerButton::section {
    background: %HEADER_BG%; border: none; border-bottom: 1px solid %BORDER%;
}

/* ---------- scrollbars ---------- */
QScrollBar:vertical { background: transparent; width: 12px; margin: 2px; }
QScrollBar::handle:vertical { background: %SCROLL%; border-radius: 4px; min-height: 30px; }
QScrollBar::handle:vertical:hover { background: %SCROLL_HOVER%; }
QScrollBar:horizontal { background: transparent; height: 12px; margin: 2px; }
QScrollBar::handle:horizontal { background: %SCROLL%; border-radius: 4px; min-width: 30px; }
QScrollBar::handle:horizontal:hover { background: %SCROLL_HOVER%; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

/* ---------- containers ---------- */
QSplitter::handle { background: transparent; }
QSplitter::handle:hover { background: %SELBG%; }
QSplitter::handle:horizontal { width: 5px; }
QSplitter::handle:vertical { height: 5px; }

QGroupBox {
    border: 1px solid %BORDER%;
    border-radius: 8px;
    margin-top: 12px;
    padding-top: 6px;
    font-weight: 600;
}
QGroupBox::title {
    subcontrol-origin: margin;
    left: 10px;
    padding: 0 4px;
    color: %TEXT2%;
}

QStatusBar { background: %WINDOW%; border-top: 1px solid %BORDER%; color: %TEXT2%; }
QStatusBar::item { border: none; }

QToolTip {
    background: %TIP_BG%;
    color: %TIP_TEXT%;
    border: 1px solid %TIP_BORDER%;
    border-radius: 4px;
    padding: 5px 8px;
}

QCheckBox { spacing: 6px; }

/* ---------- scope roles ---------- */
QWidget[scopeRole="toolbarStrip"] {
    background: %STRIP%;
    border-bottom: 1px solid %BORDER%;
}
QLabel[scopeRole="sectionLabel"] {
    color: %TEXT2%;
    font-size: 11px;
    font-weight: 700;
    letter-spacing: 1px;
    margin-top: 2px;
}
QLabel[scopeRole="dim"] { color: %TEXT2%; }
QLabel[scopeRole="emptyHint"] { color: %TEXT_DISABLED%; font-size: 13px; background: transparent; }
QLabel[scopeRole="pill"] {
    border-radius: 10px;
    padding: 2px 12px;
    font-weight: 600;
    font-size: 12px;
    background: %HOVER%;
    color: %TEXT2%;
    border: 1px solid %BORDER%;
}
QLabel[scopeRole="pill"][pillTone="ok"]   { background: %OK_SOFT%;   color: %OK_STRONG%;   border-color: %OK_BORDER%; }
QLabel[scopeRole="pill"][pillTone="busy"] { background: %SELBG%;     color: %ACCENT_TEXT%; border-color: %ACCENT_SOFT%; }
QLabel[scopeRole="pill"][pillTone="warn"] { background: %WARN_SOFT%; color: %WARN_STRONG%; border-color: %WARN_BORDER%; }
QLabel[scopeRole="pill"][pillTone="error"]{ background: %ERR_SOFT%;  color: %ERR_STRONG%;  border-color: %ERR_BORDER%; }
QLabel[scopeRole="pill"][pillTone="rec"]  { background: %REC_SOFT%;  color: %REC_STRONG%;  border-color: %REC_BORDER%; }
)qss");

    // "Accent text" must stay readable on the soft selection wash: the
    // hover/checked backgrounds are translucent accent, so the saturated
    // accent itself reads fine on both themes.
    s.replace("%WINDOW%",        rgba(t.window));
    s.replace("%BASE%",          rgba(t.base));
    s.replace("%ALT%",           rgba(t.alt));
    s.replace("%STRIP%",         rgba(t.strip));
    s.replace("%HOVER%",         rgba(t.hover));
    s.replace("%TEXT%",          rgba(t.text));
    s.replace("%TEXT2%",         rgba(t.text2));
    s.replace("%TEXT_DISABLED%", rgba(t.textDisabled));
    s.replace("%BORDER%",        rgba(t.border));
    s.replace("%BORDER_STRONG%", rgba(t.borderStrong));
    s.replace("%GRID%",          rgba(t.grid));
    s.replace("%HEADER_BG%",     rgba(t.headerBg));
    s.replace("%BTN%",           rgba(t.btn));
    s.replace("%BTN_HOVER%",     rgba(t.btnHover));
    s.replace("%BTN_DOWN%",      rgba(t.btnDown));
    s.replace("%ACCENT%",        rgba(t.accent));
    s.replace("%ACCENT_HOVER%",  rgba(t.accentHover));
    s.replace("%ACCENT_DOWN%",   rgba(t.accentDown));
    s.replace("%ACCENT_SOFT%",   rgba(withAlpha(t.accent, 110)));
    s.replace("%ACCENT_TEXT%",   rgba(isDarkTheme() ? t.accentHover : t.accentDown));
    s.replace("%SELBG%",         rgba(t.selBg));
    s.replace("%SCROLL%",        rgba(t.scroll));
    s.replace("%SCROLL_HOVER%",  rgba(t.scrollHover));
    s.replace("%RECORD%",        rgba(t.record));
    s.replace("%RECORD_STRONG%", rgba(t.recordStrong));
    s.replace("%OK_SOFT%",       rgba(withAlpha(t.ok, 38)));
    s.replace("%OK_STRONG%",     rgba(t.okStrong));
    s.replace("%OK_BORDER%",     rgba(withAlpha(t.ok, 90)));
    s.replace("%WARN_SOFT%",     rgba(withAlpha(t.warn, 38)));
    s.replace("%WARN_STRONG%",   rgba(t.warnStrong));
    s.replace("%WARN_BORDER%",   rgba(withAlpha(t.warn, 90)));
    s.replace("%ERR_SOFT%",      rgba(withAlpha(t.error, 38)));
    s.replace("%ERR_STRONG%",    rgba(t.errorStrong));
    s.replace("%ERR_BORDER%",    rgba(withAlpha(t.error, 90)));
    s.replace("%REC_SOFT%",      rgba(withAlpha(t.record, 40)));
    s.replace("%REC_STRONG%",    rgba(t.recordStrong));
    s.replace("%REC_BORDER%",    rgba(withAlpha(t.record, 100)));
    s.replace("%TIP_BG%",        rgba(t.tipBg));
    s.replace("%TIP_TEXT%",      rgba(t.tipText));
    s.replace("%TIP_BORDER%",    rgba(t.tipBorder));
    return s;
}

QPalette buildPalette(const Tokens& t) {
    QPalette p;
    p.setColor(QPalette::Window,          t.window);
    p.setColor(QPalette::WindowText,      t.text);
    p.setColor(QPalette::Base,            t.base);
    p.setColor(QPalette::AlternateBase,   t.alt);
    p.setColor(QPalette::Text,            t.text);
    p.setColor(QPalette::Button,          t.btn);
    p.setColor(QPalette::ButtonText,      t.text);
    p.setColor(QPalette::BrightText,      t.errorStrong);
    p.setColor(QPalette::Highlight,       t.accent);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Link,            t.accent);
    p.setColor(QPalette::ToolTipBase,     t.tipBg);
    p.setColor(QPalette::ToolTipText,     t.tipText);
    p.setColor(QPalette::PlaceholderText, t.textDisabled);
    p.setColor(QPalette::Light,           t.btnHover);
    p.setColor(QPalette::Midlight,        t.border);
    p.setColor(QPalette::Mid,             t.borderStrong);
    p.setColor(QPalette::Dark,            t.window.darker(115));
    p.setColor(QPalette::Shadow,          QColor(0, 0, 0));

    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) {
        p.setColor(QPalette::Disabled, role, t.textDisabled);
    }
    p.setColor(QPalette::Disabled, QPalette::Base,      t.window);
    p.setColor(QPalette::Disabled, QPalette::Highlight, t.borderStrong);
    return p;
}

constexpr const char* kSettingsKey = "ui/themeMode";
bool g_dark = false;

bool resolveDark(ThemeMode mode) {
    if (mode == ThemeMode::Dark)  return true;
    if (mode == ThemeMode::Light) return false;
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}

void applyTheme(ThemeMode mode) {
    static bool fusionSet = false;
    if (!fusionSet) {
        QApplication::setStyle(QStyleFactory::create("Fusion"));
        fusionSet = true;
    }
    g_dark = resolveDark(mode);
    const Tokens t = g_dark ? darkTokens() : lightTokens();
    qApp->setPalette(buildPalette(t));
    qApp->setStyleSheet(buildSheet(t));
}

}  // namespace

ThemeMode themeMode() {
    switch (QSettings().value(kSettingsKey, 0).toInt()) {
        case 1:  return ThemeMode::Light;
        case 2:  return ThemeMode::Dark;
        default: return ThemeMode::System;
    }
}

void setThemeMode(ThemeMode mode) {
    QSettings().setValue(kSettingsKey, static_cast<int>(mode));
    applyTheme(mode);
}

bool isDarkTheme() { return g_dark; }

void applySavedTheme() {
    applyTheme(themeMode());
    // Follow live OS scheme flips while in System mode.
    static bool hooked = false;
    if (!hooked) {
        hooked = true;
        QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
                         qApp, [](Qt::ColorScheme) {
                             if (themeMode() == ThemeMode::System)
                                 applyTheme(ThemeMode::System);
                         });
    }
}

}  // namespace scope::style
