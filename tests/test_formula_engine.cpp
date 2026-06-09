#include "scope/analyser/FormulaEngine.h"
#include "scope/analyser/FunctionRegistry.h"
#include "scope/core/Signal.h"
#include "scope/core/SignalStore.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

using namespace scope::core;
using namespace scope::analyser;

namespace {
std::shared_ptr<Signal> makeRamp(QString name, std::size_t n, double dtSec, double startVal = 0.0) {
    Signal::Meta m;
    m.name = std::move(name);
    m.dataType = DataType::Float64;
    m.sampleRateHz = 1.0 / dtSec;
    auto sig = std::make_shared<Signal>(m);
    std::vector<TimestampNs> ts(n);
    std::vector<double> vs(n);
    for (std::size_t i = 0; i < n; ++i) {
        ts[i] = static_cast<TimestampNs>(i * dtSec * 1e9);
        vs[i] = startVal + static_cast<double>(i);
    }
    sig->append(ts.data(), reinterpret_cast<const std::byte*>(vs.data()), n);
    return sig;
}

// A sine of amplitude `amp` at `freq` Hz, sampled at `fs` Hz. fs is chosen in
// tests to divide 1e9 so timestamps are exact integer nanoseconds.
std::shared_ptr<Signal> makeSine(QString name, std::size_t n, double fs,
                                 double freq, double amp) {
    Signal::Meta m;
    m.name = std::move(name);
    m.dataType = DataType::Float64;
    m.sampleRateHz = fs;
    auto sig = std::make_shared<Signal>(m);
    std::vector<TimestampNs> ts(n);
    std::vector<double> vs(n);
    const double dt = 1.0 / fs;
    for (std::size_t i = 0; i < n; ++i) {
        ts[i] = static_cast<TimestampNs>(std::llround(i * dt * 1e9));
        vs[i] = amp * std::sin(2.0 * M_PI * freq * i * dt);
    }
    sig->append(ts.data(), reinterpret_cast<const std::byte*>(vs.data()), n);
    return sig;
}

double peakValue(const std::shared_ptr<Signal>& s) {
    auto v = s->readAsDouble();
    double m = 0.0;
    for (double x : v) m = std::max(m, x);
    return m;
}

// Arbitrary value series at `fs` Hz (fs divides 1e9 in tests → exact ts).
std::shared_ptr<Signal> makeSeries(QString name, const std::vector<double>& vals,
                                   double fs) {
    Signal::Meta m;
    m.name = std::move(name);
    m.dataType = DataType::Float64;
    m.sampleRateHz = fs;
    auto sig = std::make_shared<Signal>(m);
    std::vector<TimestampNs> ts(vals.size());
    const double dt = 1.0 / fs;
    for (std::size_t i = 0; i < vals.size(); ++i)
        ts[i] = static_cast<TimestampNs>(std::llround(i * dt * 1e9));
    sig->append(ts.data(), reinterpret_cast<const std::byte*>(vals.data()),
                vals.size());
    return sig;
}
}

TEST(FormulaEngine, AliasChannel) {
    SignalStore store;
    store.add(makeRamp("A", 100, 0.01));
    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("B = A", &err)) << err.toStdString();
    auto b = store.get("B");
    ASSERT_TRUE(b);
    EXPECT_EQ(b->sampleCount(), 100u);
}

TEST(FormulaEngine, ScalarTimesChannelPlusChannel) {
    SignalStore store;
    store.add(makeRamp("A", 10, 0.01));        // 0..9
    store.add(makeRamp("B", 10, 0.01, 100));   // 100..109
    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("C = 2 * A + B", &err)) << err.toStdString();
    auto c = store.get("C");
    ASSERT_TRUE(c);
    auto vs = c->readAsDouble();
    ASSERT_EQ(vs.size(), 10u);
    for (std::size_t i = 0; i < vs.size(); ++i) {
        EXPECT_DOUBLE_EQ(vs[i], 2.0 * static_cast<double>(i) + 100.0 + static_cast<double>(i));
    }
}

TEST(FormulaEngine, FilterFunctionRunsAndLowPasses) {
    SignalStore store;
    // Step input
    Signal::Meta m;
    m.name = "step";
    m.dataType = DataType::Float64;
    auto step = std::make_shared<Signal>(m);
    constexpr std::size_t N = 200;
    std::vector<TimestampNs> ts(N);
    std::vector<double> vs(N);
    for (std::size_t i = 0; i < N; ++i) {
        ts[i] = static_cast<TimestampNs>(i * 1'000'000);  // 1 ms
        vs[i] = (i < 50) ? 0.0 : 1.0;
    }
    step->append(ts.data(), reinterpret_cast<const std::byte*>(vs.data()), N);
    store.add(step);

    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("out = Filter(step, 0.05)", &err)) << err.toStdString();
    auto out = store.get("out");
    ASSERT_TRUE(out);
    auto ovs = out->readAsDouble();
    ASSERT_EQ(ovs.size(), N);
    // Just-after-step value should be small (filter rising), final value close to 1.
    EXPECT_LT(ovs[55], 0.5);
    EXPECT_GT(ovs.back(), 0.9);
}

TEST(FormulaEngine, IntegralOfConstantIsLinear) {
    SignalStore store;
    Signal::Meta m;
    m.name = "k"; m.dataType = DataType::Float64;
    auto k = std::make_shared<Signal>(m);
    constexpr std::size_t N = 11;
    std::vector<TimestampNs> ts(N);
    std::vector<double> vs(N, 2.0);
    for (std::size_t i = 0; i < N; ++i) ts[i] = static_cast<TimestampNs>(i * 1'000'000'000ll);
    k->append(ts.data(), reinterpret_cast<const std::byte*>(vs.data()), N);
    store.add(k);

    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("S = Integral(k)", &err)) << err.toStdString();
    auto out = store.get("S");
    ASSERT_TRUE(out);
    auto ovs = out->readAsDouble();
    // Integral of f(t)=2 from 0..N*1s = 2*N at the last sample.
    EXPECT_NEAR(ovs.back(), 2.0 * 10.0, 1e-9);
}

TEST(FormulaEngine, DerivativeOfLinearIsConstant) {
    SignalStore store;
    store.add(makeRamp("L", 21, 0.1));  // value=i, time=i*0.1 → slope = 10
    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("D = Derivative(L)", &err)) << err.toStdString();
    auto out = store.get("D");
    auto ovs = out->readAsDouble();
    // Central difference on a true linear ramp: every sample reports
    // the exact slope. Endpoints use forward/backward, which also
    // give the exact slope here because the ramp is uniform.
    for (std::size_t i = 0; i < ovs.size(); ++i) {
        EXPECT_NEAR(ovs[i], 10.0, 1e-9) << "at i=" << i;
    }
}

TEST(FormulaEngine, UnknownChannelGivesError) {
    SignalStore store;
    FormulaEngine engine(store);
    QString err;
    EXPECT_FALSE(engine.evaluate("Out = NotThere", &err));
    EXPECT_TRUE(err.contains("NotThere"));
}

TEST(FormulaEngine, NestedFunctionAndArithmetic) {
    SignalStore store;
    store.add(makeRamp("A", 100, 0.01, 1.0));  // 1..100 over 1s
    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("B = Filter(A, 0.02) + 5", &err)) << err.toStdString();
    auto out = store.get("B");
    ASSERT_TRUE(out);
    EXPECT_EQ(out->sampleCount(), 100u);
}

// Helper: makes a constant-value signal of length n at rate hz.
namespace {
std::shared_ptr<Signal> makeConstAtRate(QString name, std::size_t n, double hz, double value) {
    Signal::Meta m;
    m.name = std::move(name);
    m.dataType = DataType::Float64;
    m.sampleRateHz = hz;
    auto sig = std::make_shared<Signal>(m);
    std::vector<TimestampNs> ts(n);
    std::vector<double> vs(n, value);
    const auto dt = static_cast<TimestampNs>(1e9 / hz);
    for (std::size_t i = 0; i < n; ++i) ts[i] = static_cast<TimestampNs>(i) * dt;
    sig->append(ts.data(), reinterpret_cast<const std::byte*>(vs.data()), n);
    return sig;
}
}

TEST(FormulaEngine, AddChannelsAtDifferentRates) {
    SignalStore store;
    // Both 1 second long; A at 1 kHz (1000 samples), B at 100 Hz (100 samples).
    // Both constant: A=10, B=1. Result should be ~11 everywhere.
    store.add(makeConstAtRate("Hi",  1000, 1000.0, 10.0));
    store.add(makeConstAtRate("Lo",   100,  100.0,  1.0));
    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("Sum = Hi + Lo", &err)) << err.toStdString();
    auto sum = store.get("Sum");
    ASSERT_TRUE(sum);
    auto vs = sum->readAsDouble();
    ASSERT_GT(vs.size(), 100u);   // resampled to Hi's grid → ~1000 samples
    for (double v : vs) EXPECT_NEAR(v, 11.0, 1e-9);
}

