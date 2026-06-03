#pragma once

#include "scope/core/SignalStore.h"

#include <QWidget>

namespace scope::analyser {

// Phase 3 will implement: formula editor with autocomplete, function help
// panel, multi-pane linked-cursor plot, derived-channel materialisation.
// Phase 1 ships an empty placeholder that compiles into the shell so the
// build graph stays whole.
class AnalyserWidget : public QWidget {
    Q_OBJECT
public:
    explicit AnalyserWidget(scope::core::SignalStore& store, QWidget* parent = nullptr);

private:
    scope::core::SignalStore& store_;
};

}  // namespace scope::analyser
