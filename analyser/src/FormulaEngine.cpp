#include "scope/analyser/FormulaEngine.h"

namespace scope::analyser {

struct FormulaEngine::Impl {
    scope::core::SignalStore& store;
};

FormulaEngine::FormulaEngine(scope::core::SignalStore& store)
    : impl_(std::make_unique<Impl>(Impl{store})) {}

FormulaEngine::~FormulaEngine() = default;

bool FormulaEngine::evaluate(const QString& /*sourceLine*/, QString* errorOut) {
    if (errorOut) *errorOut = "FormulaEngine::evaluate: Phase 3";
    return false;
}

}  // namespace scope::analyser
