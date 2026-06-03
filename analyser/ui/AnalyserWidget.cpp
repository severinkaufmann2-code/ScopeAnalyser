#include "scope/analyser/AnalyserWidget.h"

#include <QLabel>
#include <QVBoxLayout>

namespace scope::analyser {

AnalyserWidget::AnalyserWidget(scope::core::SignalStore& store, QWidget* parent)
    : QWidget(parent), store_(store) {
    auto* label = new QLabel("Analyser — Phase 3 placeholder.\n"
                             "Will host: formula editor with autocomplete, function help,\n"
                             "linked-cursor multi-pane plot, derived channels.",
                             this);
    label->setAlignment(Qt::AlignCenter);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(label);
}

}  // namespace scope::analyser