TEST(FormulaEngine, ResampleExplicitToHz) {
    SignalStore store;
    store.add(makeConstAtRate("Hi", 1000, 1000.0, 7.0));   // 1 kHz, 1 s
    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("Lo = Resample(Hi, 100)", &err)) << err.toStdString();
    auto out = store.get("Lo");
    ASSERT_TRUE(out);
    // 1 s span at 100 Hz → ~100 samples (101 including both endpoints).
    EXPECT_GT(out->sampleCount(),  90u);
    EXPECT_LT(out->sampleCount(), 120u);
    auto vs = out->readAsDouble();
    for (double v : vs) EXPECT_NEAR(v, 7.0, 1e-9);
}

TEST(FormulaEngine, ResampleToReferenceSignal) {
    SignalStore store;
    store.add(makeConstAtRate("Hi",  1000, 1000.0, 3.0));
    store.add(makeConstAtRate("Ref",   50,   50.0, 0.0));
    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("Down = Resample(Hi, Ref)", &err)) << err.toStdString();
    auto out = store.get("Down");
    ASSERT_TRUE(out);
    EXPECT_EQ(out->sampleCount(), 50u);  // same timestamps as Ref (all in range)
    auto vs = out->readAsDouble();
    for (double v : vs) EXPECT_NEAR(v, 3.0, 1e-9);
}

TEST(FormulaEngine, NoOverlapRangeError) {
    SignalStore store;
    Signal::Meta ma; ma.name = "A"; ma.dataType = DataType::Float64;
    auto a = std::make_shared<Signal>(ma);
    TimestampNs aTs[3] = {0, 1'000'000, 2'000'000};
    double      aVs[3] = {1, 2, 3};
    a->append(aTs, reinterpret_cast<const std::byte*>(aVs), 3);
    store.add(a);

    Signal::Meta mb; mb.name = "B"; mb.dataType = DataType::Float64;
    auto b = std::make_shared<Signal>(mb);
    TimestampNs bTs[3] = {10'000'000, 11'000'000, 12'000'000};
    double      bVs[3] = {4, 5, 6};
    b->append(bTs, reinterpret_cast<const std::byte*>(bVs), 3);
    store.add(b);

    FormulaEngine engine(store);
    QString err;
    EXPECT_FALSE(engine.evaluate("Bad = A + B", &err));
    EXPECT_TRUE(err.contains("overlap")) << err.toStdString();
}

TEST(FunctionRegistryBuiltins, AllRegistered) {
    auto& reg = FunctionRegistry::instance();
    EXPECT_NE(reg.find("Filter"),     nullptr);
    EXPECT_NE(reg.find("Integral"),   nullptr);
    EXPECT_NE(reg.find("Derivative"), nullptr);
    EXPECT_NE(reg.find("Mean"),       nullptr);
    EXPECT_NE(reg.find("RMS"),        nullptr);
    EXPECT_NE(reg.find("Min"),        nullptr);
    EXPECT_NE(reg.find("Max"),        nullptr);
    EXPECT_NE(reg.find("Shift"),      nullptr);
    EXPECT_NE(reg.find("Abs"),        nullptr);
    EXPECT_NE(reg.find("Sqrt"),       nullptr);
    EXPECT_NE(reg.find("Log"),        nullptr);
    EXPECT_NE(reg.find("Sin"),        nullptr);
    EXPECT_NE(reg.find("Cos"),        nullptr);
    EXPECT_NE(reg.find("Resample"),   nullptr);
}

// -------- Round-tripping the new element-wise math --------

TEST(FormulaEngine, PowerScalarExponent) {
    SignalStore store;
    store.add(makeRamp("A", 5, 1.0));  // 0,1,2,3,4
    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("P = Power(A, 2)", &err)) << err.toStdString();
    auto vs = store.get("P")->readAsDouble();
    ASSERT_EQ(vs.size(), 5u);
    for (std::size_t i = 0; i < 5; ++i) {
        EXPECT_DOUBLE_EQ(vs[i], static_cast<double>(i * i));
    }
}

TEST(FormulaEngine, ModBy3) {
    SignalStore store;
    store.add(makeRamp("A", 7, 1.0));  // 0..6
    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("M = Mod(A, 3)", &err)) << err.toStdString();
    auto vs = store.get("M")->readAsDouble();
    const std::vector<double> expected = {0, 1, 2, 0, 1, 2, 0};
    ASSERT_EQ(vs.size(), expected.size());
    for (std::size_t i = 0; i < vs.size(); ++i)
        EXPECT_DOUBLE_EQ(vs[i], expected[i]);
}

TEST(FormulaEngine, FloorCeilRoundSign) {
    SignalStore store;
    // values 0, 0.4, 1.0, 1.5, 2.9, -0.5, -1.5
    Signal::Meta m; m.name = "V"; m.dataType = DataType::Float64; m.sampleRateHz = 1.0;
    auto sig = std::make_shared<Signal>(m);
    const std::vector<double> vs = {0.0, 0.4, 1.0, 1.5, 2.9, -0.5, -1.5};
    std::vector<TimestampNs> ts(vs.size());
    for (std::size_t i = 0; i < vs.size(); ++i) ts[i] = i * 1'000'000'000LL;
    sig->append(ts.data(), reinterpret_cast<const std::byte*>(vs.data()), vs.size());
    store.add(sig);

    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("F = Floor(V)", &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("C = Ceil(V)",  &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("R = Round(V)", &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("S = Sign(V)",  &err)) << err.toStdString();
    auto fv = store.get("F")->readAsDouble();
    auto cv = store.get("C")->readAsDouble();
    auto rv = store.get("R")->readAsDouble();
    auto sv = store.get("S")->readAsDouble();
    EXPECT_DOUBLE_EQ(fv[1],  0.0);   // floor(0.4)
    EXPECT_DOUBLE_EQ(cv[1],  1.0);   // ceil(0.4)
    EXPECT_DOUBLE_EQ(rv[3],  2.0);   // round(1.5) → 2 (banker's may differ, std::round → 2)
    EXPECT_DOUBLE_EQ(sv[0],  0.0);
    EXPECT_DOUBLE_EQ(sv[1],  1.0);
    EXPECT_DOUBLE_EQ(sv[5], -1.0);
}

TEST(FormulaEngine, ExpLog10Inverses) {
    SignalStore store;
    store.add(makeRamp("A", 5, 1.0, 1.0));  // 1..5 (avoid log10(0))
    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("E = Exp(A)", &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("L = Log10(A)", &err)) << err.toStdString();
    auto ev = store.get("E")->readAsDouble();
    auto lv = store.get("L")->readAsDouble();
    EXPECT_NEAR(ev[0], std::exp(1.0),  1e-12);
    EXPECT_NEAR(lv[4], std::log10(5.0), 1e-12);
}

TEST(FormulaEngine, LimitClampsInRange) {
    SignalStore store;
    store.add(makeRamp("A", 11, 1.0));  // 0..10
    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("Lm = Limit(A, 2, 7)", &err)) << err.toStdString();
    auto vs = store.get("Lm")->readAsDouble();
    EXPECT_DOUBLE_EQ(vs[0],  2.0);
    EXPECT_DOUBLE_EQ(vs[5],  5.0);
    EXPECT_DOUBLE_EQ(vs[10], 7.0);
}

TEST(FormulaEngine, MinMaxPairScalarAndSignal) {
    SignalStore store;
    store.add(makeRamp("A", 11, 1.0));  // 0..10
    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("Lo = Min(A, 5)", &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("Hi = Max(A, 5)", &err)) << err.toStdString();
    auto lo = store.get("Lo")->readAsDouble();
    auto hi = store.get("Hi")->readAsDouble();
    for (int i = 0; i < 11; ++i) {
        EXPECT_DOUBLE_EQ(lo[i], std::min(static_cast<double>(i), 5.0));
        EXPECT_DOUBLE_EQ(hi[i], std::max(static_cast<double>(i), 5.0));
    }
}

TEST(FormulaEngine, MinMaxPairSignalSignal) {
    SignalStore store;
    store.add(makeRamp("A", 11, 1.0));         // 0..10
    store.add(makeRamp("B", 11, 1.0, 5));      // 5..15
    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("Lo = Min(A, B)", &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("Hi = Max(A, B)", &err)) << err.toStdString();
    auto lo = store.get("Lo")->readAsDouble();
    auto hi = store.get("Hi")->readAsDouble();
    for (int i = 0; i < 11; ++i) {
        EXPECT_DOUBLE_EQ(lo[i], static_cast<double>(i));      // A always <= B
        EXPECT_DOUBLE_EQ(hi[i], static_cast<double>(i + 5));  // B always >= A
    }
}

TEST(FormulaEngine, RollingMinMaxStillWork) {
    SignalStore store;
    Signal::Meta m; m.name = "S"; m.dataType = DataType::Float64; m.sampleRateHz = 10.0;
    auto sig = std::make_shared<Signal>(m);
    const std::size_t N = 21;
    std::vector<TimestampNs> ts(N);
    std::vector<double> vs(N);
    // 1.0 at i==0, 0 elsewhere
    for (std::size_t i = 0; i < N; ++i) {
        ts[i] = static_cast<TimestampNs>(i * 0.1 * 1e9);
        vs[i] = (i == 10) ? 1.0 : 0.0;
    }
    sig->append(ts.data(), reinterpret_cast<const std::byte*>(vs.data()), N);
    store.add(sig);

    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("Mx = RollingMax(S, 0.5)", &err)) << err.toStdString();
    auto mx = store.get("Mx")->readAsDouble();
    // The spike sits inside any window that overlaps t = 1.0 s; at samples
    // i = 10..14 the trailing-0.5 s window still includes it (just barely).
    EXPECT_DOUBLE_EQ(mx[10], 1.0);
    EXPECT_DOUBLE_EQ(mx[14], 1.0);
    // Far enough away → 0
    EXPECT_DOUBLE_EQ(mx[20], 0.0);
}

