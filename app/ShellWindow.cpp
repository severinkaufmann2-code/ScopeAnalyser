#include "ShellWindow.h"

#include "scope/recorder/RecorderWidget.h"
#include "scope/analyser/AnalyserWidget.h"
#include "scope/converter/ConverterWidget.h"

#include <QTabWidget>

namespace scope::app {

ShellWindow::ShellWindow(QWidget* parent) : QMainWindow(parent) {
    tabs_ = new QTabWidget(this);
    tabs_->addTab(new scope::recorder::RecorderWidget(store_, this),    "Recorder");
    tabs_->addTab(new scope::analyser::AnalyserWidget(store_, this),    "Analyser");
    tabs_->addTab(new scope::converter::ConverterWidget(store_, this),  "Converter");
    setCentralWidget(tabs_);
    setWindowTitle("ScopeAnalyser");
}

}  // namespace scope::app
