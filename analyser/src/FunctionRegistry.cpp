#include "scope/analyser/FunctionRegistry.h"
#include "scope/core/Signal.h"

#include <QString>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <deque>
#include <limits>

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
                                         const QString& unit = "",
                                         Signal::Domain domain = Signal::Domain::Time) {
    Signal::Meta meta;
    meta.name = sourceFormula;          // overridden by caller before add()
    meta.unit = unit;
    meta.dataType = DataType::Float64;
    meta.sourceSymbol = sourceFormula;
    meta.domain = domain;
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

// ---- FilterHP(signal, tau_seconds) ----
// 1st-order high-pass: the exact complement of the low-pass Filter — the
// signal minus its low-passed version. Removes slow drift / DC offset and
// passes fast changes; same -3 dB cutoff as Filter(tau). Filter(x,tau) +
// FilterHP(x,tau) reconstructs x sample-for-sample. Causal (has phase lag) —
// wrap in filtfilt() for zero phase.
std::shared_ptr<Signal> impl_FilterHP(const FunctionArgs& a, QString* err) {
    if (!ensureN(a, 2, err, "FilterHP")) return nullptr;
    auto signal = a[0];
    double tau = 0;
    if (!asScalar(a[1], tau) || tau <= 0) {
        if (err) *err = "FilterHP: tau must be a positive scalar (seconds)";
        return nullptr;
    }
    auto view = signal->snapshotForRead();
    auto src  = signal->readAsDouble();
    std::vector<TimestampNs> ts(view.timestamps, view.timestamps + view.count);
    std::vector<double> dst(view.count);
    if (view.count == 0)
        return makeDoubleSignal("FilterHP", ts, dst, signal->meta().unit);
    auto dt = samplePeriodsSec(ts);
    double lp = src[0];     // low-pass accumulator (same recurrence as Filter)
    dst[0] = 0.0;           // x[0] - lp == 0
    for (std::size_t i = 1; i < view.count; ++i) {
        const double alpha = dt[i] / (tau + dt[i]);
        lp = alpha * src[i] + (1.0 - alpha) * lp;
        dst[i] = src[i] - lp;
    }
    return makeDoubleSignal("FilterHP", ts, dst, signal->meta().unit);
}

// ---- Reverse(signal) — flip the waveform in time ----
// Values are reversed; timestamps stay ascending starting at the original
// first timestamp, with the inter-sample spacing reversed (so a causal filter
// applied to the result sees the reversed dt sequence). Reversing twice
// restores the original signal exactly — this is the backward step filtfilt()
// uses to turn any causal filter into a zero-phase one.
std::shared_ptr<Signal> impl_Reverse(const FunctionArgs& a, QString* err) {
    if (!ensureN(a, 1, err, "Reverse")) return nullptr;
    auto signal = a[0];
    auto view = signal->snapshotForRead();
    auto src  = signal->readAsDouble();
    const std::size_t n = view.count;
    std::vector<TimestampNs> ts(n);
    std::vector<double> dst(n);
    if (n == 0)
        return makeDoubleSignal("Reverse", ts, dst, signal->meta().unit,
                                signal->meta().domain);
    const TimestampNs t0 = view.timestamps[0];
    const TimestampNs tN = view.timestamps[n - 1];
    for (std::size_t i = 0; i < n; ++i) {
        ts[i]  = t0 + (tN - view.timestamps[n - 1 - i]);
        dst[i] = (n - 1 - i) < src.size() ? src[n - 1 - i] : 0.0;
    }
    return makeDoubleSignal("Reverse", ts, dst, signal->meta().unit,
                            signal->meta().domain);
}

// ---- Butterworth low-/high-pass (shared core) ----
// Cascade of RBJ biquads using the per-section Butterworth Q (plus a 1st-order
// section for odd orders). The sample rate is derived from the timestamps — a
// uniform-rate assumption; for non-uniform data this is an approximation.
// Passband gain is unity. Causal, so it has phase lag — wrap in filtfilt() for
// zero phase. `highpass` selects the high-pass coefficient set.
static std::shared_ptr<Signal> butterworthImpl(const FunctionArgs& a,
                                               bool highpass,
                                               const QString& name,
                                               QString* err) {
    if (a.size() < 2 || a.size() > 3) {
        if (err) *err = QString("%1 expects 2..3 args, got %2")
                            .arg(name).arg(a.size());
        return nullptr;
    }
    auto signal = a[0];
    double fc = 0.0, orderArg = 2.0;
    if (!asScalar(a[1], fc) || fc <= 0) {
        if (err) *err = QString("%1: cutoff_hz must be a positive scalar").arg(name);
        return nullptr;
    }
    if (a.size() == 3 && (!asScalar(a[2], orderArg) || orderArg < 1)) {
        if (err) *err = QString("%1: order must be an integer >= 1").arg(name);
        return nullptr;
    }
    const int order = std::max(1, static_cast<int>(std::lround(orderArg)));

    auto view = signal->snapshotForRead();
    auto src  = signal->readAsDouble();
    std::vector<TimestampNs> ts(view.timestamps, view.timestamps + view.count);
    std::vector<double> dst(src.begin(), src.end());
    if (view.count < 2)
        return makeDoubleSignal(name, ts, dst, signal->meta().unit);

    const double spanSec = (ts.back() - ts.front()) * 1e-9;
    if (spanSec <= 0) {
        if (err) *err = QString("%1: timestamps must be increasing").arg(name);
        return nullptr;
    }
    const double fs = (view.count - 1) / spanSec;
    if (fc >= fs / 2.0) {
        if (err) *err = QString("%1: cutoff %2 Hz must be below the Nyquist "
                                "frequency (%3 Hz)").arg(name).arg(fc).arg(fs / 2.0);
        return nullptr;
    }

    const double w0   = 2.0 * M_PI * fc / fs;
    const double cosw = std::cos(w0);
    const double sinw = std::sin(w0);

    // Direct-form-I biquad applied in place over dst.
    auto applyBiquad = [&](double b0, double b1, double b2, double a1, double a2) {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        for (double& sample : dst) {
            const double x0 = sample;
            const double y0 = b0 * x0 + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = x0; y2 = y1; y1 = y0;
            sample = y0;
        }
    };

    const int nBiquads = order / 2;
    for (int m = 0; m < nBiquads; ++m) {
        // Butterworth pole-pair angle → section Q. cos(phi) < 0 here, so Q > 0.
        const double phi = M_PI * (2.0 * m + order + 1.0) / (2.0 * order);
        const double Q   = -1.0 / (2.0 * std::cos(phi));
        const double alpha = sinw / (2.0 * Q);
        const double a0 = 1.0 + alpha;
        if (highpass) {
            applyBiquad( (1.0 + cosw) / 2.0 / a0,
                        -(1.0 + cosw)       / a0,
                         (1.0 + cosw) / 2.0 / a0,
                         (-2.0 * cosw)      / a0,
                         (1.0 - alpha)      / a0);
        } else {
            applyBiquad((1.0 - cosw) / 2.0 / a0,
                        (1.0 - cosw)       / a0,
                        (1.0 - cosw) / 2.0 / a0,
                        (-2.0 * cosw)      / a0,
                        (1.0 - alpha)      / a0);
        }
    }
    if (order % 2 == 1) {
        // Odd order → one real pole: a 1st-order section at fc (bilinear).
        const double c  = std::tan(w0 / 2.0);
        const double a1 = (c - 1.0) / (1.0 + c);
        const double b0 = highpass ? 1.0 / (1.0 + c) : c / (1.0 + c);
        const double b1 = highpass ? -b0 : b0;
        double x1 = 0, y1 = 0;
        for (double& sample : dst) {
            const double x0 = sample;
            const double y0 = b0 * x0 + b1 * x1 - a1 * y1;
            x1 = x0; y1 = y0;
            sample = y0;
        }
    }
    return makeDoubleSignal(name, ts, dst, signal->meta().unit);
}

std::shared_ptr<Signal> impl_Butterworth(const FunctionArgs& a, QString* err) {
    return butterworthImpl(a, /*highpass=*/false, "Butterworth", err);
}
std::shared_ptr<Signal> impl_ButterworthHP(const FunctionArgs& a, QString* err) {
    return butterworthImpl(a, /*highpass=*/true, "ButterworthHP", err);
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

// ---- Derivative(signal) — plain Δy/Δx (central difference) ----
//
// Textbook central difference on the interior, forward at the start,
// backward at the end. Same shape as numpy.gradient.
//
//   dst[0]      = (v[1]   - v[0])   / (t[1]   - t[0])
//   dst[i]      = (v[i+1] - v[i-1]) / (t[i+1] - t[i-1])   for 1 ≤ i ≤ N-2
//   dst[N-1]    = (v[N-1] - v[N-2]) / (t[N-1] - t[N-2])
//
// If the source signal is staircase-quantised (a PLC value that stays
// flat for several samples then jumps), this produces the classic
// comb pattern — that's a *data shape* issue, fix it at import with
// the Converter's "Value plateaus" collapse option. For noise, compose
// with Filter:  Derivative(Filter(s, tau)).
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
    {
        const double dtF = (ts[1] - ts[0]) * 1e-9;
        if (dtF != 0) dst[0] = (src[1] - src[0]) / dtF;
        const double dtB = (ts.back() - ts[ts.size() - 2]) * 1e-9;
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
std::shared_ptr<Signal> impl_RollingMin(const FunctionArgs& a, QString* err) {
    return rolling(a, err, "RollingMin",
        [](const std::vector<double>& src, std::size_t lo, std::size_t hi){
            double m = src[lo];
            for (std::size_t k = lo + 1; k <= hi; ++k) m = std::min(m, src[k]);
            return m;
        });
}
std::shared_ptr<Signal> impl_RollingMax(const FunctionArgs& a, QString* err) {
    return rolling(a, err, "RollingMax",
        [](const std::vector<double>& src, std::size_t lo, std::size_t hi){
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
std::shared_ptr<Signal> impl_Tan(const FunctionArgs& a, QString* err)  {
    return elementwiseUnary(a, err, "Tan",  [](double v){ return std::tan(v); });
}
std::shared_ptr<Signal> impl_Asin(const FunctionArgs& a, QString* err) {
    return elementwiseUnary(a, err, "Asin", [](double v){ return std::asin(v); });
}
std::shared_ptr<Signal> impl_Acos(const FunctionArgs& a, QString* err) {
    return elementwiseUnary(a, err, "Acos", [](double v){ return std::acos(v); });
}
std::shared_ptr<Signal> impl_Atan(const FunctionArgs& a, QString* err) {
    return elementwiseUnary(a, err, "Atan", [](double v){ return std::atan(v); });
}
std::shared_ptr<Signal> impl_Exp(const FunctionArgs& a, QString* err)  {
    return elementwiseUnary(a, err, "Exp",  [](double v){ return std::exp(v); });
}
std::shared_ptr<Signal> impl_Log10(const FunctionArgs& a, QString* err) {
    return elementwiseUnary(a, err, "Log10", [](double v){ return std::log10(v); });
}
std::shared_ptr<Signal> impl_Floor(const FunctionArgs& a, QString* err) {
    return elementwiseUnary(a, err, "Floor", [](double v){ return std::floor(v); });
}
std::shared_ptr<Signal> impl_Ceil(const FunctionArgs& a, QString* err) {
    return elementwiseUnary(a, err, "Ceil",  [](double v){ return std::ceil(v); });
}
std::shared_ptr<Signal> impl_Round(const FunctionArgs& a, QString* err) {
    return elementwiseUnary(a, err, "Round", [](double v){ return std::round(v); });
}
std::shared_ptr<Signal> impl_Sign(const FunctionArgs& a, QString* err) {
    return elementwiseUnary(a, err, "Sign", [](double v){
        return (v > 0) - (v < 0);
    });
}

// ---- Elementwise binary functions ----
// Mirrors FormulaEngine.cpp's elementwiseBinary but uses *err instead of
// EvalCtx so it's reachable from named-function impls. Same three paths:
// constant broadcast → fast same-grid → resample on the intersection.
template <typename Op>
std::shared_ptr<Signal> elementwiseBinary(const std::shared_ptr<Signal>& a,
                                          const std::shared_ptr<Signal>& b,
                                          Op op,
                                          QString* err,
                                          const char* opName) {
    auto avView = a->snapshotForRead();
    auto bvView = b->snapshotForRead();
    auto av = a->readAsDouble();
    auto bv = b->readAsDouble();
    const bool aConst = (avView.count == 1 && avView.timestamps[0] == 0);
    const bool bConst = (bvView.count == 1 && bvView.timestamps[0] == 0);

    auto makeSig = [&](const std::vector<TimestampNs>& ts,
                       const std::vector<double>& vs) {
        Signal::Meta m;
        m.dataType = DataType::Float64;
        m.name = opName;
        auto s = std::make_shared<Signal>(m);
        s->append(ts.data(),
                  reinterpret_cast<const std::byte*>(vs.data()),
                  vs.size());
        return s;
    };

    if (aConst || bConst) {
        const std::size_t n = aConst ? bvView.count : avView.count;
        const TimestampNs* ts = aConst ? bvView.timestamps : avView.timestamps;
        std::vector<double> out(n);
        for (std::size_t i = 0; i < n; ++i) {
            const double va = aConst ? av[0] : av[i];
            const double vb = bConst ? bv[0] : bv[i];
            out[i] = op(va, vb);
        }
        std::vector<TimestampNs> outTs(ts, ts + n);
        return makeSig(outTs, out);
    }

    if (avView.count == 0 || bvView.count == 0) {
        if (err) *err = QString("%1: empty operand").arg(opName);
        return nullptr;
    }

    if (avView.count == bvView.count
        && avView.timestamps[0] == bvView.timestamps[0]
        && avView.timestamps[avView.count - 1]
               == bvView.timestamps[bvView.count - 1]) {
        const std::size_t n = avView.count;
        std::vector<double> out(n);
        for (std::size_t i = 0; i < n; ++i) out[i] = op(av[i], bv[i]);
        std::vector<TimestampNs> outTs(avView.timestamps, avView.timestamps + n);
        return makeSig(outTs, out);
    }

    // Different grids: build a uniform grid over the intersection and
    // linearly interpolate both. Simpler than the parser's chooser.
    const TimestampNs tLo = std::max(avView.timestamps[0], bvView.timestamps[0]);
    const TimestampNs tHi = std::min(avView.timestamps[avView.count - 1],
                                     bvView.timestamps[bvView.count - 1]);
    if (tLo > tHi) {
        if (err) *err = QString("%1: signals don't overlap in time").arg(opName);
        return nullptr;
    }
    auto countInRange = [&](const Signal::ReadView& v) {
        std::size_t c = 0;
        for (std::size_t i = 0; i < v.count; ++i)
            if (v.timestamps[i] >= tLo && v.timestamps[i] <= tHi) ++c;
        return c;
    };
    const bool useA = countInRange(avView) >= countInRange(bvView);
    const auto& gridView = useA ? avView  : bvView;
    const auto& gridVals = useA ? av      : bv;
    const auto& otherView = useA ? bvView : avView;
    const auto& otherVals = useA ? bv     : av;

    auto interp = [&](const Signal::ReadView& v,
                      const std::vector<double>& vals,
                      TimestampNs t) -> double {
        if (v.count == 0) return 0;
        if (t <= v.timestamps[0]) return vals.front();
        if (t >= v.timestamps[v.count - 1]) return vals.back();
        // binary search
        std::size_t lo = 0, hi = v.count - 1;
        while (lo + 1 < hi) {
            const std::size_t mid = (lo + hi) / 2;
            if (v.timestamps[mid] <= t) lo = mid;
            else hi = mid;
        }
        const double t0 = static_cast<double>(v.timestamps[lo]);
        const double t1 = static_cast<double>(v.timestamps[hi]);
        const double f = (static_cast<double>(t) - t0) / (t1 - t0);
        return vals[lo] + f * (vals[hi] - vals[lo]);
    };

    std::vector<TimestampNs> outTs;
    std::vector<double> outVs;
    outTs.reserve(gridView.count);
    outVs.reserve(gridView.count);
    for (std::size_t i = 0; i < gridView.count; ++i) {
        const TimestampNs t = gridView.timestamps[i];
        if (t < tLo || t > tHi) continue;
        const double va = useA ? gridVals[i] : interp(otherView, otherVals, t);
        const double vb = useA ? interp(otherView, otherVals, t) : gridVals[i];
        outTs.push_back(t);
        outVs.push_back(op(va, vb));
    }
    return makeSig(outTs, outVs);
}

std::shared_ptr<Signal> impl_Power(const FunctionArgs& a, QString* err) {
    if (!ensureN(a, 2, err, "Power")) return nullptr;
    return elementwiseBinary(a[0], a[1],
        [](double x, double e){ return std::pow(x, e); }, err, "Power");
}
std::shared_ptr<Signal> impl_Mod(const FunctionArgs& a, QString* err) {
    if (!ensureN(a, 2, err, "Mod")) return nullptr;
    return elementwiseBinary(a[0], a[1],
        [](double x, double d){ return d != 0 ? std::fmod(x, d) : 0.0; },
        err, "Mod");
}
std::shared_ptr<Signal> impl_Atan2(const FunctionArgs& a, QString* err) {
    if (!ensureN(a, 2, err, "Atan2")) return nullptr;
    return elementwiseBinary(a[0], a[1],
        [](double y, double x){ return std::atan2(y, x); }, err, "Atan2");
}

// Limit(signal, lo, hi) — three args. lo/hi must be scalars.
std::shared_ptr<Signal> impl_Limit(const FunctionArgs& a, QString* err) {
    if (!ensureN(a, 3, err, "Limit")) return nullptr;
    double lo = 0, hi = 0;
    if (!asScalar(a[1], lo) || !asScalar(a[2], hi)) {
        if (err) *err = "Limit: lo and hi must be scalars";
        return nullptr;
    }
    if (lo > hi) std::swap(lo, hi);
    auto signal = a[0];
    auto view = signal->snapshotForRead();
    auto src  = signal->readAsDouble();
    std::vector<TimestampNs> ts(view.timestamps, view.timestamps + view.count);
    std::vector<double> dst(view.count);
    for (std::size_t i = 0; i < view.count; ++i)
        dst[i] = std::clamp(src[i], lo, hi);
    return makeDoubleSignal("Limit", ts, dst, signal->meta().unit);
}

// Min(a, b) / Max(a, b) — element-wise pair (Python's min(a, b), or
// numpy.minimum / numpy.maximum). Either arg can be a signal or a scalar.
std::shared_ptr<Signal> impl_MinPair(const FunctionArgs& a, QString* err) {
    if (!ensureN(a, 2, err, "Min")) return nullptr;
    return elementwiseBinary(a[0], a[1],
        [](double x, double y){ return std::min(x, y); }, err, "Min");
}
std::shared_ptr<Signal> impl_MaxPair(const FunctionArgs& a, QString* err) {
    if (!ensureN(a, 2, err, "Max")) return nullptr;
    return elementwiseBinary(a[0], a[1],
        [](double x, double y){ return std::max(x, y); }, err, "Max");
}

// Replace(signal, oldValue, newValue [, tolerance])
//   Element-wise substitution. Wherever signal[i] "matches" oldValue, emit
//   newValue; otherwise emit signal[i]. Match is exact by default; pass a
//   positive tolerance to match |signal[i] - oldValue| <= tolerance.
//   newValue can be a scalar or another signal (intersection-resampled).
std::shared_ptr<Signal> impl_Replace(const FunctionArgs& a, QString* err) {
    if (a.size() < 3 || a.size() > 4) {
        if (err) *err = QString("Replace expects 3 or 4 arg(s), got %1")
                            .arg(a.size());
        return nullptr;
    }
    double oldV = 0;
    if (!asScalar(a[1], oldV)) {
        if (err) *err = "Replace: oldValue must be a scalar";
        return nullptr;
    }
    double tol = 0;
    if (a.size() == 4 && !asScalar(a[3], tol)) {
        if (err) *err = "Replace: tolerance must be a scalar";
        return nullptr;
    }
    if (tol < 0) tol = -tol;

    auto match = [oldV, tol](double v) {
        return (tol == 0) ? (v == oldV) : (std::abs(v - oldV) <= tol);
    };
    // newValue can be scalar or signal: piggy-back on elementwiseBinary by
    // pairing signal with newValue and choosing between sides via match().
    return elementwiseBinary(a[0], a[2],
        [match](double v, double newV){ return match(v) ? newV : v; },
        err, "Replace");
}

// ForwardFill(signal [, fillValue [, tolerance]]) — replace samples that
// match the fill marker with the last preceding non-fill value. Match is
// exact by default; pass a positive tolerance to match |v - fillValue| <=
// tolerance. Leading run of matched samples is preserved (no good value
// has been seen yet).
std::shared_ptr<Signal> impl_ForwardFill(const FunctionArgs& a, QString* err) {
    if (a.size() < 1 || a.size() > 3) {
        if (err) *err = QString("ForwardFill expects 1, 2, or 3 arg(s), got %1")
                            .arg(a.size());
        return nullptr;
    }
    double fillValue = 0.0;
    if (a.size() >= 2 && !asScalar(a[1], fillValue)) {
        if (err) *err = "ForwardFill: fillValue must be a scalar";
        return nullptr;
    }
    double tol = 0.0;
    if (a.size() == 3 && !asScalar(a[2], tol)) {
        if (err) *err = "ForwardFill: tolerance must be a scalar";
        return nullptr;
    }
    if (tol < 0) tol = -tol;

    auto signal = a[0];
    auto view = signal->snapshotForRead();
    auto src  = signal->readAsDouble();
    std::vector<TimestampNs> ts(view.timestamps, view.timestamps + view.count);
    std::vector<double> dst(view.count);

    auto matches = [fillValue, tol](double v) {
        return (tol == 0) ? (v == fillValue) : (std::abs(v - fillValue) <= tol);
    };

    bool haveGood = false;
    double lastGood = 0.0;
    for (std::size_t i = 0; i < view.count; ++i) {
        const double v = src[i];
        if (matches(v)) {
            dst[i] = haveGood ? lastGood : v;   // keep fill if no good value yet
        } else {
            dst[i] = v;
            lastGood = v;
            haveGood = true;
        }
    }
    auto out = makeDoubleSignal("ForwardFill", ts, dst,
                                signal->meta().unit,
                                signal->meta().domain);
    return out;
}

// ---- Slice(signal, t_start, t_end) — keep samples with t in [start, end] ----
std::shared_ptr<Signal> impl_Slice(const FunctionArgs& a, QString* err) {
    if (!ensureN(a, 3, err, "Slice")) return nullptr;
    double tStartSec = 0, tEndSec = 0;
    if (!asScalar(a[1], tStartSec) || !asScalar(a[2], tEndSec)) {
        if (err) *err = "Slice: t_start and t_end must be scalars (seconds)";
        return nullptr;
    }
    if (tStartSec > tEndSec) std::swap(tStartSec, tEndSec);
    const TimestampNs tLo = static_cast<TimestampNs>(tStartSec * 1e9);
    const TimestampNs tHi = static_cast<TimestampNs>(tEndSec   * 1e9);
    auto signal = a[0];
    auto view = signal->snapshotForRead();
    auto src  = signal->readAsDouble();
    std::vector<TimestampNs> ts;
    std::vector<double> dst;
    ts.reserve(view.count);
    dst.reserve(view.count);
    for (std::size_t i = 0; i < view.count; ++i) {
        if (view.timestamps[i] < tLo || view.timestamps[i] > tHi) continue;
        ts.push_back(view.timestamps[i]);
        dst.push_back(i < src.size() ? src[i] : 0.0);
    }
    return makeDoubleSignal("Slice", ts, dst, signal->meta().unit,
                            signal->meta().domain);
}

// ---- FFT(signal [, window]) — single-sided amplitude spectrum ----
//
// Windows the samples (Hann by default; flat-top for accurate peak
// amplitude; or none), zero-pads to the next power of 2, runs an in-place
// radix-2 Cooley-Tukey FFT, and returns the gain-corrected single-sided
// amplitude of the first N/2 + 1 bins. The output's "timestamps" encode
// frequency: bin k → k × df nanoseconds (so a 50 Hz peak appears at
// timestamp 50e9 — the plot's X axis numerically matches Hz). Right-click
// the X axis label to rename "t [s]" → "f [Hz]".
namespace {
void fftRadix2(std::vector<std::complex<double>>& x) {
    const std::size_t N = x.size();
    if (N <= 1) return;
    // Bit-reverse permutation
    std::size_t j = 0;
    for (std::size_t i = 1; i < N; ++i) {
        std::size_t bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(x[i], x[j]);
    }
    // Butterflies
    for (std::size_t len = 2; len <= N; len <<= 1) {
        const double ang = -2.0 * M_PI / static_cast<double>(len);
        const std::complex<double> wlen(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < N; i += len) {
            std::complex<double> w(1.0, 0.0);
            const std::size_t half = len / 2;
            for (std::size_t k = 0; k < half; ++k) {
                const auto u = x[i + k];
                const auto v = x[i + k + half] * w;
                x[i + k]        = u + v;
                x[i + k + half] = u - v;
                w *= wlen;
            }
        }
    }
}
}  // namespace

// FFT analysis window. code: 0 = Hann (default), 1 = flat-top, 2 = none.
double windowCoeff(int code, std::size_t i, std::size_t n) {
    if (n <= 1) return 1.0;
    const double x = 2.0 * M_PI * static_cast<double>(i)
                   / static_cast<double>(n - 1);
    switch (code) {
        case 1:  // flat-top — wide flat main lobe → accurate peak amplitude
            return 0.21557895
                 - 0.41663158  * std::cos(x)
                 + 0.277263158 * std::cos(2.0 * x)
                 - 0.083578947 * std::cos(3.0 * x)
                 + 0.006947368 * std::cos(4.0 * x);
        case 2:  // rectangular / none
            return 1.0;
        default: // 0 = Hann
            return 0.5 * (1.0 - std::cos(x));
    }
}

// Parse the optional window argument shared by FFT / PeakHz (arg index
// `idx`). Absent → Hann (0). Returns false + sets *err on a bad code.
bool parseWindowArg(const FunctionArgs& a, std::size_t idx, int& win,
                    QString* err, const char* who) {
    win = 0;
    if (a.size() <= idx) return true;
    double wd = 0;
    if (!asScalar(a[idx], wd) || wd < 0 || wd > 2 ||
        wd != std::floor(wd)) {
        if (err) *err = QString("%1: window must be 0 (Hann), 1 (flat-top) "
                                "or 2 (none)").arg(who);
        return false;
    }
    win = static_cast<int>(wd);
    return true;
}

// Shared FFT front-end: window the samples, zero-pad to a power of two,
// transform, and return the raw |X[k]| for bins 0..N/2. Also reports the
// sample rate, transform length N, and Σ window (coherent gain) so callers
// can amplitude-calibrate. Returns false + sets *err on bad input.
bool fftMagnitude(const std::shared_ptr<Signal>& signal, int win,
                  std::vector<double>& mag, double& fs, std::size_t& N,
                  double& windowSum, QString* err, const char* who) {
    auto view = signal->snapshotForRead();
    if (view.count < 2) {
        if (err) *err = QString("%1: signal must have at least 2 samples.").arg(who);
        return false;
    }
    auto src = signal->readAsDouble();

    // Average dt to define the sample rate. Non-uniform grids get a single
    // representative dt; for badly non-uniform data, Resample first.
    const double durationSec =
        (view.timestamps[view.count - 1] - view.timestamps[0]) * 1e-9;
    const double dt = durationSec / static_cast<double>(view.count - 1);
    if (dt <= 0) {
        if (err) *err = QString("%1: non-monotonic / zero-duration time grid.").arg(who);
        return false;
    }
    fs = 1.0 / dt;

    N = 1;
    while (N < view.count) N <<= 1;
    if (N < 2) N = 2;
    std::vector<std::complex<double>> buf(N, {0.0, 0.0});
    windowSum = 0.0;
    for (std::size_t i = 0; i < view.count; ++i) {
        const double w = windowCoeff(win, i, view.count);
        windowSum += w;
        buf[i] = {src[i] * w, 0.0};
    }
    fftRadix2(buf);

    const std::size_t outN = N / 2 + 1;
    mag.resize(outN);
    for (std::size_t k = 0; k < outN; ++k) mag[k] = std::abs(buf[k]);
    return true;
}

std::shared_ptr<Signal> impl_FFT(const FunctionArgs& a, QString* err) {
    if (a.empty() || a.size() > 2) {
        if (err) *err = QString("FFT expects 1..2 args, got %1").arg(a.size());
        return nullptr;
    }
    int win = 0;
    if (!parseWindowArg(a, 1, win, err, "FFT")) return nullptr;

    std::vector<double> mag;
    double fs = 0.0, windowSum = 0.0;
    std::size_t N = 0;
    if (!fftMagnitude(a[0], win, mag, fs, N, windowSum, err, "FFT")) return nullptr;

    // Calibrate to true single-sided amplitude: a tone of amplitude A reads
    // ~A at its peak, independent of the window. DC and Nyquist are already
    // one-sided, so they skip the ×2.
    const double scaleMid = (windowSum > 0.0) ? 2.0 / windowSum : 0.0;
    const double scaleEnd = (windowSum > 0.0) ? 1.0 / windowSum : 0.0;
    const double df = fs / static_cast<double>(N);
    const std::size_t outN = mag.size();
    std::vector<TimestampNs> outTs(outN);
    std::vector<double> outVs(outN);
    for (std::size_t k = 0; k < outN; ++k) {
        outTs[k] = static_cast<TimestampNs>(k * df * 1e9);
        const double s = (k == 0 || k == N / 2) ? scaleEnd : scaleMid;
        outVs[k] = mag[k] * s;
    }
    return makeDoubleSignal("FFT", outTs, outVs, "magnitude",
                            Signal::Domain::Frequency);
}

// ---- PeakHz(signal [, window]) — interpolated dominant peak frequency ----
//
// Finds the largest magnitude bin (excluding DC) and refines its location
// with 3-point parabolic interpolation on the log-magnitude, so the result
// is accurate to a small fraction of a bin instead of ± half a bin. Returns
// a single Hz value (a 1-sample constant) for read-out or further math. A
// strong DC offset / trend can bias it toward low frequency — subtract the
// mean first (e.g. PeakHz(speed - Mean(speed, 1))) if that bites.
std::shared_ptr<Signal> impl_PeakHz(const FunctionArgs& a, QString* err) {
    if (a.empty() || a.size() > 2) {
        if (err) *err = QString("PeakHz expects 1..2 args, got %1").arg(a.size());
        return nullptr;
    }
    int win = 0;
    if (!parseWindowArg(a, 1, win, err, "PeakHz")) return nullptr;

    std::vector<double> mag;
    double fs = 0.0, windowSum = 0.0;
    std::size_t N = 0;
    if (!fftMagnitude(a[0], win, mag, fs, N, windowSum, err, "PeakHz")) return nullptr;

    const std::size_t outN = mag.size();
    // Largest bin above DC.
    std::size_t kmax = 1;
    double best = -1.0;
    for (std::size_t k = 1; k < outN; ++k) {
        if (mag[k] > best) { best = mag[k]; kmax = k; }
    }
    // Parabolic peak interpolation on log-magnitude (guards spectrum edges).
    double delta = 0.0;
    if (kmax >= 1 && kmax + 1 < outN) {
        const double am = std::log(mag[kmax - 1] + 1e-300);
        const double bm = std::log(mag[kmax]     + 1e-300);
        const double gm = std::log(mag[kmax + 1] + 1e-300);
        const double denom = am - 2.0 * bm + gm;
        if (denom != 0.0) delta = 0.5 * (am - gm) / denom;
        delta = std::clamp(delta, -0.5, 0.5);
    }
    const double df = fs / static_cast<double>(N);
    const double hz = (static_cast<double>(kmax) + delta) * df;

    std::vector<TimestampNs> ts{0};
    std::vector<double>      vals{hz};
    return makeDoubleSignal("PeakHz", ts, vals, "Hz");
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

// Shared by Gate / FFTWelch: find the event windows. Walk `signal`, mark where
// the (interpolated) `gate` is within [low, high], group into contiguous runs,
// and keep only runs lasting >= minLengthSec. Returns [start, end) index pairs
// into the signal. minLengthSec <= 0 keeps every run (even 1-sample blips).
std::vector<std::pair<std::size_t, std::size_t>>
findGateWindows(const Signal::ReadView& sView,
                const Signal::ReadView& gView,
                const std::vector<double>& gVals,
                double low, double high, double minLengthSec) {
    std::vector<std::pair<std::size_t, std::size_t>> runs;
    std::size_t cursor = 0;
    bool in = false;
    std::size_t start = 0;
    for (std::size_t i = 0; i < sView.count; ++i) {
        const double g = interpAtHere(gView.timestamps, gVals, gView.count,
                                      sView.timestamps[i], cursor);
        const bool here = (g >= low && g <= high);
        if (here && !in)       { in = true; start = i; }
        else if (!here && in)  { in = false; runs.emplace_back(start, i); }
    }
    if (in) runs.emplace_back(start, sView.count);

    if (minLengthSec > 0.0) {
        const TimestampNs minNs = static_cast<TimestampNs>(minLengthSec * 1e9);
        std::vector<std::pair<std::size_t, std::size_t>> kept;
        for (const auto& r : runs) {
            const TimestampNs dur =
                sView.timestamps[r.second - 1] - sView.timestamps[r.first];
            if (dur >= minNs) kept.push_back(r);
        }
        runs.swap(kept);
    }
    return runs;
}

// ---- Gate(signal, gate, low, high [, min_length [, mode]]) ----
//
// Keep `signal` only where `gate` is within [low, high]; cut the rest.
//   min_length — drop in-range stretches shorter than this many SECONDS
//                (default 0 = keep all). Removes brief blips / transits.
//   mode 0 (default) — SMASH: kept windows closed up back-to-back (compact).
//   mode 1           — REAL TIME: keep original timestamps, drop the rest.
//
// `gate` may be sampled differently from `signal` (linearly interpolated onto
// it). Bounds inclusive; low/high swapped if backwards. Note: smash still joins
// consecutive windows with a connecting line — use FFTWelch for a seam-free
// spectrum.
std::shared_ptr<Signal> impl_Gate(const FunctionArgs& a, QString* err) {
    if (a.size() < 4 || a.size() > 6) {
        if (err) *err = QString("Gate expects 4..6 args "
            "(signal, gate, low, high [, min_length [, mode]]), got %1").arg(a.size());
        return nullptr;
    }
    auto signal = a[0];
    auto gate   = a[1];
    double low = 0.0, high = 0.0;
    if (!asScalar(a[2], low) || !asScalar(a[3], high)) {
        if (err) *err = "Gate: low and high must be scalars";
        return nullptr;
    }
    if (low > high) std::swap(low, high);
    double minLen = 0.0;
    if (a.size() >= 5 && !asScalar(a[4], minLen)) {
        if (err) *err = "Gate: min_length must be a scalar (seconds)";
        return nullptr;
    }
    if (minLen < 0.0) minLen = 0.0;
    int mode = 0;
    if (a.size() == 6) {
        double md = 0.0;
        if (!asScalar(a[5], md) || (md != 0.0 && md != 1.0)) {
            if (err) *err = "Gate: mode must be 0 (smash) or 1 (real time)";
            return nullptr;
        }
        mode = static_cast<int>(md);
    }

    auto sView = signal->snapshotForRead();
    auto sVals = signal->readAsDouble();
    auto gView = gate->snapshotForRead();
    auto gVals = gate->readAsDouble();
    if (gView.count == 0) {
        if (err) *err = "Gate: gate signal is empty";
        return nullptr;
    }

    const auto runs = findGateWindows(sView, gView, gVals, low, high, minLen);

    std::vector<TimestampNs> outTs;
    std::vector<double>      outVs;
    outTs.reserve(sView.count);
    outVs.reserve(sView.count);

    if (mode == 1) {
        // REAL TIME — keep original timestamps.
        for (const auto& r : runs)
            for (std::size_t i = r.first; i < r.second; ++i) {
                outTs.push_back(sView.timestamps[i]);
                outVs.push_back(sVals[i]);
            }
    } else {
        // SMASH — close the gaps between windows (nominalDt across each join).
        TimestampNs nominalDt = 1;
        if (sView.count >= 2) {
            std::vector<TimestampNs> diffs;
            diffs.reserve(sView.count - 1);
            for (std::size_t i = 1; i < sView.count; ++i)
                diffs.push_back(sView.timestamps[i] - sView.timestamps[i - 1]);
            std::nth_element(diffs.begin(), diffs.begin() + diffs.size() / 2, diffs.end());
            nominalDt = diffs[diffs.size() / 2];
            if (nominalDt <= 0) nominalDt = 1;
        }
        TimestampNs outT = 0;
        bool have = false;
        for (const auto& r : runs) {
            for (std::size_t i = r.first; i < r.second; ++i) {
                if (!have) {
                    outT = sView.timestamps[i];
                    have = true;
                } else {
                    TimestampNs step = (i == r.first)
                        ? nominalDt                                         // join
                        : sView.timestamps[i] - sView.timestamps[i - 1];   // within run
                    if (step <= 0) step = nominalDt;
                    outT += step;
                }
                outTs.push_back(outT);
                outVs.push_back(sVals[i]);
            }
        }
    }

    return makeDoubleSignal("Gate", outTs, outVs,
                            signal->meta().unit, signal->meta().domain);
}

// ---- FFTWelch(signal, gate, low, high [, min_length [, window]]) ----
//
// Welch-style averaged amplitude spectrum of the gated events: find each event
// window (gate in [low, high], lasting >= min_length seconds), FFT each one
// SEPARATELY (windowed, zero-padded to a common length), and average the
// magnitudes. No concatenation → no seam — the clean way to get the frequency
// of an event that recurs across a long capture.
// window: 0 = Hann (default), 1 = flat-top, 2 = none.
std::shared_ptr<Signal> impl_FFTWelch(const FunctionArgs& a, QString* err) {
    if (a.size() < 4 || a.size() > 6) {
        if (err) *err = QString("FFTWelch expects 4..6 args "
            "(signal, gate, low, high [, min_length [, window]]), got %1").arg(a.size());
        return nullptr;
    }
    auto signal = a[0];
    auto gate   = a[1];
    double low = 0.0, high = 0.0;
    if (!asScalar(a[2], low) || !asScalar(a[3], high)) {
        if (err) *err = "FFTWelch: low and high must be scalars";
        return nullptr;
    }
    if (low > high) std::swap(low, high);
    double minLen = 0.0;
    if (a.size() >= 5 && !asScalar(a[4], minLen)) {
        if (err) *err = "FFTWelch: min_length must be a scalar (seconds)";
        return nullptr;
    }
    if (minLen < 0.0) minLen = 0.0;
    int win = 0;
    if (!parseWindowArg(a, 5, win, err, "FFTWelch")) return nullptr;

    auto sView = signal->snapshotForRead();
    auto sVals = signal->readAsDouble();
    auto gView = gate->snapshotForRead();
    auto gVals = gate->readAsDouble();
    if (gView.count == 0) {
        if (err) *err = "FFTWelch: gate signal is empty";
        return nullptr;
    }

    const auto runs = findGateWindows(sView, gView, gVals, low, high, minLen);

    // Sample rate from a window's internal spacing (the signal's native rate),
    // and the common transform length from the longest window.
    double dtNs = 0.0;
    std::size_t maxLen = 0;
    for (const auto& r : runs) {
        const std::size_t L = r.second - r.first;
        maxLen = std::max(maxLen, L);
        if (dtNs <= 0.0 && L >= 2)
            dtNs = static_cast<double>(sView.timestamps[r.first + 1]
                                       - sView.timestamps[r.first]);
    }
    if (maxLen < 2 || dtNs <= 0.0) {
        if (err) *err = "FFTWelch: no event window long enough to transform "
                        "(check low/high and min_length).";
        return nullptr;
    }
    const double fs = 1e9 / dtNs;

    std::size_t N = 1;
    while (N < maxLen) N <<= 1;
    if (N < 2) N = 2;
    const std::size_t outN = N / 2 + 1;

    std::vector<double> avg(outN, 0.0);
    int used = 0;
    for (const auto& r : runs) {
        const std::size_t L = r.second - r.first;
        if (L < 2) continue;
        std::vector<std::complex<double>> buf(N, {0.0, 0.0});
        double windowSum = 0.0;
        for (std::size_t k = 0; k < L; ++k) {
            const double w = windowCoeff(win, k, L);
            windowSum += w;
            buf[k] = {sVals[r.first + k] * w, 0.0};
        }
        fftRadix2(buf);
        const double scaleMid = (windowSum > 0.0) ? 2.0 / windowSum : 0.0;
        const double scaleEnd = (windowSum > 0.0) ? 1.0 / windowSum : 0.0;
        for (std::size_t k = 0; k < outN; ++k) {
            const double s = (k == 0 || k == N / 2) ? scaleEnd : scaleMid;
            avg[k] += std::abs(buf[k]) * s;
        }
        ++used;
    }
    if (used == 0) {
        if (err) *err = "FFTWelch: no usable event windows.";
        return nullptr;
    }

    const double df = fs / static_cast<double>(N);
    std::vector<TimestampNs> outTs(outN);
    std::vector<double>      outVs(outN);
    for (std::size_t k = 0; k < outN; ++k) {
        outTs[k] = static_cast<TimestampNs>(k * df * 1e9);
        outVs[k] = avg[k] / static_cast<double>(used);
    }
    return makeDoubleSignal("FFTWelch", outTs, outVs, "magnitude",
                            Signal::Domain::Frequency);
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
        "1st-order low-pass IIR with time constant tau (causal — has phase "
        "lag; wrap in filtfilt() for zero phase).", 2, 2, &impl_Filter);
    add("FilterHP",   "FilterHP(signal, tau_seconds)",
        "1st-order high-pass: removes slow drift / DC, passes fast changes "
        "(complement of Filter; same cutoff). Causal — wrap in filtfilt() "
        "for zero phase.", 2, 2, &impl_FilterHP);
    add("Butterworth","Butterworth(signal, cutoff_hz, order)",
        "Digital Butterworth low-pass (uniform-rate, bilinear). order is "
        "optional (default 2). Causal — wrap in filtfilt() for zero phase.",
        2, 3, &impl_Butterworth);
    add("ButterworthHP","ButterworthHP(signal, cutoff_hz, order)",
        "Digital Butterworth high-pass (uniform-rate, bilinear). order is "
        "optional (default 2). Causal — wrap in filtfilt() for zero phase.",
        2, 3, &impl_ButterworthHP);
    add("Reverse",    "Reverse(signal)",
        "Flip the signal in time (values reversed, timestamps preserved). "
        "Reversing twice returns the original.", 1, 1, &impl_Reverse);
    // filtfilt is a higher-order *special form* handled directly in the
    // parser (its first argument is a filter NAME, not data). This registry
    // entry exists only so it shows up in autocomplete / help; the impl here
    // is never reached because the parser intercepts the call first.
    add("filtfilt",   "filtfilt(FilterName, signal, params...)",
        "Zero-phase filtering: runs the named filter forward then backward so "
        "there is no phase lag. e.g. filtfilt(Filter, speed, 0.05) or "
        "filtfilt(Butterworth, speed, 10, 4).",
        2, -1, [](const FunctionArgs&, QString* e) {
            if (e) *e = "filtfilt must name a filter as its first argument, "
                        "e.g. filtfilt(Filter, speed, 0.05)";
            return std::shared_ptr<Signal>(nullptr);
        });
    add("Integral",   "Integral(signal)",
        "Trapezoidal cumulative integration.", 1, 1, &impl_Integral);
    add("Derivative", "Derivative(signal)",
        "Δy/Δx via central difference (forward at the first sample, "
        "backward at the last). For staircase-quantised data, collapse "
        "value plateaus at import in the Converter. For noisy data, "
        "compose: Derivative(Filter(s, tau)).",
        1, 1, &impl_Derivative);
    add("Mean",       "Mean(signal, window_seconds)",
        "Rolling arithmetic mean over the last window_seconds.", 2, 2, &impl_Mean);
    add("RMS",        "RMS(signal, window_seconds)",
        "Rolling root-mean-square.", 2, 2, &impl_RMS);
    add("Min",        "Min(a, b)",
        "Element-wise pair minimum. Both args can be signals or scalars.",
        2, 2, &impl_MinPair);
    add("Max",        "Max(a, b)",
        "Element-wise pair maximum. Both args can be signals or scalars.",
        2, 2, &impl_MaxPair);
    add("RollingMin", "RollingMin(signal, window_seconds)",
        "Minimum over a sliding time window.", 2, 2, &impl_RollingMin);
    add("RollingMax", "RollingMax(signal, window_seconds)",
        "Maximum over a sliding time window.", 2, 2, &impl_RollingMax);
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
    add("Tan",        "Tan(signal)",
        "Element-wise tangent.", 1, 1, &impl_Tan);
    add("Asin",       "Asin(signal)",
        "Element-wise arcsine (radians).", 1, 1, &impl_Asin);
    add("Acos",       "Acos(signal)",
        "Element-wise arccosine (radians).", 1, 1, &impl_Acos);
    add("Atan",       "Atan(signal)",
        "Element-wise arctangent (radians).", 1, 1, &impl_Atan);
    add("Atan2",      "Atan2(y, x)",
        "Element-wise two-argument arctangent (radians, preserves quadrant). "
        "Both args can be signals or scalars.",
        2, 2, &impl_Atan2);
    add("Exp",        "Exp(signal)",
        "Element-wise e^signal.", 1, 1, &impl_Exp);
    add("Log10",      "Log10(signal)",
        "Element-wise base-10 logarithm.", 1, 1, &impl_Log10);
    add("Floor",      "Floor(signal)",
        "Element-wise round towards −∞.", 1, 1, &impl_Floor);
    add("Ceil",       "Ceil(signal)",
        "Element-wise round towards +∞.", 1, 1, &impl_Ceil);
    add("Round",      "Round(signal)",
        "Element-wise round to nearest integer.", 1, 1, &impl_Round);
    add("Sign",       "Sign(signal)",
        "Element-wise sign: −1, 0, or +1.", 1, 1, &impl_Sign);
    add("Power",      "Power(base, exponent)",
        "Element-wise power. Both args can be signals or scalars.",
        2, 2, &impl_Power);
    add("Mod",        "Mod(signal, divisor)",
        "Element-wise floating-point modulo (sign of result follows dividend, "
        "as in C/C++ fmod). Both args can be signals or scalars.",
        2, 2, &impl_Mod);
    add("Limit",      "Limit(signal, lo, hi)",
        "Element-wise clamp into the [lo, hi] interval. lo/hi must be scalars.",
        3, 3, &impl_Limit);
    add("Replace",    "Replace(signal, oldValue, newValue [, tolerance])",
        "Element-wise substitution. Where signal matches oldValue (exact by "
        "default, or within ±tolerance), emit newValue (scalar or another "
        "signal). Useful for swapping sentinels or filling specific values.",
        3, 4, &impl_Replace);
    add("ForwardFill", "ForwardFill(signal [, fillValue [, tolerance]])",
        "Replace samples matching fillValue (default 0) with the last "
        "preceding non-matching value. Match is exact by default, or "
        "within ±tolerance if given. Leading run of matched samples is "
        "kept. Useful for sensor dropouts that report a sentinel value.",
        1, 3, &impl_ForwardFill);
    add("Slice",      "Slice(signal, t_start, t_end)",
        "Keep only samples whose timestamp falls in [t_start, t_end] seconds. "
        "Inclusive bounds.",
        3, 3, &impl_Slice);
    add("Gate",       "Gate(signal, gate, low, high [, min_length [, mode]])",
        "Keep `signal` only where `gate` is in [low, high]; cut the rest. "
        "min_length = drop in-range stretches shorter than this many SECONDS "
        "(default 0) — removes brief blips/transits. mode 0 = smash (default): "
        "close the gaps so windows sit back-to-back; mode 1 = real time: keep "
        "the original timestamps. `gate` may be on a different sample rate "
        "(interpolated onto `signal`); bounds inclusive.",
        4, 6, &impl_Gate);
    add("FFTWelch",   "FFTWelch(signal, gate, low, high [, min_length [, window]])",
        "Welch-averaged amplitude spectrum of the gated events: FFT each event "
        "window (gate in [low,high], lasting >= min_length s) separately and "
        "average them — no concatenation, so no seam. The clean way to get the "
        "frequency of an event that recurs across a long capture. window: "
        "0 = Hann (default), 1 = flat-top, 2 = none.",
        4, 6, &impl_FFTWelch);
    add("FFT",        "FFT(signal [, window])",
        "Single-sided amplitude spectrum via a windowed, zero-padded radix-2 "
        "FFT. window: 0 = Hann (default), 1 = flat-top (accurate peak "
        "amplitude), 2 = none. Magnitudes are gain-corrected so a tone reads "
        "its true amplitude. Output X axis is frequency in Hz (numerically; "
        "the chart label still shows 't [s]' — right-click to rename).",
        1, 2, &impl_FFT);
    add("PeakHz",     "PeakHz(signal [, window])",
        "Dominant peak frequency in Hz, refined by parabolic interpolation "
        "between FFT bins — far more accurate than the raw bin spacing fs/N. "
        "window: 0 = Hann (default), 1 = flat-top, 2 = none. Returns a single "
        "value you can read in the table or use in further formulas.",
        1, 2, &impl_PeakHz);
    add("Resample",   "Resample(signal, rate_Hz_or_reference_signal)",
        "Linear-interpolate signal onto a new time grid. Pass a positive scalar "
        "for the target rate in Hz, or another signal to copy its timestamps.",
        2, 2, &impl_Resample);
}

}  // namespace scope::analyser