TEST(FormulaEngine, Atan2YOverX) {
    SignalStore store;
    // y = [0, 1, 1, 0]    x = [1, 1, 0, -1]   →  atan2 = 0, π/4, π/2, π
    Signal::Meta my; my.name = "Y"; my.dataType = DataType::Float64; my.sampleRateHz = 1.0;
    Signal::Meta mx; mx.name = "X"; mx.dataType = DataType::Float64; mx.sampleRateHz = 1.0;
    auto yS = std::make_shared<Signal>(my);
    auto xS = std::make_shared<Signal>(mx);
    const std::vector<double> yv = {0, 1, 1, 0};
    const std::vector<double> xv = {1, 1, 0, -1};
    std::vector<TimestampNs> ts(yv.size());
    for (std::size_t i = 0; i < ts.size(); ++i) ts[i] = i * 1'000'000'000LL;
    yS->append(ts.data(), reinterpret_cast<const std::byte*>(yv.data()), yv.size());
    xS->append(ts.data(), reinterpret_cast<const std::byte*>(xv.data()), xv.size());
    store.add(yS); store.add(xS);

    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("Th = Atan2(Y, X)", &err)) << err.toStdString();
    auto th = store.get("Th")->readAsDouble();
    EXPECT_NEAR(th[0], 0.0,                 1e-12);
    EXPECT_NEAR(th[1], M_PI / 4.0,          1e-12);
    EXPECT_NEAR(th[2], M_PI / 2.0,          1e-12);
    EXPECT_NEAR(th[3], M_PI,                1e-12);
}

// -------- Slice --------

TEST(FormulaEngine, SliceKeepsOnlyInWindow) {
    SignalStore store;
    store.add(makeRamp("A", 11, 0.1));  // t = 0, 0.1, ..., 1.0
    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("S = Slice(A, 0.3, 0.6)", &err)) << err.toStdString();
    auto view = store.get("S")->snapshotForRead();
    auto vs   = store.get("S")->readAsDouble();
    // Expected: timestamps 0.3, 0.4, 0.5, 0.6 → 4 samples
    ASSERT_EQ(view.count, 4u);
    EXPECT_NEAR(view.timestamps[0] / 1e9, 0.3, 1e-9);
    EXPECT_NEAR(view.timestamps[3] / 1e9, 0.6, 1e-9);
    EXPECT_DOUBLE_EQ(vs[0], 3.0);
    EXPECT_DOUBLE_EQ(vs[3], 6.0);
}

// -------- FFT --------

TEST(FormulaEngine, FFTPeakAtKnownFrequency) {
    SignalStore store;
    // 1024 samples at 1024 Hz of sin(2π * 50 * t). Peak should be at 50 Hz.
    const std::size_t N = 1024;
    const double fs = 1024.0;
    const double f0 = 50.0;
    Signal::Meta m; m.name = "S"; m.dataType = DataType::Float64; m.sampleRateHz = fs;
    auto sig = std::make_shared<Signal>(m);
    std::vector<TimestampNs> ts(N);
    std::vector<double> vs(N);
    for (std::size_t i = 0; i < N; ++i) {
        const double t = i / fs;
        ts[i] = static_cast<TimestampNs>(t * 1e9);
        vs[i] = std::sin(2.0 * M_PI * f0 * t);
    }
    sig->append(ts.data(), reinterpret_cast<const std::byte*>(vs.data()), N);
    store.add(sig);

    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("F = FFT(S)", &err)) << err.toStdString();
    auto out = store.get("F");
    auto view = out->snapshotForRead();
    auto mv   = out->readAsDouble();
    // Find the peak bin
    std::size_t peakIdx = 0;
    for (std::size_t i = 1; i < view.count; ++i) {
        if (mv[i] > mv[peakIdx]) peakIdx = i;
    }
    const double peakFreqHz = view.timestamps[peakIdx] / 1e9;
    // Allow ±2 bin slack for Hann window + integer-padding effects.
    const double df = (view.count > 1)
        ? (view.timestamps[1] - view.timestamps[0]) / 1e9 : 0;
    EXPECT_NEAR(peakFreqHz, f0, 2.0 * df) << "peak at " << peakFreqHz << " Hz";
}

// -------- Replace --------

namespace {
std::shared_ptr<Signal> makeFromValues(QString name,
                                        const std::vector<double>& vs) {
    Signal::Meta m;
    m.name = std::move(name);
    m.dataType = DataType::Float64;
    auto sig = std::make_shared<Signal>(m);
    std::vector<TimestampNs> ts(vs.size());
    for (std::size_t i = 0; i < vs.size(); ++i) ts[i] = i * 1'000'000'000LL;
    sig->append(ts.data(), reinterpret_cast<const std::byte*>(vs.data()), vs.size());
    return sig;
}
}  // namespace

TEST(FormulaEngine, ReplaceExactConstantSwapsMatches) {
    SignalStore store;
    store.add(makeFromValues("S", {0, 1, 0, 2, 0, 3}));
    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("R = Replace(S, 0, 99)", &err)) << err.toStdString();
    auto vs = store.get("R")->readAsDouble();
    const std::vector<double> want = {99, 1, 99, 2, 99, 3};
    ASSERT_EQ(vs.size(), want.size());
    for (std::size_t i = 0; i < want.size(); ++i) EXPECT_DOUBLE_EQ(vs[i], want[i]);
}

TEST(FormulaEngine, ReplaceWithToleranceCatchesNearMisses) {
    SignalStore store;
    store.add(makeFromValues("S", {0.0, 1.0, 1e-10, 2.0}));
    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("R = Replace(S, 0, 99, 1e-9)", &err)) << err.toStdString();
    auto vs = store.get("R")->readAsDouble();
    EXPECT_DOUBLE_EQ(vs[0], 99.0);
    EXPECT_DOUBLE_EQ(vs[1],  1.0);
    EXPECT_DOUBLE_EQ(vs[2], 99.0);  // 1e-10 ≤ 1e-9, matched
    EXPECT_DOUBLE_EQ(vs[3],  2.0);
}

TEST(FormulaEngine, ReplaceWithoutToleranceMissesNearZero) {
    SignalStore store;
    store.add(makeFromValues("S", {0.0, 1e-10}));
    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("R = Replace(S, 0, 99)", &err)) << err.toStdString();
    auto vs = store.get("R")->readAsDouble();
    EXPECT_DOUBLE_EQ(vs[0], 99.0);
    EXPECT_DOUBLE_EQ(vs[1], 1e-10);  // exact match fails
}

TEST(FormulaEngine, ReplaceUsesSignalAsReplacement) {
    SignalStore store;
    store.add(makeFromValues("S", {0, 1, 0, 2}));
    store.add(makeFromValues("Fallback", {10, 20, 30, 40}));
    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("R = Replace(S, 0, Fallback)", &err)) << err.toStdString();
    auto vs = store.get("R")->readAsDouble();
    const std::vector<double> want = {10, 1, 30, 2};
    ASSERT_EQ(vs.size(), want.size());
    for (std::size_t i = 0; i < want.size(); ++i) EXPECT_DOUBLE_EQ(vs[i], want[i]);
}

// -------- ForwardFill --------

