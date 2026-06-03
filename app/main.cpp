#include "ShellWindow.h"

#include <QApplication>

#include <spdlog/spdlog.h>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("ScopeAnalyser");
    QApplication::setOrganizationName("ScopeAnalyser");

    spdlog::set_level(spdlog::level::info);
    spdlog::info("ScopeAnalyser starting on {}/{}",
                 QSysInfo::prettyProductName().toStdString(),
                 QSysInfo::buildAbi().toStdString());

    scope::app::ShellWindow window;
    window.resize(1440, 900);
    window.show();

    return app.exec();
}
