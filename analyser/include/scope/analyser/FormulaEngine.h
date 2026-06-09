#pragma once

#include "scope/core/SignalStore.h"

#include <QHash>
#include <QString>
#include <QStringList>

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
    // On success the (name -> expression) pair is remembered so the channel
    // can be re-saved and recomputed later.
    bool evaluate(const QString& sourceLine, QString* errorOut = nullptr);

    // ---- Formula registry --------------------------------------------
    // The engine is the source of truth for which channels are derived and
    // by what expression. This survives a save/reopen even when the file
    // format doesn't round-trip Signal::Meta::sourceSymbol (e.g. CSV), so
    // formulas aren't silently dropped on the next save.

    // Expression (RHS only, no "name =") for a derived channel, or empty.
    QString formulaFor(const QString& name) const;

    // Record a formula without evaluating it (used when reopening a file
    // whose sources aren't available yet — keeps the definition so it can
    // be saved again and recomputed once sources appear).
    void rememberFormula(const QString& name, const QString& expr);

    // Forget a derived channel's formula (e.g. when it's removed).
    void forget(const QString& name);

    // Re-evaluate every remembered formula against the current store, in
    // dependency order (multi-pass: a formula that references another
    // formula resolves once its dependency has been recomputed). Returns
    // the number successfully (re-)evaluated; failures are reported via
    // errorsOut and their definitions are retained. No-op (returns 0) when
    // already recomputing, so it's safe to call from store change handlers.
    int recomputeAll(QStringList* errorsOut = nullptr);

    bool isRecomputing() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace scope::analyser