TEST(FormulaEngine, ForwardFillFillsGapsWithLastGoodValue) {
    SignalStore store;
    store.add(makeFromValues("S", {5, 0, 0, 7, 0, 8}));
    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("F = ForwardFill(S)", &err)) << err.toStdString();
    auto vs = store.get("F")->readAsDouble();
    const std::vector<double> want = {5, 5, 5, 7, 7, 8};
    ASSERT_EQ(vs.size(), want.size());
    for (std::size_t i = 0; i < want.size(); ++i) EXPECT_DOUBLE_EQ(vs[i], want[i]);
}

TEST(FormulaEngine, ForwardFillLeadingFillStaysAsIs) {
    SignalStore store;
    store.add(makeFromValues("S", {0, 0, 5, 0}));
    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("F = ForwardFill(S)", &err)) << err.toStdString();
    auto vs = store.get("F")->readAsDouble();
    const std::vector<double> want = {0, 0, 5, 5};
    ASSERT_EQ(vs.size(), want.size());
    for (std::size_t i = 0; i < want.size(); ++i) EXPECT_DOUBLE_EQ(vs[i], want[i]);
}

TEST(FormulaEngine, ForwardFillCustomMarker) {
    SignalStore store;
    store.add(makeFromValues("S", {5, 99, 99, 7, 99, 8}));
    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("F = ForwardFill(S, 99)", &err)) << err.toStdString();
    auto vs = store.get("F")->readAsDouble();
    const std::vector<double> want = {5, 5, 5, 7, 7, 8};
    ASSERT_EQ(vs.size(), want.size());
    for (std::size_t i = 0; i < want.size(); ++i) EXPECT_DOUBLE_EQ(vs[i], want[i]);
}

TEST(FormulaEngine, ForwardFillTolerantMatchesNearbyValues) {
    SignalStore store;
    // Values that drift slightly from the -28000 sentinel.
    store.add(makeFromValues("S",
        {100, -28013, -27995, 200, -28000, 300}));
    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate(
        "F = ForwardFill(S, -28000, 100)", &err)) << err.toStdString();
    auto vs = store.get("F")->readAsDouble();
    const std::vector<double> want = {100, 100, 100, 200, 200, 300};
    ASSERT_EQ(vs.size(), want.size());
    for (std::size_t i = 0; i < want.size(); ++i) EXPECT_DOUBLE_EQ(vs[i], want[i]);
}

// ---- FFT amplitude calibration + windows + PeakHz interpolation ----------

// Default (Hann) FFT now reports true single-sided amplitude: an on-bin tone
// of amplitude 2.0 peaks at ~2.0. (1024 samples @ 2 kHz → bins at k·1.953 Hz;
// 125 Hz = bin 64 exactly.)
TEST(FormulaEngine, FftHannCalibratedAmplitudeOnBin) {
    SignalStore store;
    store.add(makeSine("S", 1024, 2000.0, 125.0, 2.0));
    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("F = FFT(S)", &err)) << err.toStdString();
    auto f = store.get("F");
    ASSERT_TRUE(f);
    EXPECT_EQ(f->meta().domain, Signal::Domain::Frequency);
    EXPECT_NEAR(peakValue(f), 2.0, 0.02);   // within 1%
}

// Flat-top (window=1) keeps amplitude accurate even for an OFF-bin tone,
// where Hann would scallop-loss ~15% low. 125.97 Hz sits between bins 64/65.
TEST(FormulaEngine, FftFlatTopAmplitudeAccurateOffBin) {
    SignalStore store;
    store.add(makeSine("S", 1024, 2000.0, 125.97, 2.0));
    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("Fh = FFT(S, 1)", &err)) << err.toStdString();
    EXPECT_NEAR(peakValue(store.get("Fh")), 2.0, 0.06);   // flat-top within ~3%
}

// PeakHz refines the peak between bins. 2 s @ 2 kHz → N=4096, df≈0.488 Hz, so
// the raw nearest bin is up to 0.244 Hz off; interpolation gets well inside it.
TEST(FormulaEngine, PeakHzInterpolatesBetweenBins) {
    SignalStore store;
    store.add(makeSine("S", 4000, 2000.0, 125.3, 1.0));
    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("pk = PeakHz(S)", &err)) << err.toStdString();
    auto v = store.get("pk")->readAsDouble();
    ASSERT_EQ(v.size(), 1u);                 // scalar result
    EXPECT_NEAR(v[0], 125.3, 0.1);           // ≪ half a bin (0.244 Hz)
}

TEST(FormulaEngine, PeakHzAcceptsWindowArg) {
    SignalStore store;
    store.add(makeSine("S", 4000, 2000.0, 311.7, 1.0));
    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("pk = PeakHz(S, 1)", &err)) << err.toStdString();
    EXPECT_NEAR(store.get("pk")->readAsDouble().at(0), 311.7, 0.2);
}

TEST(FormulaEngine, FftRejectsBadWindowCode) {
    SignalStore store;
    store.add(makeSine("S", 256, 2000.0, 100.0, 1.0));
    FormulaEngine engine(store);
    QString err;
    EXPECT_FALSE(engine.evaluate("F = FFT(S, 5)", &err));
    EXPECT_FALSE(err.isEmpty());
}

// ---- Gate(signal, gate, low, high [, min_length [, mode]]) + FFTWelch -----

// Smash (default): keep in-range samples, close the gaps into one contiguous
// uniformly-spaced signal.
TEST(FormulaEngine, GateSmashClosesGaps) {
    SignalStore store;
    std::vector<double> sv(100), gv(100, 0.0);
    for (std::size_t i = 0; i < 100; ++i) sv[i] = static_cast<double>(i);
    for (std::size_t i = 20; i < 40; ++i) gv[i] = 1.0;   // window 1
    for (std::size_t i = 60; i < 80; ++i) gv[i] = 1.0;   // window 2
    store.add(makeSeries("S", sv, 100.0));
    store.add(makeSeries("G", gv, 100.0));
    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("out = Gate(S, G, 0.5, 1.5)", &err)) << err.toStdString();
    auto vs = store.get("out")->readAsDouble();
    ASSERT_EQ(vs.size(), 40u);
    for (std::size_t i = 0; i < 20; ++i) EXPECT_DOUBLE_EQ(vs[i], 20.0 + i);
    for (std::size_t i = 0; i < 20; ++i) EXPECT_DOUBLE_EQ(vs[20 + i], 60.0 + i);
    auto view = store.get("out")->snapshotForRead();
    for (std::size_t i = 1; i < view.count; ++i)
        EXPECT_EQ(view.timestamps[i] - view.timestamps[i - 1], 10'000'000);
}

// min_length drops a brief blip but keeps a sustained window — exactly the
// "data point in the middle of the gap" case.
TEST(FormulaEngine, GateMinLengthDropsBlips) {
    SignalStore store;
    std::vector<double> sv(20), gv(20, 0.0);
    for (std::size_t i = 0; i < 20; ++i) sv[i] = static_cast<double>(i);
    for (std::size_t i = 4; i < 9; ++i) gv[i] = 1.0;     // window: idx 4..8 (0.4 s @ 10 Hz)
    gv[14] = 1.0;                                        // lone 1-sample blip (0 s span)
    store.add(makeSeries("S", sv, 10.0));
    store.add(makeSeries("G", gv, 10.0));
    FormulaEngine engine(store);
    QString err;
    // No min_length → blip kept (5 + 1 = 6 samples).
    ASSERT_TRUE(engine.evaluate("a = Gate(S, G, 0.5, 1.5)", &err)) << err.toStdString();
    EXPECT_EQ(store.get("a")->sampleCount(), 6u);
    // min_length 0.1 s → blip (0 s) dropped, window (0.4 s) kept → 5 samples.
    ASSERT_TRUE(engine.evaluate("b = Gate(S, G, 0.5, 1.5, 0.1)", &err)) << err.toStdString();
    auto vs = store.get("b")->readAsDouble();
    ASSERT_EQ(vs.size(), 5u);
    for (std::size_t i = 0; i < 5; ++i) EXPECT_DOUBLE_EQ(vs[i], 4.0 + i);
}

// Real-time mode (mode 1) keeps the original timestamps; the gap is preserved.
TEST(FormulaEngine, GateRealTimeKeepsTimestamps) {
    SignalStore store;
    std::vector<double> sv(20), gv(20, 0.0);
    for (std::size_t i = 0; i < 20; ++i) sv[i] = static_cast<double>(i);
    for (std::size_t i = 4; i < 9;  ++i) gv[i] = 1.0;    // window 1: idx 4..8
    for (std::size_t i = 14; i < 17; ++i) gv[i] = 1.0;   // window 2: idx 14..16
    store.add(makeSeries("S", sv, 10.0));                // dt = 0.1 s (1e8 ns)
    store.add(makeSeries("G", gv, 10.0));
    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("out = Gate(S, G, 0.5, 1.5, 0, 1)", &err)) << err.toStdString();
    auto out = store.get("out");
    auto vs = out->readAsDouble();
    auto view = out->snapshotForRead();
    ASSERT_EQ(vs.size(), 8u);                            // 5 + 3
    EXPECT_DOUBLE_EQ(vs[0], 4.0);
    EXPECT_DOUBLE_EQ(vs[5], 14.0);
    EXPECT_EQ(view.timestamps[0], 4 * 100'000'000);      // idx 4 at its real time
    EXPECT_EQ(view.timestamps[5] - view.timestamps[4], 6 * 100'000'000);  // real gap idx 8→14
}

