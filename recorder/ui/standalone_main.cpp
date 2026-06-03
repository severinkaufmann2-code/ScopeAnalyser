#include "scope/recorder/RecorderWidget.h"
#include "scope/core/SignalStore.h"

#include <QApplication>
#include <QMainWindow>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("ScopeRecorder");

    scope::core::SignalStore store;
    auto* widget = new scope::recorder::RecorderWidget(store);

    QMainWindow window;
    window.setCentralWidget(widget);
    window.setWindowTitle("Scope Recorder");
    window.resize(1280, 800);
    window.show();

    return app.exec();
}
