#include "ShellWindow.h"

#include "scope/converter/HtmlExport.h"
#include "scope/style/Theme.h"

#include <QApplication>
#include <QDir>
#include <QTabWidget>
#include <QTimer>

#include <spdlog/spdlog.h>

namespace {

// Dev/CI helper: with SCOPE_SHELL_SCREENSHOT_DIR set (typically together
// with QT_QPA_PLATFORM=offscreen), seed demo signals, render each tab to
// <dir>/tab_<n>_<name>.png, and exit. No effect otherwise.
bool runScreenshotMode(scope::app::ShellWindow& window) {
    const QString dir =
        qEnvironmentVariable("SCOPE_SHELL_SCREENSHOT_DIR");
    if (dir.isEmpty()) return false;
    QDir().mkpath(dir);
    window.seedDemoData();
    QTimer::singleShot(400, &window, [&window, dir] {
        auto* tabs = window.findChild<QTabWidget*>();
        for (int i = 0; tabs && i < tabs->count(); ++i) {
            tabs->setCurrentIndex(i);
            QCoreApplication::processEvents();
            window.grab().save(QString("%1/tab_%2_%3.png")
                                   .arg(dir).arg(i)
                                   .arg(tabs->tabText(i).toLower()));
        }
        // Also exercise the interactive-HTML exporter on the demo data so
        // a reviewer can open the artifact in a browser.
        QString err;
        scope::converter::exportInteractiveHtml(
            dir + "/demo_interactive.html", window.store(), &err);
        QCoreApplication::quit();
    });
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("ScopeAnalyser");
    QApplication::setOrganizationName("ScopeAnalyser");

    scope::style::applySavedTheme();

    spdlog::set_level(spdlog::level::info);
    spdlog::info("ScopeAnalyser starting on {}/{}",
                 QSysInfo::prettyProductName().toStdString(),
                 QSysInfo::buildAbi().toStdString());

    scope::app::ShellWindow window;
    window.resize(1440, 900);
    window.show();

    runScreenshotMode(window);

    return app.exec();
}