// FFTWelch averages a separate FFT per event window — recovers the event
// frequency with no concatenation seam.
TEST(FormulaEngine, FFTWelchRecoversEventFrequency) {
    SignalStore store;
    store.add(makeSine("vib", 4000, 2000.0, 125.0, 1.0));
    std::vector<double> pos(4000, 0.0);
    for (std::size_t i = 500;  i < 1500; ++i) pos[i] = 1.0;
    for (std::size_t i = 2500; i < 3500; ++i) pos[i] = 1.0;
    store.add(makeSeries("pos", pos, 2000.0));
    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("spec = FFTWelch(vib, pos, 0.5, 1.5)", &err)) << err.toStdString();
    auto spec = store.get("spec");
    ASSERT_TRUE(spec);
    EXPECT_EQ(spec->meta().domain, Signal::Domain::Frequency);

    auto vs = spec->readAsDouble();
    auto view = spec->snapshotForRead();
    std::size_t kmax = 1;
    double best = -1.0;
    for (std::size_t k = 1; k < vs.size(); ++k)
        if (vs[k] > best) { best = vs[k]; kmax = k; }
    const double hz = static_cast<double>(view.timestamps[kmax]) * 1e-9;
    EXPECT_NEAR(hz, 125.0, 2.0);
    EXPECT_GT(best, 0.3);                                // a real peak, calibrated-ish
}

TEST(FormulaEngine, GateRejectsBadArgs) {
    SignalStore store;
    store.add(makeSeries("S", {1, 2, 3}, 10.0));
    store.add(makeSeries("G", {0, 1, 0}, 10.0));
    FormulaEngine engine(store);
    QString err;
    EXPECT_FALSE(engine.evaluate("a = Gate(S, G, 0.5)", &err));            // too few args
    EXPECT_FALSE(engine.evaluate("b = Gate(S, G, 0.5, 1.5, 0, 2)", &err)); // bad mode
    EXPECT_FALSE(engine.evaluate("c = Gate(S, G, 0.5, 1.5, 0, 1, 9)", &err)); // too many args
}

// ---- Reverse / Butterworth / filtfilt ------------------------------------

TEST(FormulaEngine, ReverseTwiceIsIdentity) {
    SignalStore store;
    store.add(makeRamp("A", 50, 0.01));   // values 0..49
    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("R  = Reverse(A)",  &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("RR = Reverse(R)",  &err)) << err.toStdString();

    auto a = store.get("A"); auto r = store.get("R"); auto rr = store.get("RR");
    ASSERT_TRUE(r); ASSERT_TRUE(rr);
    auto av = a->readAsDouble(); auto rv = r->readAsDouble(); auto rrv = rr->readAsDouble();
    ASSERT_EQ(rv.size(), av.size());
    EXPECT_DOUBLE_EQ(rv.front(), av.back());    // values flipped
    EXPECT_DOUBLE_EQ(rv.back(),  av.front());

    auto avw = a->snapshotForRead(); auto rrw = rr->snapshotForRead();
    ASSERT_EQ(rrw.count, avw.count);
    for (std::size_t i = 0; i < avw.count; ++i) {
        EXPECT_DOUBLE_EQ(rrv[i], av[i]);                       // values restored
        EXPECT_EQ(rrw.timestamps[i], avw.timestamps[i]);      // timestamps restored
    }
}

TEST(FormulaEngine, ButterworthPassesDC) {
    SignalStore store;
    store.add(makeSeries("A", std::vector<double>(200, 5.0), 100.0));
    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("B = Butterworth(A, 0, 2, 10)", &err)) << err.toStdString();
    auto bv = store.get("B")->readAsDouble();
    EXPECT_NEAR(bv.back(), 5.0, 1e-6);          // unity DC gain → settles to input
}

TEST(FormulaEngine, ButterworthLowPassSeparatesBands) {
    SignalStore store;
    const double fs = 1000.0;
    store.add(makeSine("LOW",  4000, fs,   5.0, 1.0));   // well below cutoff
    store.add(makeSine("HIGH", 4000, fs, 200.0, 1.0));   // well above cutoff
    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("BL = Butterworth(LOW,  0, 4, 50)", &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("BH = Butterworth(HIGH, 0, 4, 50)", &err)) << err.toStdString();
    auto amp = [](const std::shared_ptr<Signal>& s) {
        auto v = s->readAsDouble();
        double m = 0;
        for (std::size_t i = v.size() / 2; i < v.size(); ++i) m = std::max(m, std::abs(v[i]));
        return m;
    };
    EXPECT_GT(amp(store.get("BL")), 0.9);       // passband
    EXPECT_LT(amp(store.get("BH")), 0.1);       // stopband
}

TEST(FormulaEngine, ButterworthMinus3dBAtCutoff) {
    SignalStore store;
    const double fs = 2000.0, fc = 100.0;
    store.add(makeSine("S", 8000, fs, fc, 1.0));   // sine exactly at the cutoff
    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("B = Butterworth(S, 0, 4, 100)", &err)) << err.toStdString();
    auto v = store.get("B")->readAsDouble();
    double m = 0;
    for (std::size_t i = v.size() / 2; i < v.size(); ++i) m = std::max(m, std::abs(v[i]));
    EXPECT_NEAR(m, 1.0 / std::sqrt(2.0), 0.05); // Butterworth is -3 dB at fc
}

TEST(FormulaEngine, ButterworthRejectsCutoffAboveNyquist) {
    SignalStore store;
    store.add(makeSine("S", 200, 100.0, 5.0, 1.0));   // fs = 100 → Nyquist 50
    FormulaEngine engine(store);
    QString err;
    EXPECT_FALSE(engine.evaluate("B = Butterworth(S, 0, 2, 60)", &err));
    EXPECT_FALSE(err.isEmpty());
}

// A symmetric pulse: a causal filter delays its peak; filtfilt keeps it put.
static std::shared_ptr<Signal> makePulse(QString name, std::size_t n,
                                         int center, double fs) {
    std::vector<double> vals(n, 0.0);
    for (int i = 0; i < static_cast<int>(n); ++i)
        vals[i] = std::max(0.0, 50.0 - std::abs(i - center));
    return makeSeries(std::move(name), vals, fs);
}

TEST(FormulaEngine, FiltFiltIsZeroPhaseFilter) {
    SignalStore store;
    const int center = 500;
    store.add(makePulse("P", 1000, center, 1000.0));
    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("F  = Filter(P, 0.02)",          &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("FF = filtfilt(Filter, P, 0.02)", &err)) << err.toStdString();
    auto argmax = [](const std::shared_ptr<Signal>& s) {
        auto v = s->readAsDouble();
        int best = 0; double bv = v[0];
        for (int i = 1; i < static_cast<int>(v.size()); ++i)
            if (v[i] > bv) { bv = v[i]; best = i; }
        return best;
    };
    EXPECT_GT(argmax(store.get("F")), center + 3);   // causal filter lags
    EXPECT_NEAR(argmax(store.get("FF")), center, 2); // zero-phase stays centered
}

TEST(FormulaEngine, FiltFiltWithButterworthIsZeroPhase) {
    SignalStore store;
    const int center = 500;
    store.add(makePulse("P", 1000, center, 1000.0));
    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("FF = filtfilt(Butterworth, P, 0, 4, 30)", &err))
        << err.toStdString();
    auto v = store.get("FF")->readAsDouble();
    int best = 0; double bv = v[0];
    for (int i = 1; i < static_cast<int>(v.size()); ++i)
        if (v[i] > bv) { bv = v[i]; best = i; }
    EXPECT_NEAR(best, center, 2);
}

TEST(FormulaEngine, FiltFiltRejectsNonFilterAndBadSyntax) {
    SignalStore store;
    store.add(makeRamp("A", 50, 0.01));
    FormulaEngine engine(store);
    QString err;
    EXPECT_FALSE(engine.evaluate("X = filtfilt(Abs, A)", &err));       // not a filter
    EXPECT_FALSE(err.isEmpty());
    EXPECT_FALSE(engine.evaluate("Y = filtfilt(A, 0.05)", &err));      // 1st arg not a filter name
    EXPECT_FALSE(engine.evaluate("Z = filtfilt(Filter, A)", &err));    // Filter needs tau
}

