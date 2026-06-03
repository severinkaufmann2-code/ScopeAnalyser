#pragma once

#include "scope/core/SignalStore.h"

#include <QString>

#include <memory>

namespace scope::analyser {

// Tiny recursive-descent expression engine that turns a "<Out> = <expr>" line
// into a derived signal in the SignalStore. Channel names resolve against the
// store; function calls dispatch through FunctionRegistry. Output signals are
// registered with the formula text stored in Signal::Meta::sourceSymbol so
// they can be re-evaluated later.
//
// Supported syntax:
//   Out = Ch1                      // alias
//   Out = Ch1 + Ch2                // elementwise (channels must be same length)
//   Out = 2 * Ch1 - 5              // scalar mixing
//   Out = Filter(Ch1, 0.1)         // function with positional args
//   Out = Filter(Ch1, 0.1) + Ch2   // nested
//   Out = -Ch1                     // unary minus
//
// Two signals participating in elementwise math must currently have identical
// length and timestamps. Resampling across mismatched rates is Phase 3b.
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
