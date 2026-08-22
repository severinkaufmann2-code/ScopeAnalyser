#include "scope/analyser/AnalyserWidget.h"
#include "scope/core/SignalStore.h"
#include "scope/style/StyleKit.h"
#include "scope/style/Theme.h"

#include <QApplication>
#include <QMainWindow>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("ScopeAnalyser");
    QApplication::setOrganizationName("ScopeAnalyser");

    scope::style::applySavedTheme();

    QApplication::setWindowIcon(scope::style::appIcon());

    QGuiApplication::setDesktopFileName("ScopeAnalyser");


    scope::core::SignalStore store;
    auto* widget = new scope::analyser::AnalyserWidget(store);

    QMainWindow window;
    window.setCentralWidget(widget);
    window.setWindowTitle("Scope Analyser");
    window.resize(1280, 800);
    window.show();

    return app.exec();
}
