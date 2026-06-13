#include "ShellWindow.h"

#include "scope/recorder/RecorderWidget.h"
#include "scope/analyser/AnalyserWidget.h"
#include "scope/converter/ConverterWidget.h"
#include "scope/style/StyleKit.h"
#include "scope/style/Theme.h"

#include <QActionGroup>
#include <QCloseEvent>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QStatusBar>
#include <QTabWidget>

#include <cmath>
#include <memory>
#include <vector>

namespace scope::app {

ShellWindow::ShellWindow(QWidget* parent) : QMainWindow(parent) {
    tabs_ = new QTabWidget(this);
    tabs_->setDocumentMode(true);
    tabs_->setIconSize({18, 18});
    tabs_->addTab(new scope::recorder::RecorderWidget(store_, this),
                  style::icon(style::Glyph::RecordTab), "Recorder");
    tabs_->addTab(new scope::analyser::AnalyserWidget(store_, this),
                  style::icon(style::Glyph::AnalyseTab), "Analyser");
    tabs_->addTab(new scope::converter::ConverterWidget(store_, this),
                  style::icon(style::Glyph::ConvertTab), "Converter");
    setCentralWidget(tabs_);
    setWindowTitle("ScopeAnalyser");
    setWindowIcon(style::icon(style::Glyph::AppLogo));

    buildMenus();

    storeSummary_ = new QLabel(this);
    storeSummary_->setProperty("scopeRole", "dim");
    statusBar()->addPermanentWidget(storeSummary_);
    statusBar()->showMessage("Ready");
    updateStoreSummary();
    connect(&store_, &scope::core::SignalStore::channelAdded,
            this, [this](const QString&) { updateStoreSummary(); });
    connect(&store_, &scope::core::SignalStore::channelRemoved,
            this, [this](const QString&) { updateStoreSummary(); });

    QSettings settings;
    restoreGeometry(settings.value("shell/geometry").toByteArray());
    tabs_->setCurrentIndex(settings.value("shell/tab", 0).toInt());
}

void ShellWindow::closeEvent(QCloseEvent* ev) {
    QSettings settings;
    settings.setValue("shell/geometry", saveGeometry());
    settings.setValue("shell/tab", tabs_->currentIndex());
    QMainWindow::closeEvent(ev);
}

void ShellWindow::updateStoreSummary() {
    const auto n = store_.channelNames().size();
    storeSummary_->setText(n == 1 ? QString("Signal store: 1 channel")
                                  : QString("Signal store: %1 channels").arg(n));
}

void ShellWindow::buildMenus() {
    auto* fileMenu = menuBar()->addMenu("&File");
    auto* quit = fileMenu->addAction("E&xit");
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QWidget::close);

    auto* viewMenu = menuBar()->addMenu("&View");
    auto* themeMenu = viewMenu->addMenu("&Appearance");
    auto* group = new QActionGroup(this);
    auto addThemeAction = [&](const QString& text, style::ThemeMode mode) {
        auto* a = themeMenu->addAction(text);
        a->setCheckable(true);
        a->setChecked(style::themeMode() == mode);
        group->addAction(a);
        connect(a, &QAction::triggered, this,
                [mode] { style::setThemeMode(mode); });
    };
    addThemeAction("&System", style::ThemeMode::System);
    addThemeAction("&Light",  style::ThemeMode::Light);
    addThemeAction("&Dark",   style::ThemeMode::Dark);

    auto* helpMenu = menuBar()->addMenu("&Help");
    auto* controls = helpMenu->addAction("&Plot mouse && keyboard controls…");
    connect(controls, &QAction::triggered, this, [this] {
        QMessageBox box(this);
        box.setWindowTitle("Plot controls");
        box.setTextFormat(Qt::RichText);
        box.setText(
            "<h3>Mouse</h3>"
            "<table cellspacing='0' cellpadding='3'>"
            "<tr><td><b>Scroll</b></td><td>zoom X + Y at the cursor</td></tr>"
            "<tr><td><b>Scroll over an axis</b></td><td>zoom only that axis</td></tr>"
            "<tr><td><b>Ctrl + Scroll</b></td><td>zoom X only</td></tr>"
            "<tr><td><b>Shift + Scroll</b></td><td>zoom all Y axes (or the hovered one)</td></tr>"
            "<tr><td><b>Left-drag</b></td><td>pan</td></tr>"
            "<tr><td><b>Ctrl + Left-drag</b></td><td>zoom into a rectangle</td></tr>"
            "<tr><td><b>Right-click a Y axis</b></td><td>rename / set range / auto-scale / remove</td></tr>"
            "<tr><td><b>Click a trace</b></td><td>select its row in the channel list</td></tr>"
            "</table>"
            "<h3>Keyboard</h3>"
            "<table cellspacing='0' cellpadding='3'>"
            "<tr><td><b>Home</b></td><td>fit all data</td></tr>"
            "<tr><td><b>+ / −</b></td><td>zoom in / out</td></tr>"
            "<tr><td><b>Arrow keys</b></td><td>pan</td></tr>"
            "</table>"
            "<h3>Measure</h3>"
            "<p>Toggle <b>Δ Measure</b> in the plot toolbar, click two points "
            "to read Δx, Δy and 1/|Δx|; right-click clears.</p>");
        box.exec();
    });
    auto* about = helpMenu->addAction("&About ScopeAnalyser");
    connect(about, &QAction::triggered, this, [this] {
        QMessageBox::about(this, "About ScopeAnalyser",
            "<b>ScopeAnalyser</b><br>"
            "Signal recording (TwinCAT ADS), analysis, and data conversion.<br><br>"
            "The Analyser and Converter share one in-memory signal store, so "
            "imported or derived channels are available in both. The Recorder "
            "keeps its captures separate until you press “Send to Analyser”.");
    });
}

void ShellWindow::seedDemoData() {
    using scope::core::Signal;
    const auto t0 = scope::core::nowNs();
    const std::size_t n = 2000;
    auto mk = [&](const QString& name, const QString& unit, auto fn) {
        Signal::Meta meta;
        meta.name = name;
        meta.unit = unit;
        meta.dataType = scope::core::DataType::Float64;
        meta.sampleRateHz = 1000.0;
        auto s = std::make_shared<Signal>(meta);
        std::vector<scope::core::TimestampNs> ts(n);
        std::vector<double> vs(n);
        for (std::size_t i = 0; i < n; ++i) {
            ts[i] = t0 + static_cast<scope::core::TimestampNs>(i) * 1'000'000;
            vs[i] = fn(i / 1000.0);
        }
        s->append(ts.data(), reinterpret_cast<const std::byte*>(vs.data()), n);
        store_.add(s);
    };
    mk("Axis position", "mm", [](double t) { return 25.0 * std::sin(2 * M_PI * 1.5 * t); });
    mk("Axis velocity", "mm/s", [](double t) { return 230.0 * std::cos(2 * M_PI * 1.5 * t); });
    mk("Hydraulic pressure", "bar", [](double t) {
        return 90.0 + 18.0 * std::sin(2 * M_PI * 0.7 * t + 0.8)
             + 4.0 * std::sin(2 * M_PI * 11.0 * t);
    });
}

}  // namespace scope::app
