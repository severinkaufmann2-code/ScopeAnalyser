#pragma once

#include <QString>

#include <functional>
#include <memory>
#include <vector>

namespace scope::core { class Signal; }

namespace scope::analyser {

// A function invocation receives the already-evaluated argument signals (or
// scalar literals lifted to constant signals) and returns a new signal.
using FunctionArgs = std::vector<std::shared_ptr<scope::core::Signal>>;
using FunctionImpl = std::function<std::shared_ptr<scope::core::Signal>(
                         const FunctionArgs&, QString* errOut)>;

struct FunctionDescriptor {
    QString name;                       // case-sensitive, matches in parser
    QString signature;                  // human-readable: "Filter(signal, tau_seconds)"
    QString summary;                    // one line for tooltips
    QString help;                       // markdown for the help panel
    int     minArgs{0};
    int     maxArgs{0};                 // -1 = unbounded
    FunctionImpl impl;
};

class FunctionRegistry {
public:
    static FunctionRegistry& instance();

    void registerFunction(FunctionDescriptor desc);
    const std::vector<FunctionDescriptor>& all() const { return list_; }
    const FunctionDescriptor* find(const QString& name) const;

    // Idempotent. Called from FormulaEngine ctor.
    void registerBuiltins();

private:
    FunctionRegistry() = default;
    std::vector<FunctionDescriptor> list_;
    bool builtinsRegistered_{false};
};

}  // namespace scope::analyser
