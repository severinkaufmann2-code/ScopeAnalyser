#include "scope/analyser/AnalyserWidget.h"
#include "scope/analyser/FormulaEngine.h"

#include "AnalyserPlot.h"

#include <QVBoxLayout>

namespace scope::analyser {

AnalyserWidget::AnalyserWidget(scope::core::SignalStore& store, QWidget* parent)
    : QWidget(parent), store_(store) {
    engine_ = std::make_unique<FormulaEngine>(store_);
    plot_   = new ui::AnalyserPlot(store_, *engine_, this);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(plot_);
}

AnalyserWidget::~AnalyserWidget() = default;

}  // namespace scope::analyser
