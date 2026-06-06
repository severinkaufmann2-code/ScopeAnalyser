#include "scope/analyser/FunctionRegistry.h"
#include "scope/core/Signal.h"

#include <QString>

#include <algorithm>
#include <cmath>
#include <complex>
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

// ---- FFT(signal) — magnitude spectrum ----
//
// Resamples to a uniform grid using the intersection-resample logic
// (so non-uniform CSV imports work), applies a Hann window, zero-pads
// to the next power of 2, runs an in-place radix-2 Cooley-Tukey FFT,
// and returns the magnitude of the first N/2 + 1 bins. The output's
// "timestamps" encode frequency: bin k → k × df nanoseconds (so a
// 50 Hz peak appears at timestamp 50e9 — the plot's X axis numerically
// matches Hz). Right-click the X axis label to rename "t [s]" → "f [Hz]".
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

std::shared_ptr<Signal> impl_FFT(const FunctionArgs& a, QString* err) {
    if (!ensureN(a, 1, err, "FFT")) return nullptr;
    auto signal = a[0];
    auto view = signal->snapshotForRead();
    if (view.count < 2) {
        if (err) *err = "FFT: signal must have at least 2 samples.";
        return nullptr;
    }
    auto src = signal->readAsDouble();

    // Average dt to define the sample rate. Non-uniform grids get a
    // single representative dt; for badly non-uniform data the user
    // should resample first.
    const double durationSec =
        (view.timestamps[view.count - 1] - view.timestamps[0]) * 1e-9;
    const double dt = durationSec / static_cast<double>(view.count - 1);
    if (dt <= 0) {
        if (err) *err = "FFT: non-monotonic / zero-duration time grid.";
        return nullptr;
    }
    const double fs = 1.0 / dt;

    // Pad to next power of 2.
    std::size_t N = 1;
    while (N < view.count) N <<= 1;
    if (N < 2) N = 2;
    std::vector<std::complex<double>> buf(N, {0.0, 0.0});
    // Hann window applied across the actual samples; padding stays zero.
    for (std::size_t i = 0; i < view.count; ++i) {
        const double w = 0.5 * (1.0 - std::cos(2.0 * M_PI * i
                                  / static_cast<double>(view.count - 1)));
        buf[i] = {src[i] * w, 0.0};
    }
    fftRadix2(buf);

    // Magnitude spectrum, first N/2 + 1 bins.
    const double df = fs / static_cast<double>(N);
    const std::size_t outN = N / 2 + 1;
    std::vector<TimestampNs> outTs(outN);
    std::vector<double> outVs(outN);
    for (std::size_t k = 0; k < outN; ++k) {
        outTs[k] = static_cast<TimestampNs>(k * df * 1e9);
        outVs[k] = std::abs(buf[k]) / static_cast<double>(view.count);
    }
    return makeDoubleSignal("FFT", outTs, outVs, "magnitude",
                            Signal::Domain::Frequency);
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
    add("FFT",        "FFT(signal)",
        "Magnitude spectrum via Hann-windowed, zero-padded radix-2 FFT. "
        "Output X axis is frequency in Hz (numerically; the chart label still "
        "shows 't [s]' — right-click to rename).",
        1, 1, &impl_FFT);
    add("Resample",   "Resample(signal, rate_Hz_or_reference_signal)",
        "Linear-interpolate signal onto a new time grid. Pass a positive scalar "
        "for the target rate in Hz, or another signal to copy its timestamps.",
        2, 2, &impl_Resample);
}

}  // namespace scope::analyser
