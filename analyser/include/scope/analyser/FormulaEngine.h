#pragma once

#include "scope/core/SignalStore.h"

#include <QString>

#include <memory>

namespace scope::analyser {

// Phase 3: exprtk-backed vectorized expression engine. Stubbed for Phase 1.
class FormulaEngine {
public:
    explicit FormulaEngine(scope::core::SignalStore& store);
    ~FormulaEngine();

    // Compile + run an assignment of the form `OutName = <expr>` and write
    // the result as a derived Signal into the store. Returns false on error.
    bool evaluate(const QString& sourceLine, QString* errorOut = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace scope::analyser
