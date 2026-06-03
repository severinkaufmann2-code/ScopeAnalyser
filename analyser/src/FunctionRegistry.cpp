#include "scope/analyser/FunctionRegistry.h"
#include "scope/core/Signal.h"

#include <QString>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>

namespace scope::analyser {

using scope::core::Signal;
using scope::core::DataType;
using scope::core::TimestampNs;

namespace {

// All derived signals are produced as Float64 — keeps the math simple and
// matches the typical scope/oscilloscope use case.
std::shared_ptr<Signal> makeDoubleSignal(const QString& sourceFormula,
                                         const std::vector<TimestampNs>& ts,
                                         const std::vector<double>& vals,
                                         const QString& unit = "") {
    Signal::Meta meta;
    meta.name = sourceFormula;          // overridden by caller before add()
    meta.unit = unit;
    meta.dataType = DataType::Float64;
    meta.sourceSymbol = sourceFormula;
    if (ts.size() >= 2) {
        const double dtNs = static_cast<double>(ts.back() - ts.front());
        if (dtNs > 0) meta.sampleRateHz = (ts.size() - 1) * 1e9 / dtNs;
    }
    auto sig = std::make_shared<Signal>(meta);
    sig->append(ts.data(),
                reinterpret_cast<const std::byte*>(vals.data()),
                vals.size());
    return sig;
}

// dt[i] in seconds; uses central differences except at boundaries.
std::vector<double> samplePeriodsSec(const std::vector<TimestampNs>& ts) {
    std::vector<double> dt(ts.size(), 0.0);
    if (ts.size() < 2) return dt;
    dt[0] = (ts[1] - ts[0]) * 1e-9;
    for (std::size_t i = 1; i + 1 < ts.size(); ++i) {
        dt[i] = (ts[i + 1] - ts[i - 1]) * 0.5e-9;
    }
    dt.back() = (ts.back() - ts[ts.size() - 2]) * 1e-9;
    return dt;
}

bool ensureN(const FunctionArgs& a, int n, QString* err, const char* who) {
    if (static_cast<int>(a.size()) != n) {
        if (err) *err = QString("%1 expects %2 arg(s), got %3")
                            .arg(who).arg(n).arg(a.size());
        return false;
    }
    return true;
}

// Lift a constant-valued signal back to a scalar. Constants are produced by
// the parser when it encounters a literal in an arg position; they have a
// single sample whose timestamp is 0.
bool asScalar(const std::shared_ptr<Signal>& s, double& out) {
    if (!s) return false;
    auto view = s->snapshotForRead();
    if (view.count != 1) return false;
    auto vals = s->readAsDouble();
    if (vals.empty()) return false;
    out = vals[0];
    return true;
}

// ---- Filter(signal, tau_seconds) ----
std::shared_ptr<Signal> impl_Filter(const FunctionArgs& a, QString* err) {
    if (!ensureN(a, 2, err, "Filter")) return nullptr;
    auto signal = a[0];
    double tau = 0;
    if (!asScalar(a[1], tau) || tau <= 0) {
        if (err) *err = "Filter: tau must be a positive scalar (seconds)";
        return nullptr;
    }
    auto view = signal->snapshotForRead();
    auto src  = signal->readAsDouble();
    std::vector<TimestampNs> ts(view.timestamps, view.timestamps + view.count);
    std::vector<double> dst(view.count);
    if (view.count == 0) return makeDoubleSignal("Filter", ts, dst, signal->meta().unit);
    auto dt = samplePeriodsSec(ts);
    dst[0] = src[0];
    for (std::size_t i = 1; i < view.count; ++i) {
        const double alpha = dt[i] / (tau + dt[i]);
        dst[i] = alpha * src[i] + (1.0 - alpha) * dst[i - 1];
    }
    return makeDoubleSignal("Filter", ts, dst, signal->meta().unit);
}

// ---- Integral(signal) ----
std::shared_ptr<Signal> impl_Integral(const FunctionArgs& a, QString* err) {
    if (!ensureN(a, 1, err, "Integral")) return nullptr;
    auto signal = a[0];
    auto view = signal->snapshotForRead();
    auto src  = signal->readAsDouble();
    std::vector<TimestampNs> ts(view.timestamps, view.timestamps + view.count);
    std::vector<double> dst(view.count, 0.0);
    if (view.count == 0) return makeDoubleSignal("Integral", ts, dst);

    double acc = 0.0;
    dst[0] = 0.0;
    for (std::size_t i = 1; i < view.count; ++i) {
        const double dt = (ts[i] - ts[i - 1]) * 1e-9;
        acc += 0.5 * (src[i] + src[i - 1]) * dt;
        dst[i] = acc;
    }
    return makeDoubleSignal("Integral", ts, dst);
}

// ---- Derivative(signal) ----
std::shared_ptr<Signal> impl_Derivative(const FunctionArgs& a, QString* err) {
    if (!ensureN(a, 1, err, "Derivative")) return nullptr;
    auto signal = a[0];
    auto view = signal->snapshotForRead();
    auto src  = signal->readAsDouble();
    std::vector<TimestampNs> ts(view.timestamps, view.timestamps + view.count);
    std::vector<double> dst(view.count, 0.0);
    if (view.count < 2) return makeDoubleSignal("Derivative", ts, dst);
    for (std::size_t i = 1; i + 1 < view.count; ++i) {
        const double dt = (ts[i + 1] - ts[i - 1]) * 1e-9;
        dst[i] = (src[i + 1] - src[i - 1]) / (dt != 0 ? dt : 1e-12);
    }
    // Edges: forward/backward difference
    if (view.count >= 2) {
        double dtF = (ts[1] - ts[0]) * 1e-9;
        if (dtF != 0) dst[0] = (src[1] - src[0]) / dtF;
        double dtB = (ts.back() - ts[ts.size() - 2]) * 1e-9;
        if (dtB != 0) dst.back() = (src.back() - src[src.size() - 2]) / dtB;
    }
    return makeDoubleSignal("Derivative", ts, dst);
}

// ---- Rolling reductions over a time window (seconds) ----
template <typename Reduce>
std::shared_ptr<Signal> rolling(const FunctionArgs& a, QString* err,
                                const char* who, Reduce reduce) {
    if (!ensureN(a, 2, err, who)) return nullptr;
    auto signal = a[0];
    double window = 0;
    if (!asScalar(a[1], window) || window <= 0) {
        if (err) *err = QString("%1: window must be a positive scalar (seconds)").arg(who);
        return nullptr;
    }
    auto view = signal->snapshotForRead();
    auto src  = signal->readAsDouble();
    std::vector<TimestampNs> ts(view.timestamps, view.timestamps + view.count);
    std::vector<double> dst(view.count);
    if (view.count == 0) return makeDoubleSignal(who, ts, dst);

    const TimestampNs windowNs = static_cast<TimestampNs>(window * 1e9);
    std::size_t lo = 0;
    for (std::size_t i = 0; i < view.count; ++i) {
        while (ts[i] - ts[lo] > windowNs) ++lo;
        dst[i] = reduce(src, lo, i);
    }
    return makeDoubleSignal(who, ts, dst, signal->meta().unit);
}

std::shared_ptr<Signal> impl_Mean(const FunctionArgs& a, QString* err) {
    return rolling(a, err, "Mean", [](const std::vector<double>& src, std::size_t lo, std::size_t hi){
        double sum = 0;
        for (std::size_t k = lo; k <= hi; ++k) sum += src[k];
        return sum / (hi - lo + 1);
    });
}
std::shared_ptr<Signal> impl_RMS(const FunctionArgs& a, QString* err) {
    return rolling(a, err, "RMS", [](const std::vector<double>& src, std::size_t lo, std::size_t hi){
        double sum = 0;
        for (std::size_t k = lo; k <= hi; ++k) sum += src[k] * src[k];
        return std::sqrt(sum / (hi - lo + 1));
    });
}
std::shared_ptr<Signal> impl_Min(const FunctionArgs& a, QString* err) {
    return rolling(a, err, "Min", [](const std::vector<double>& src, std::size_t lo, std::size_t hi){
        double m = src[lo];
        for (std::size_t k = lo + 1; k <= hi; ++k) m = std::min(m, src[k]);
        return m;
    });
}
std::shared_ptr<Signal> impl_Max(const FunctionArgs& a, QString* err) {
    return rolling(a, err, "Max", [](const std::vector<double>& src, std::size_t lo, std::size_t hi){
        double m = src[lo];
        for (std::size_t k = lo + 1; k <= hi; ++k) m = std::max(m, src[k]);
        return m;
    });
}

// ---- Shift(signal, seconds) — time shift only, no resample ----
std::shared_ptr<Signal> impl_Shift(const FunctionArgs& a, QString* err) {
    if (!ensureN(a, 2, err, "Shift")) return nullptr;
    auto signal = a[0];
    double secs = 0;
    if (!asScalar(a[1], secs)) {
        if (err) *err = "Shift: offset must be a scalar (seconds)";
        return nullptr;
    }
    auto view = signal->snapshotForRead();
    auto src  = signal->readAsDouble();
    std::vector<TimestampNs> ts(view.count);
    const TimestampNs offset = static_cast<TimestampNs>(secs * 1e9);
    for (std::size_t i = 0; i < view.count; ++i) ts[i] = view.timestamps[i] + offset;
    return makeDoubleSignal("Shift", ts, src, signal->meta().unit);
}

// ---- Elementwise unary functions ----
template <typename Op>
std::shared_ptr<Signal> elementwiseUnary(const FunctionArgs& a, QString* err,
                                         const char* who, Op op) {
    if (!ensureN(a, 1, err, who)) return nullptr;
    auto signal = a[0];
    auto view = signal->snapshotForRead();
    auto src  = signal->readAsDouble();
    std::vector<TimestampNs> ts(view.timestamps, view.timestamps + view.count);
    std::vector<double> dst(view.count);
    for (std::size_t i = 0; i < view.count; ++i) dst[i] = op(src[i]);
    return makeDoubleSignal(who, ts, dst, signal->meta().unit);
}

std::shared_ptr<Signal> impl_Abs(const FunctionArgs& a, QString* err)  {
    return elementwiseUnary(a, err, "Abs",  [](double v){ return std::abs(v); });
}
std::shared_ptr<Signal> impl_Sqrt(const FunctionArgs& a, QString* err) {
    return elementwiseUnary(a, err, "Sqrt", [](double v){ return std::sqrt(v); });
}
std::shared_ptr<Signal> impl_Log(const FunctionArgs& a, QString* err)  {
    return elementwiseUnary(a, err, "Log",  [](double v){ return std::log(v); });
}
std::shared_ptr<Signal> impl_Sin(const FunctionArgs& a, QString* err)  {
    return elementwiseUnary(a, err, "Sin",  [](double v){ return std::sin(v); });
}
std::shared_ptr<Signal> impl_Cos(const FunctionArgs& a, QString* err)  {
    return elementwiseUnary(a, err, "Cos",  [](double v){ return std::cos(v); });
}

}  // namespace

FunctionRegistry& FunctionRegistry::instance() {
    static FunctionRegistry reg;
    reg.registerBuiltins();
    return reg;
}

void FunctionRegistry::registerFunction(FunctionDescriptor desc) {
    list_.push_back(std::move(desc));
}

const FunctionDescriptor* FunctionRegistry::find(const QString& name) const {
    for (const auto& f : list_) if (f.name == name) return &f;
    return nullptr;
}

void FunctionRegistry::registerBuiltins() {
    if (builtinsRegistered_) return;
    builtinsRegistered_ = true;

    auto add = [this](QString n, QString sig, QString sum, int amin, int amax, FunctionImpl impl) {
        registerFunction({n, sig, sum, sum, amin, amax, std::move(impl)});
    };
    add("Filter",     "Filter(signal, tau_seconds)",
        "1st-order low-pass IIR with time constant tau.", 2, 2, &impl_Filter);
    add("Integral",   "Integral(signal)",
        "Trapezoidal cumulative integration.", 1, 1, &impl_Integral);
    add("Derivative", "Derivative(signal)",
        "Central-difference time derivative.", 1, 1, &impl_Derivative);
    add("Mean",       "Mean(signal, window_seconds)",
        "Rolling arithmetic mean over the last window_seconds.", 2, 2, &impl_Mean);
    add("RMS",        "RMS(signal, window_seconds)",
        "Rolling root-mean-square.", 2, 2, &impl_RMS);
    add("Min",        "Min(signal, window_seconds)",
        "Rolling minimum.", 2, 2, &impl_Min);
    add("Max",        "Max(signal, window_seconds)",
        "Rolling maximum.", 2, 2, &impl_Max);
    add("Shift",      "Shift(signal, seconds)",
        "Time-shift the signal by offset (positive shifts forward in time).", 2, 2, &impl_Shift);
    add("Abs",        "Abs(signal)",
        "Element-wise absolute value.", 1, 1, &impl_Abs);
    add("Sqrt",       "Sqrt(signal)",
        "Element-wise square root.", 1, 1, &impl_Sqrt);
    add("Log",        "Log(signal)",
        "Element-wise natural logarithm.", 1, 1, &impl_Log);
    add("Sin",        "Sin(signal)",
        "Element-wise sine.", 1, 1, &impl_Sin);
    add("Cos",        "Cos(signal)",
        "Element-wise cosine.", 1, 1, &impl_Cos);
}

}  // namespace scope::analyser
