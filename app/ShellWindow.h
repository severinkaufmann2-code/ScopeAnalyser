#pragma once

#include "scope/core/SignalStore.h"

#include <QMainWindow>

class QLabel;
class QTabWidget;

namespace scope::app {

// Integrated shell hosting Recorder / Analyser / Converter in three tabs.
// All three bind to the same SignalStore so data flows freely between them.
// Also owns the app chrome: menu bar (theme switching, plot-controls help),
// status bar with a live store summary, and geometry/tab persistence.
class ShellWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit ShellWindow(QWidget* parent = nullptr);

    // Fills the store with a few demo signals. Only used by the offscreen
    // screenshot mode in main.cpp so layout reviews show real content.
    void seedDemoData();

    scope::core::SignalStore& store() { return store_; }

protected:
    void closeEvent(QCloseEvent* ev) override;

private:
    void buildMenus();
    void updateStoreSummary();

    scope::core::SignalStore store_;
    QTabWidget* tabs_{nullptr};
    QLabel*     storeSummary_{nullptr};
};

}  // namespace scope::app
