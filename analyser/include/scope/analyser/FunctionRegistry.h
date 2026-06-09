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

// ---- Filter frequency-response preview --------------------------------
// Computes the magnitude/phase response of a filter family for the Add-Channel
// help charts. The response is on a NOMINAL sample rate + cutoff (the real
// cutoff depends on the channel's sample rate), so it shows the *shape* —
// rolloff steepness, ripple, phase behaviour — against a normalized frequency
// axis (freqHz / fc). Reuses the same design pipeline the filters run.
struct FilterPlotSpec {
    QString family;        // "Filter", "PT", "Butterworth", "Cheby1", "Cheby2", "Elliptic"
    int     band{0};       // 0=low 1=high 2=band-pass 3=band-stop
    int     order{2};
    double  ripple{1.0};   // dB — Cheby1/Elliptic passband ripple / Cheby2 stop attenuation
    double  rippleStop{40.0};  // dB — Elliptic stopband attenuation (Rs)
};
struct FilterFreqResponse {
    std::vector<double> freqHz;     // log-spaced grid (nominal fs)
    std::vector<double> magDb;
    std::vector<double> phaseDeg;
    double fs{0.0};                 // nominal sample rate used
    double fc{0.0};                 // nominal cutoff (freqHz/fc = normalized axis)
};
FilterFreqResponse filterFreqResponse(const FilterPlotSpec& spec, int nPoints = 400);

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
