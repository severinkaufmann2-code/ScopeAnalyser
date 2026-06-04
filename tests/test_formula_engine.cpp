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
    for (std::size_t i = 1; i + 1 < ovs.size(); ++i) {
        EXPECT_NEAR(ovs[i], 10.0, 1e-9);
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
