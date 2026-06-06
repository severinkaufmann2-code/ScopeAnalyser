#include "scope/analyser/FormulaEngine.h"
#include "scope/analyser/FunctionRegistry.h"
#include "scope/core/Signal.h"
#include "scope/core/SignalStore.h"

#include <gtest/gtest.h>

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
