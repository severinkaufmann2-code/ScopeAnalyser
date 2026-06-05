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

// ---- Derivative(signal) — central difference ----
// ---- Derivative(signal, window_seconds) — least-squares slope ----
//
// One-arg form is the textbook central-difference derivative. For smooth
// signals this gives the best point-wise accuracy.
//
// Two-arg form: at each sample, fit a line through every sample within
// ±window_seconds/2 of the current time and report that line's slope.
// This is robust against sample-to-sample quantisation, corners, and
// noise — it's what you want for real recorded signals where the raw
// central or forward differences end up alternating wildly between
// adjacent samples. Pick the window roughly to match the timescale of
// the trend you care about (a few sample periods, or the timescale of
// the smoothest underlying motion).
std::shared_ptr<Signal> impl_Derivative(const FunctionArgs& a, QString* err) {
    if (a.size() != 1 && a.size() != 2) {
        if (err) *err = QString("Derivative expects 1 or 2 arg(s), got %1")
                            .arg(a.size());
        return nullptr;
    }
    auto signal = a[0];
    auto view = signal->snapshotForRead();
    auto src  = signal->readAsDouble();
    std::vector<TimestampNs> ts(view.timestamps, view.timestamps + view.count);
    std::vector<double> dst(view.count, 0.0);
    if (view.count < 2) return makeDoubleSignal("Derivative", ts, dst);

    // Two-arg form: least-squares slope over a centred time window.
    if (a.size() == 2) {
        double windowSec = 0;
        if (!asScalar(a[1], windowSec) || windowSec <= 0) {
            if (err) *err = "Derivative: window must be a positive scalar (seconds)";
            return nullptr;
        }
        const TimestampNs halfNs =
            static_cast<TimestampNs>(windowSec * 1e9 / 2.0);
        std::size_t lo = 0, hi = 0;
        for (std::size_t i = 0; i < view.count; ++i) {
            const TimestampNs tCenter = ts[i];
            while (lo < view.count && tCenter - ts[lo] > halfNs) ++lo;
            while (hi + 1 < view.count && ts[hi + 1] - tCenter <= halfNs) ++hi;
            // Need ≥2 distinct timestamps to fit a slope; fall back to
            // central-difference if the window is tighter than the sample
            // spacing.
            if (hi <= lo) {
                if (i > 0 && i + 1 < view.count) {
                    const double dt = (ts[i + 1] - ts[i - 1]) * 1e-9;
                    dst[i] = (src[i + 1] - src[i - 1]) / (dt != 0 ? dt : 1e-12);
                }
                continue;
            }
            // Least-squares slope: m = Σ((x-x̄)(y-ȳ)) / Σ((x-x̄)²)
            const std::size_t n = hi - lo + 1;
            double sumX = 0, sumY = 0;
            for (std::size_t k = lo; k <= hi; ++k) {
                sumX += ts[k] * 1e-9;
                sumY += src[k];
            }
            const double meanX = sumX / static_cast<double>(n);
            const double meanY = sumY / static_cast<double>(n);
            double num = 0, den = 0;
            for (std::size_t k = lo; k <= hi; ++k) {
                const double dx = ts[k] * 1e-9 - meanX;
                num += dx * (src[k] - meanY);
                den += dx * dx;
            }
            dst[i] = (den != 0) ? num / den : 0.0;
        }
        return makeDoubleSignal("Derivative", ts, dst);
    }

    // One-arg form: central difference (default).
    for (std::size_t i = 1; i + 1 < view.count; ++i) {
        const double dt = (ts[i + 1] - ts[i - 1]) * 1e-9;
        dst[i] = (src[i + 1] - src[i - 1]) / (dt != 0 ? dt : 1e-12);
    }
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

// Helpers for Resample.
bool isConstantHere(const std::shared_ptr<Signal>& s) {
    if (!s) return false;
    auto view = s->snapshotForRead();
    return view.count == 1 && view.timestamps[0] == 0
        && s->meta().sourceSymbol.isEmpty();
}

double interpAtHere(const TimestampNs* ts, const std::vector<double>& vals,
                    std::size_t n, TimestampNs t, std::size_t& cursor) {
    if (n == 0) return 0.0;
    if (t <= ts[0]) return vals[0];
    if (t >= ts[n - 1]) return vals[n - 1];
    while (cursor + 1 < n && ts[cursor + 1] < t) ++cursor;
    const std::size_t hi = std::min(cursor + 1, n - 1);
    if (cursor == hi) return vals[cursor];
    const double dt = static_cast<double>(ts[hi] - ts[cursor]);
    if (dt == 0) return vals[cursor];
    const double frac = static_cast<double>(t - ts[cursor]) / dt;
    return vals[cursor] + frac * (vals[hi] - vals[cursor]);
}

// Resample(signal, target):
//   target = scalar constant  → resample to that target rate in Hz, evenly
//                                spaced timestamps over the signal's range.
//   target = another signal   → resample to that signal's timestamps,
//                                restricted to the source signal's range.
std::shared_ptr<Signal> impl_Resample(const FunctionArgs& a, QString* err) {
    if (!ensureN(a, 2, err, "Resample")) return nullptr;
    auto src = a[0];
    auto target = a[1];
    auto srcView = src->snapshotForRead();
    auto srcVals = src->readAsDouble();
    std::vector<TimestampNs> outTs;
    if (srcView.count == 0) return makeDoubleSignal("Resample", outTs, {}, src->meta().unit);

    if (isConstantHere(target)) {
        double rate = 0;
        if (!asScalar(target, rate) || rate <= 0) {
            if (err) *err = "Resample: target rate must be a positive scalar (Hz)";
            return nullptr;
        }
        const auto dtNs = static_cast<TimestampNs>(1e9 / rate);
        if (dtNs <= 0) {
            if (err) *err = "Resample: rate too high for nanosecond timestamps";
            return nullptr;
        }
        const TimestampNs first = srcView.timestamps[0];
        const TimestampNs last  = srcView.timestamps[srcView.count - 1];
        for (TimestampNs t = first; t <= last; t += dtNs) outTs.push_back(t);
    } else {
        auto tView = target->snapshotForRead();
        const TimestampNs lo = srcView.timestamps[0];
        const TimestampNs hi = srcView.timestamps[srcView.count - 1];
        outTs.reserve(tView.count);
        for (std::size_t i = 0; i < tView.count; ++i) {
            const TimestampNs t = tView.timestamps[i];
            if (t >= lo && t <= hi) outTs.push_back(t);
        }
    }

    std::vector<double> outVals;
    outVals.reserve(outTs.size());
    std::size_t cursor = 0;
    for (TimestampNs t : outTs) {
        outVals.push_back(
            interpAtHere(srcView.timestamps, srcVals, srcView.count, t, cursor));
    }
    return makeDoubleSignal("Resample", outTs, outVals, src->meta().unit);
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
    add("Derivative", "Derivative(signal[, window_seconds])",
        "Time derivative. With one arg: central difference (point-wise). "
        "With a window: least-squares slope over a centred time window — "
        "robust against quantisation, corners, and noise.",
        1, 2, &impl_Derivative);
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
    add("Resample",   "Resample(signal, rate_Hz_or_reference_signal)",
        "Linear-interpolate signal onto a new time grid. Pass a positive scalar "
        "for the target rate in Hz, or another signal to copy its timestamps.",
        2, 2, &impl_Resample);
}

}  // namespace scope::analyser
