#pragma once

#include "scope/core/SignalStore.h"

#include <QMainWindow>

class QTabWidget;

namespace scope::app {

// Integrated shell hosting Recorder / Analyser / Converter in three tabs.
// All three bind to the same SignalStore so data flows freely between them.
class ShellWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit ShellWindow(QWidget* parent = nullptr);

private:
    scope::core::SignalStore store_;
    QTabWidget* tabs_;
};

}  // namespace scope::app
