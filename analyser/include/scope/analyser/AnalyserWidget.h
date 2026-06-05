#pragma once

#include "scope/core/SignalStore.h"

#include <QWidget>

#include <memory>

namespace scope::analyser {

class FormulaEngine;
namespace ui { class AnalyserPlot; }

// Thin wrapper around the multi-Y plot. Channel management (add, edit
// formula, save chart, autocomplete) lives inside AnalyserPlot's
// sidebar. The FormulaEngine is owned here so the dialog can access it
// via a non-owning reference.
class AnalyserWidget : public QWidget {
    Q_OBJECT
public:
    explicit AnalyserWidget(scope::core::SignalStore& store, QWidget* parent = nullptr);
    ~AnalyserWidget();

private:
    scope::core::SignalStore&       store_;
    std::unique_ptr<FormulaEngine>  engine_;
    ui::AnalyserPlot*               plot_{nullptr};
};

}  // namespace scope::analyser