TEST(FormulaEngine, FilterHPPlusLowPassReconstructsInput) {
    SignalStore store;
    store.add(makeSine("A", 2000, 1000.0, 7.0, 1.0));
    FormulaEngine engine(store); QString err;
    ASSERT_TRUE(engine.evaluate("LP  = Filter(A, 0.05)",   &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("HP  = FilterHP(A, 0.05)", &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("SUM = LP + HP",           &err)) << err.toStdString();
    auto a = store.get("A")->readAsDouble();
    auto s = store.get("SUM")->readAsDouble();
    ASSERT_EQ(a.size(), s.size());
    for (std::size_t i = 0; i < a.size(); ++i) EXPECT_NEAR(s[i], a[i], 1e-9);
}

TEST(FormulaEngine, FilterHPRemovesDcKeepsFastChanges) {
    SignalStore store;
    store.add(makeSeries("C",  std::vector<double>(500, 3.0), 100.0));
    store.add(makeSine  ("HI", 4000, 1000.0, 200.0, 1.0));   // above cutoff
    store.add(makeSine  ("LO", 4000, 1000.0,   1.0, 1.0));   // below cutoff
    FormulaEngine engine(store); QString err;
    ASSERT_TRUE(engine.evaluate("DC = FilterHP(C,  0.05)", &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("BH = FilterHP(HI, 0.05)", &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("BL = FilterHP(LO, 0.05)", &err)) << err.toStdString();
    auto amp = [&](const char* nm) {
        auto v = store.get(nm)->readAsDouble();
        double m = 0; for (std::size_t i = v.size()/2; i < v.size(); ++i) m = std::max(m, std::abs(v[i]));
        return m;
    };
    EXPECT_LT(amp("DC"), 1e-6);     // DC / drift removed
    EXPECT_GT(amp("BH"), 0.9);      // fast changes pass
    EXPECT_LT(amp("BL"), 0.4);      // slow content attenuated
}

TEST(FormulaEngine, FiltFiltWorksWithFilterHP) {
    SignalStore store;
    store.add(makeSine("A", 4000, 1000.0, 1.0, 1.0));
    FormulaEngine engine(store); QString err;
    ASSERT_TRUE(engine.evaluate("Z = filtfilt(FilterHP, A, 0.05)", &err)) << err.toStdString();
    EXPECT_EQ(store.get("Z")->sampleCount(), 4000u);
}

TEST(FormulaEngine, ButterworthHighPassSeparatesBands) {
    SignalStore store;
    const double fs = 1000.0;
    store.add(makeSine("LOW",  4000, fs,   5.0, 1.0));   // below cutoff → blocked
    store.add(makeSine("HIGH", 4000, fs, 200.0, 1.0));   // above cutoff → passes
    FormulaEngine engine(store); QString err;
    ASSERT_TRUE(engine.evaluate("BL = Butterworth(LOW,  1, 4, 50)", &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("BH = Butterworth(HIGH, 1, 4, 50)", &err)) << err.toStdString();
    auto amp = [&](const char* nm) {
        auto v = store.get(nm)->readAsDouble();
        double m = 0; for (std::size_t i = v.size()/2; i < v.size(); ++i) m = std::max(m, std::abs(v[i]));
        return m;
    };
    EXPECT_LT(amp("BL"), 0.1);    // low frequency blocked
    EXPECT_GT(amp("BH"), 0.9);    // high frequency passes
    // and it composes in filtfilt for a zero-phase high-pass
    ASSERT_TRUE(engine.evaluate("Z = filtfilt(Butterworth, HIGH, 1, 4, 50)", &err))
        << err.toStdString();
    EXPECT_EQ(store.get("Z")->sampleCount(), 4000u);
}

// Steady-state amplitude over the second half (avoids the startup transient).
static double ssAmp(const std::shared_ptr<Signal>& s) {
    auto v = s->readAsDouble();
    double m = 0;
    for (std::size_t i = v.size() / 2; i < v.size(); ++i) m = std::max(m, std::abs(v[i]));
    return m;
}

TEST(FormulaEngine, Cheby1LowPassRippleAndStopband) {
    SignalStore store;
    const double fs = 1000.0;
    store.add(makeSine("LOW",  4000, fs,   5.0, 1.0));   // deep in passband
    store.add(makeSine("HIGH", 4000, fs, 200.0, 1.0));   // deep in stopband
    FormulaEngine engine(store); QString err;
    // order 4, 1 dB passband ripple, cutoff 50 Hz
    ASSERT_TRUE(engine.evaluate("PL = Cheby1(LOW,  0, 4, 1, 50)", &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("PH = Cheby1(HIGH, 0, 4, 1, 50)", &err)) << err.toStdString();
    // passband stays within the 1 dB ripple band [10^(-1/20), 1] ≈ [0.891, 1]
    EXPECT_GT(ssAmp(store.get("PL")), 0.85);
    EXPECT_LT(ssAmp(store.get("PL")), 1.05);
    EXPECT_LT(ssAmp(store.get("PH")), 0.1);    // stopband
}

TEST(FormulaEngine, Cheby2LowPassFlatPassbandStopAttenuation) {
    SignalStore store;
    const double fs = 1000.0;
    store.add(makeSine("LOW",  4000, fs,   5.0, 1.0));
    store.add(makeSine("HIGH", 4000, fs, 200.0, 1.0));
    FormulaEngine engine(store); QString err;
    // order 4, 40 dB stopband attenuation, cutoff 50 Hz
    ASSERT_TRUE(engine.evaluate("PL = Cheby2(LOW,  0, 4, 40, 50)", &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("PH = Cheby2(HIGH, 0, 4, 40, 50)", &err)) << err.toStdString();
    EXPECT_GT(ssAmp(store.get("PL")), 0.95);   // flat passband
    EXPECT_LT(ssAmp(store.get("PH")), 0.05);   // stopband ≤ ~10^(-40/20)=0.01
}

TEST(FormulaEngine, EllipticLowPassRippleAndStopband) {
    SignalStore store;
    const double fs = 1000.0;
    store.add(makeSine("LOW",  4000, fs,   5.0, 1.0));   // deep in passband
    store.add(makeSine("HIGH", 4000, fs, 200.0, 1.0));   // deep in stopband
    FormulaEngine engine(store); QString err;
    // order 4, Rp = 1 dB passband ripple, Rs = 40 dB stop atten, cutoff 50 Hz
    ASSERT_TRUE(engine.evaluate("EL = Elliptic(LOW,  0, 4, 1, 40, 50)", &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("EH = Elliptic(HIGH, 0, 4, 1, 40, 50)", &err)) << err.toStdString();
    // passband stays within the 1 dB ripple band [10^(-1/20), 1] ≈ [0.891, 1]
    EXPECT_GT(ssAmp(store.get("EL")), 0.85);
    EXPECT_LT(ssAmp(store.get("EL")), 1.05);
    // stopband at 200 Hz must be at least 40 dB down (≤ 10^(-40/20) = 0.01)
    EXPECT_LT(ssAmp(store.get("EH")), 0.01 * 1.5);
}

TEST(FormulaEngine, EllipticSteeperThanButterworth) {
    SignalStore store;
    const double fs = 1000.0;
    // 80 Hz tone, just above the 50 Hz cutoff (1.6×fc) — the transition band
    // where elliptic's much steeper rolloff shows clearly vs Butterworth.
    store.add(makeSine("S", 4000, fs, 80.0, 1.0));
    FormulaEngine engine(store); QString err;
    ASSERT_TRUE(engine.evaluate("B = Butterworth(S, 0, 4, 50)",     &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("E = Elliptic(S, 0, 4, 1, 40, 50)", &err)) << err.toStdString();
    // Same order → elliptic attenuates the just-past-cutoff tone far more.
    EXPECT_LT(ssAmp(store.get("E")), ssAmp(store.get("B")));
}

TEST(FormulaEngine, EllipticBandPass) {
    SignalStore store;
    const double fs = 1000.0;
    store.add(makeSine("LO",  4000, fs,   10.0, 1.0));   // below band
    store.add(makeSine("MID", 4000, fs,   50.0, 1.0));   // inside band (40..60)
    store.add(makeSine("HI",  4000, fs,  200.0, 1.0));   // above band
    FormulaEngine engine(store); QString err;
    ASSERT_TRUE(engine.evaluate("BPlo  = Elliptic(LO,  2, 4, 1, 40, 40, 60)", &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("BPmid = Elliptic(MID, 2, 4, 1, 40, 40, 60)", &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("BPhi  = Elliptic(HI,  2, 4, 1, 40, 40, 60)", &err)) << err.toStdString();
    EXPECT_GT(ssAmp(store.get("BPmid")), 0.7);
    EXPECT_LT(ssAmp(store.get("BPlo")),  0.2);
    EXPECT_LT(ssAmp(store.get("BPhi")),  0.2);
}

TEST(FormulaEngine, EllipticRejectsBadArgs) {
    SignalStore store;
    store.add(makeSine("S", 1000, 1000.0, 5.0, 1.0));
    FormulaEngine engine(store); QString err;
    EXPECT_FALSE(engine.evaluate("a = Elliptic(S, 0, 4, 50)", &err));         // missing Rs + ripples
    EXPECT_FALSE(engine.evaluate("b = Elliptic(S, 0, 4, 1, 40, 50, 60)", &err)); // LP takes one cutoff
    EXPECT_FALSE(engine.evaluate("c = Elliptic(S, 2, 4, 1, 40, 50)", &err));  // BP needs f2
    EXPECT_FALSE(engine.evaluate("d = Elliptic(S, 0, 4, 40, 1, 50)", &err));  // Rs must exceed Rp
    EXPECT_TRUE (engine.evaluate("e = filtfilt(Elliptic, S, 0, 4, 1, 40, 50)", &err)) << err.toStdString();
}

TEST(FormulaEngine, FiltFiltWithEllipticIsZeroPhase) {
    SignalStore store;
    const int center = 500;
    store.add(makePulse("P", 1000, center, 1000.0));
    FormulaEngine engine(store); QString err;
    // Zero-phase: forward+backward elliptic leaves a symmetric pulse with its
    // peak still centered (mirrors the Butterworth filtfilt test).
    ASSERT_TRUE(engine.evaluate("FF = filtfilt(Elliptic, P, 0, 4, 1, 40, 30)", &err))
        << err.toStdString();
    auto ff = store.get("FF")->readAsDouble();
    int bestFF = 0; double bv = ff[0];
    for (int i = 1; i < static_cast<int>(ff.size()); ++i)
        if (ff[i] > bv) { bv = ff[i]; bestFF = i; }
    EXPECT_NEAR(bestFF, center, 3);

    // The plain causal elliptic, by contrast, lags — its peak lands later.
    ASSERT_TRUE(engine.evaluate("CAUS = Elliptic(P, 0, 4, 1, 40, 30)", &err))
        << err.toStdString();
    auto cs = store.get("CAUS")->readAsDouble();
    int bestC = 0; double cb = cs[0];
    for (int i = 1; i < static_cast<int>(cs.size()); ++i)
        if (cs[i] > cb) { cb = cs[i]; bestC = i; }
    EXPECT_GT(bestC, center + 3);
}

TEST(FormulaEngine, BandPassAndBandStop) {
    SignalStore store;
    const double fs = 1000.0;
    store.add(makeSine("LO",  4000, fs,   10.0, 1.0));   // below band
    store.add(makeSine("MID", 4000, fs,   50.0, 1.0));   // inside band (40..60)
    store.add(makeSine("HI",  4000, fs,  200.0, 1.0));   // above band
    FormulaEngine engine(store); QString err;
    // Band-pass 40..60 Hz
    ASSERT_TRUE(engine.evaluate("BPlo  = Butterworth(LO,  2, 2, 40, 60)", &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("BPmid = Butterworth(MID, 2, 2, 40, 60)", &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("BPhi  = Butterworth(HI,  2, 2, 40, 60)", &err)) << err.toStdString();
    EXPECT_GT(ssAmp(store.get("BPmid")), 0.7);
    EXPECT_LT(ssAmp(store.get("BPlo")),  0.2);
    EXPECT_LT(ssAmp(store.get("BPhi")),  0.2);
    // Band-stop 40..60 Hz: the complement
    ASSERT_TRUE(engine.evaluate("BSlo  = Butterworth(LO,  3, 2, 40, 60)", &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("BSmid = Butterworth(MID, 3, 2, 40, 60)", &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("BShi  = Butterworth(HI,  3, 2, 40, 60)", &err)) << err.toStdString();
    EXPECT_LT(ssAmp(store.get("BSmid")), 0.3);
    EXPECT_GT(ssAmp(store.get("BSlo")),  0.8);
    EXPECT_GT(ssAmp(store.get("BShi")),  0.8);
}

TEST(FormulaEngine, IirRejectsBadArgs) {
    SignalStore store;
    store.add(makeSine("S", 1000, 1000.0, 5.0, 1.0));
    FormulaEngine engine(store); QString err;
    EXPECT_FALSE(engine.evaluate("a = Butterworth(S, 5, 4, 50)", &err));     // band out of range
    EXPECT_FALSE(engine.evaluate("b = Butterworth(S, 2, 4, 50)", &err));     // BP needs f2
    EXPECT_FALSE(engine.evaluate("c = Butterworth(S, 0, 4, 50, 60)", &err)); // LP takes one cutoff
    EXPECT_FALSE(engine.evaluate("d = Cheby1(S, 0, 4, 50)", &err));          // missing ripple arg
    EXPECT_TRUE (engine.evaluate("e = filtfilt(Cheby1, S, 0, 4, 1, 50)", &err)) << err.toStdString();
}

TEST(FormulaEngine, PtMatchesFilterAtOrderOne) {
    SignalStore store;
    store.add(makeSine("S", 2000, 1000.0, 7.0, 1.0));
    FormulaEngine engine(store); QString err;
    ASSERT_TRUE(engine.evaluate("LP  = Filter(S, 0.05)",     &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("P0  = PT(S, 0, 1, 0.05)",   &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("HP  = FilterHP(S, 0.05)",   &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("P1  = PT(S, 1, 1, 0.05)",   &err)) << err.toStdString();
    auto lp = store.get("LP")->readAsDouble(); auto p0 = store.get("P0")->readAsDouble();
    auto hp = store.get("HP")->readAsDouble(); auto p1 = store.get("P1")->readAsDouble();
    ASSERT_EQ(lp.size(), p0.size());
    for (std::size_t i = 0; i < lp.size(); ++i) {
        EXPECT_NEAR(p0[i], lp[i], 1e-12);   // PT order-1 low  == Filter
        EXPECT_NEAR(p1[i], hp[i], 1e-12);   // PT order-1 high == FilterHP
    }
}

TEST(FormulaEngine, PtHigherOrderIsSteeper) {
    SignalStore store;
    store.add(makeSine("S", 8000, 1000.0, 20.0, 1.0));   // above the ~3 Hz corner
    FormulaEngine engine(store); QString err;
    ASSERT_TRUE(engine.evaluate("P1 = PT(S, 0, 1, 0.05)", &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("P3 = PT(S, 0, 3, 0.05)", &err)) << err.toStdString();
    EXPECT_LT(ssAmp(store.get("P3")), ssAmp(store.get("P1")));   // more stages → more attenuation
}

TEST(FormulaEngine, PtRejectsBandPass) {
    SignalStore store;
    store.add(makeSine("S", 1000, 1000.0, 5.0, 1.0));
    FormulaEngine engine(store); QString err;
    EXPECT_FALSE(engine.evaluate("a = PT(S, 2, 2, 0.05)", &err));   // band-pass not supported
    EXPECT_FALSE(engine.evaluate("b = PT(S, 0, 2)", &err));         // missing tau
    EXPECT_TRUE (engine.evaluate("c = filtfilt(PT, S, 0, 2, 0.05)", &err)) << err.toStdString();
}

TEST(FormulaEngine, FilterResponsePreviewSanity) {
    auto magAt = [](const FilterFreqResponse& r, double ratio) {
        double best = 1e18, m = 0;
        for (std::size_t i = 0; i < r.freqHz.size(); ++i) {
            const double d = std::abs(r.freqHz[i] / r.fc - ratio);
            if (d < best) { best = d; m = r.magDb[i]; }
        }
        return m;
    };
    // Butterworth low-pass order 4: ~0 dB passband, -3 dB at fc, steep rolloff.
    FilterPlotSpec lp; lp.family = "Butterworth"; lp.band = 0; lp.order = 4;
    const auto rl = filterFreqResponse(lp);
    ASSERT_GT(rl.fc, 0.0);
    ASSERT_EQ(rl.freqHz.size(), rl.magDb.size());
    EXPECT_GT(magAt(rl, 0.1),  -1.0);
    EXPECT_NEAR(magAt(rl, 1.0), -3.0, 1.5);
    EXPECT_LT(magAt(rl, 8.0), -40.0);
    // 1st-order high-pass: blocks DC-side, passes high.
    FilterPlotSpec hp; hp.family = "Filter"; hp.band = 1; hp.order = 1;
    const auto rh = filterFreqResponse(hp);
    EXPECT_LT(magAt(rh, 0.1), -15.0);
    EXPECT_GT(magAt(rh, 10.0), -1.0);
}

TEST(FormulaEngine, FftCutKeepRoundTrip) {
    // fs = N so 10 Hz / 100 Hz land exactly on bins → clean, no leakage/edges.
    const double fs = 1024.0; const std::size_t n = 1024;
    std::vector<double> vals(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = i / fs;
        vals[i] = std::sin(2*M_PI*10.0*t) + 0.5*std::sin(2*M_PI*100.0*t);
    }
    SignalStore store; store.add(makeSeries("X", vals, fs));
    FormulaEngine engine(store); QString err;
    ASSERT_TRUE(engine.evaluate("LO  = FFTKeep(X, 5, 15)",   &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("HI  = FFTKeep(X, 90, 110)", &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("CUT = FFTCut(X, 90, 110)",  &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("ALL = FFTKeep(X, 0, 520)",  &err)) << err.toStdString();
    auto mx = [&](const char* nm) {
        auto v = store.get(nm)->readAsDouble(); double m = 0;
        for (double x : v) m = std::max(m, std::abs(x));
        return m;
    };
    EXPECT_NEAR(mx("LO"),  1.0, 0.02);   // isolated 10 Hz tone
    EXPECT_NEAR(mx("HI"),  0.5, 0.02);   // isolated 100 Hz tone
    EXPECT_NEAR(mx("CUT"), 1.0, 0.02);   // 100 Hz removed → 10 Hz remains

    // Keeping everything up to Nyquist is the identity (within FFT round-off).
    auto x = store.get("X")->readAsDouble();
    auto all = store.get("ALL")->readAsDouble();
    ASSERT_EQ(x.size(), all.size());
    double maxDiff = 0;
    for (std::size_t i = 0; i < x.size(); ++i) maxDiff = std::max(maxDiff, std::abs(x[i]-all[i]));
    EXPECT_LT(maxDiff, 1e-9);
}

TEST(FormulaEngine, FftCutKeepRejectBadArgs) {
    SignalStore store; store.add(makeSine("S", 256, 1000.0, 10.0, 1.0));
    FormulaEngine engine(store); QString err;
    EXPECT_FALSE(engine.evaluate("a = FFTCut(S, 100, 50)", &err));   // f_hi <= f_lo
    EXPECT_FALSE(engine.evaluate("b = FFTCut(S, 50)", &err));        // wrong arg count
    EXPECT_TRUE (engine.evaluate("c = FFTKeep(S, 5, 15)", &err)) << err.toStdString();
}

TEST(FormulaEngine, RoundOptionalTolerance) {
    SignalStore store;
    store.add(makeSeries("X", {0.98, 1.4, 2.02, 2.5}, 100.0));
    FormulaEngine engine(store); QString err;
    ASSERT_TRUE(engine.evaluate("R0 = Round(X)",      &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("RT = Round(X, 0.1)", &err)) << err.toStdString();
    auto r0 = store.get("R0")->readAsDouble();
    auto rt = store.get("RT")->readAsDouble();
    ASSERT_EQ(r0.size(), 4u); ASSERT_EQ(rt.size(), 4u);
    EXPECT_DOUBLE_EQ(r0[0], 1.0); EXPECT_DOUBLE_EQ(r0[1], 1.0);
    EXPECT_DOUBLE_EQ(r0[2], 2.0); EXPECT_DOUBLE_EQ(r0[3], 3.0);   // plain round
    EXPECT_DOUBLE_EQ(rt[0], 1.0);   // 0.98 within 0.1 of 1 → snaps
    EXPECT_DOUBLE_EQ(rt[1], 1.4);   // 0.4 away → left unchanged
    EXPECT_DOUBLE_EQ(rt[2], 2.0);   // 2.02 within 0.1 → snaps
    EXPECT_DOUBLE_EQ(rt[3], 2.5);   // 0.5 away → left unchanged
    EXPECT_FALSE(engine.evaluate("E = Round(X, 0.1, 1)", &err));  // too many args
}

TEST(FormulaEngine, RevertFftRoundTrip) {
    const std::size_t n = 256; const double fs = 256.0;   // power of two → exact
    std::vector<double> vals(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = i / fs;
        vals[i] = std::sin(2*M_PI*5*t) + 0.4*std::sin(2*M_PI*30*t) + 0.2*(i / double(n));
    }
    SignalStore store; store.add(makeSeries("X", vals, fs));
    FormulaEngine engine(store); QString err;
    ASSERT_TRUE(engine.evaluate("M = RFFTmag(X)",      &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("P = RFFTphase(X)",    &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("Y = revertFFT(M, P)", &err)) << err.toStdString();
    auto x = store.get("X")->readAsDouble();
    auto y = store.get("Y")->readAsDouble();
    ASSERT_EQ(y.size(), x.size());
    double maxDiff = 0;
    for (std::size_t i = 0; i < x.size(); ++i) maxDiff = std::max(maxDiff, std::abs(x[i]-y[i]));
    EXPECT_LT(maxDiff, 1e-9);
}

TEST(FormulaEngine, RevertFftBandZeroRemovesTone) {
    const std::size_t n = 1024; const double fs = 1024.0;
    std::vector<double> vals(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = i / fs;
        vals[i] = std::sin(2*M_PI*10*t) + 0.5*std::sin(2*M_PI*100*t);
    }
    SignalStore store; store.add(makeSeries("X", vals, fs));
    FormulaEngine engine(store); QString err;
    ASSERT_TRUE(engine.evaluate("M  = RFFTmag(X)",          &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("P  = RFFTphase(X)",        &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("M2 = BandZero(M, 90, 110)", &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("Y  = revertFFT(M2, P)",    &err)) << err.toStdString();
    auto y = store.get("Y")->readAsDouble();
    double mx = 0; for (double v : y) mx = std::max(mx, std::abs(v));
    EXPECT_NEAR(mx, 1.0, 0.02);     // 100 Hz removed → only the 10 Hz tone (amp 1)
    // BandZero must zero exactly the in-band bins and leave the rest.
    auto m  = store.get("M")->readAsDouble();
    auto m2 = store.get("M2")->readAsDouble();
    auto mv = store.get("M")->snapshotForRead();
    bool ok = true;
    for (std::size_t k = 0; k < mv.count; ++k) {
        const double hz = mv.timestamps[k] / 1e9;
        if (hz >= 90 && hz <= 110) { if (m2[k] != 0.0)  ok = false; }
        else                       { if (m2[k] != m[k]) ok = false; }
    }
    EXPECT_TRUE(ok);
}

TEST(FormulaEngine, RevertFftRejectsBadArgs) {
    SignalStore store;
    store.add(makeSeries("X", std::vector<double>(256, 1.0), 256.0));
    store.add(makeSine("S", 128, 128.0, 5.0, 1.0));
    FormulaEngine engine(store); QString err;
    ASSERT_TRUE(engine.evaluate("M = RFFTmag(X)",   &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("P = RFFTphase(S)", &err)) << err.toStdString();  // mismatched length
    EXPECT_FALSE(engine.evaluate("Y = revertFFT(M, P)",   &err));   // bin counts differ
    EXPECT_FALSE(engine.evaluate("Z = BandZero(M, 100, 50)", &err)); // f_hi < f_lo
}

TEST(FormulaEngine, RevertFftBandKeepIsolatesTone) {
    const std::size_t n = 1024; const double fs = 1024.0;
    std::vector<double> vals(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = i / fs;
        vals[i] = std::sin(2*M_PI*10*t) + 0.5*std::sin(2*M_PI*100*t);
    }
    SignalStore store; store.add(makeSeries("X", vals, fs));
    FormulaEngine engine(store); QString err;
    ASSERT_TRUE(engine.evaluate("M  = RFFTmag(X)",           &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("P  = RFFTphase(X)",         &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("MK = BandKeep(M, 90, 110)", &err)) << err.toStdString();
    ASSERT_TRUE(engine.evaluate("Y  = revertFFT(MK, P)",     &err)) << err.toStdString();
    auto y = store.get("Y")->readAsDouble();
    double mx = 0; for (double v : y) mx = std::max(mx, std::abs(v));
    EXPECT_NEAR(mx, 0.5, 0.02);     // only the 100 Hz tone kept (amp 0.5)
    // BandKeep and BandZero partition the spectrum: keep + zero == original.
    ASSERT_TRUE(engine.evaluate("MZ = BandZero(M, 90, 110)", &err)) << err.toStdString();
    auto m  = store.get("M")->readAsDouble();
    auto mk = store.get("MK")->readAsDouble();
    auto mz = store.get("MZ")->readAsDouble();
    ASSERT_EQ(mk.size(), m.size()); ASSERT_EQ(mz.size(), m.size());
    bool partition = true;
    for (std::size_t k = 0; k < m.size(); ++k)
        if (std::abs(mk[k] + mz[k] - m[k]) > 1e-12) partition = false;
    EXPECT_TRUE(partition);
}
