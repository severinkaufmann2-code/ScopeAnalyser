#pragma once

#include <QString>

#include <vector>

namespace scope::analyser {

// Registry of formula functions surfaced to the editor's autocomplete and to
// the side-panel help. Single source of truth shared between the engine and
// the UI. Populated by the engine's plugin functions in Phase 3.
struct FunctionDescriptor {
    QString name;
    QString signature;   // e.g. "Filter(signal, tau_seconds)"
    QString summary;     // one line for tooltips
    QString help;        // markdown for the help panel
};

class FunctionRegistry {
public:
    static FunctionRegistry& instance();

    void registerFunction(FunctionDescriptor desc);
    const std::vector<FunctionDescriptor>& all() const { return list_; }

private:
    std::vector<FunctionDescriptor> list_;
};

}  // namespace scope::analyser
