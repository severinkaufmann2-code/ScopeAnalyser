// Numerical-correctness tests for the Analyser's function library, checked
// against analytic ground truth (closed-form integrals/derivatives, exact
// window membership, exact FFT bin math on power-of-two grids, …). These
// complement test_formula_engine.cpp, which focuses on plumbing and filter
// shapes; here the point is "the numbers the user reads are right".
//
// Tests with the DISABLED_ prefix pin DESIRED behaviour for known issues
// (documented in _PlansAndExecution/20260612_1005_Plans.md). They are kept
// out of the green suite until a fix is approved; run them with
//   scope_tests --gtest_also_run_disabled_tests --gtest_filter='*KnownIssue*'
// to reproduce the findings.

#include "scope/analyser/FormulaEngine.h"
#include "scope/core/Signal.h"
#include "scope/core/SignalStore.h"

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <vector>

using namespace scope::core;
using namespace scope::analyser;

namespace {

// Values at fs Hz starting at startNs (fs divides 1e9 in tests → exact ts).
std::shared_ptr<Signal> seriesAt(QString name, const std::vector<double>& vals,
                                 double fs, TimestampNs startNs = 0) {
    Signal::Meta m;
    m.name = std::move(name);
    m.dataType = DataType::Float64;
    m.sampleRateHz = fs;
    auto sig = std::make_shared<Signal>(m);
    std::vector<TimestampNs> ts(vals.size());
    const double dt = 1.0 / fs;
    for (std::size_t i = 0; i < vals.size(); ++i)
        ts[i] = startNs + static_cast<TimestampNs>(std::llround(i * dt * 1e9));
    sig->append(ts.data(), reinterpret_cast<const std::byte*>(vals.data()),
                vals.size());
    return sig;
}

std::shared_ptr<Signal> sineAt(QString name, std::size_t n, double fs,
                               double freq, double amp,
                               TimestampNs startNs = 0, double phase = 0.0) {
    std::vector<double> vs(n);
    for (std::size_t i = 0; i < n; ++i)
        vs[i] = amp * std::sin(2.0 * M_PI * freq * i / fs + phase);
    return seriesAt(std::move(name), vs, fs, startNs);
}

std::vector<double> eval(SignalStore& store, FormulaEngine& engine,
                         const QString& line, const QString& outName) {
    QString err;
    EXPECT_TRUE(engine.evaluate(line, &err))
        << line.toStdString() << " failed: " << err.toStdString();
    auto out = store.get(outName);
    EXPECT_TRUE(out) << outName.toStdString() << " missing from store";
    return out ? out->readAsDouble() : std::vector<double>{};
}

}  // namespace

// ===========================================================================
// Calculus
// ===========================================================================

// ∫ A·sin(2πft) dt = A/(2πf) · (1 − cos(2πft)). Trapezoidal error for this
// integrand is bounded by Σ dt³/12·|y''| ≈ A(2πf)²·dt²·T/12 ≈ 1.6e-4 here.
TEST(FunctionCorrectness, IntegralOfSineMatchesClosedForm) {
    SignalStore store;
    const double fs = 1000.0, f = 5.0, A = 2.0;
    const std::size_t n = 1000;
    store.add(sineAt("A", n, fs, f, A));
    FormulaEngine engine(store);
    const auto out = eval(store, engine, "I = Integral(A)", "I");
    ASSERT_EQ(out.size(), n);
    const double w = 2.0 * M_PI * f;
    for (std::size_t i = 0; i < n; i += 37) {
        const double t = i / fs;
        const double expect = A / w * (1.0 - std::cos(w * t));
        EXPECT_NEAR(out[i], expect, 5e-4)
            << "integral wrong at t=" << t << " s";
    }
}

// Central differences are exact for polynomials up to degree 2 on a uniform
// grid: d/dt (t²) = 2t with no truncation error in the interior.
TEST(FunctionCorrectness, DerivativeOfQuadraticExactInInterior) {
    SignalStore store;
    const double fs = 100.0;
    const std::size_t n = 51;
    std::vector<double> vs(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = i / fs;
        vs[i] = t * t;
    }
    store.add(seriesAt("A", vs, fs));
    FormulaEngine engine(store);
    const auto out = eval(store, engine, "D = Derivative(A)", "D");
    ASSERT_EQ(out.size(), n);
    for (std::size_t i = 1; i + 1 < n; ++i) {
        const double t = i / fs;
        EXPECT_NEAR(out[i], 2.0 * t, 1e-9) << "interior sample " << i;
    }
    // Boundaries use one-sided differences: d/dt(t²) at 0 reads dt (=1/fs),
    // at the end 2t_end − dt. That O(dt) edge error is inherent to the method.
    EXPECT_NEAR(out.front(), 1.0 / fs, 1e-9);
    EXPECT_NEAR(out.back(), 2.0 * (n - 1) / fs - 1.0 / fs, 1e-9);
}

TEST(FunctionCorrectness, DerivativeOfSineMatchesCosine) {
    SignalStore store;
    const double fs = 1000.0, f = 2.0, A = 1.0;
    const std::size_t n = 1000;
    store.add(sineAt("A", n, fs, f, A));
    FormulaEngine engine(store);
    const auto out = eval(store, engine, "D = Derivative(A)", "D");
    ASSERT_EQ(out.size(), n);
    const double w = 2.0 * M_PI * f;
    for (std::size_t i = 1; i + 1 < n; i += 41) {
        const double t = i / fs;
        EXPECT_NEAR(out[i], A * w * std::cos(w * t), 1e-3)
            << "derivative wrong at t=" << t;
    }
}

// ===========================================================================
// Rolling statistics
// ===========================================================================

// Window membership is inclusive: t[i] − t[lo] <= window. At 1 Hz with a 2 s
// window each sample averages itself and up to two predecessors.
TEST(FunctionCorrectness, RollingMeanWindowMembershipExact) {
    SignalStore store;
    store.add(seriesAt("A", {1, 2, 3, 4, 5}, 1.0));
    FormulaEngine engine(store);
    const auto out = eval(store, engine, "M = Mean(A, 2)", "M");
    ASSERT_EQ(out.size(), 5u);
    const double expect[5] = {1.0, 1.5, 2.0, 3.0, 4.0};
    for (int i = 0; i < 5; ++i)
        EXPECT_DOUBLE_EQ(out[i], expect[i]) << "rolling mean sample " << i;
}

// Over an integer number of full cycles Σ sin² = N/2 exactly, so the final
// rolling-RMS sample over the whole capture is A/√2.
TEST(FunctionCorrectness, RmsOfFullCyclesIsAmplitudeOverSqrt2) {
    SignalStore store;
    const double fs = 1000.0, f = 10.0, A = 3.0;
    const std::size_t n = 1000;   // exactly 10 cycles
    store.add(sineAt("A", n, fs, f, A));
    FormulaEngine engine(store);
    const auto out = eval(store, engine, "R = RMS(A, 10)", "R");
    ASSERT_EQ(out.size(), n);
    EXPECT_NEAR(out.back(), A / std::sqrt(2.0), 1e-9);
}

TEST(FunctionCorrectness, RmsOfConstantIsTheConstant) {
    SignalStore store;
    store.add(seriesAt("A", std::vector<double>(64, 7.25), 100.0));
    FormulaEngine engine(store);
    const auto out = eval(store, engine, "R = RMS(A, 0.2)", "R");
    for (double v : out) EXPECT_DOUBLE_EQ(v, 7.25);
}

// ===========================================================================
// Elementwise functions and operators
// ===========================================================================

TEST(FunctionCorrectness, TrigInversesRoundTrip) {
    SignalStore store;
    store.add(seriesAt("A", {-1.2, -0.5, 0.0, 0.7, 1.4}, 10.0));
    FormulaEngine engine(store);
    const auto asinSin = eval(store, engine, "B = Asin(Sin(A))", "B");
    const auto atanTan = eval(store, engine, "C = Atan(Tan(A))", "C");
    const auto acosCos = eval(store, engine, "D = Acos(Cos(A))", "D");
    const double in[5] = {-1.2, -0.5, 0.0, 0.7, 1.4};
    for (int i = 0; i < 5; ++i) {
        EXPECT_NEAR(asinSin[i], in[i], 1e-12);          // |x| < π/2
        EXPECT_NEAR(atanTan[i], in[i], 1e-12);          // |x| < π/2
        EXPECT_NEAR(acosCos[i], std::abs(in[i]), 1e-12); // acos lands in [0, π]
    }
}

TEST(FunctionCorrectness, SqrtAbsExpLogIdentities) {
    SignalStore store;
    store.add(seriesAt("A", {-4.0, -0.25, 0.5, 9.0}, 10.0));
    FormulaEngine engine(store);
    const auto sq  = eval(store, engine, "B = Sqrt(A * A)", "B");
    const auto ab  = eval(store, engine, "C = Abs(A)", "C");
    const auto rt  = eval(store, engine, "D = Log(Exp(A))", "D");
    const double in[4] = {-4.0, -0.25, 0.5, 9.0};
    for (int i = 0; i < 4; ++i) {
        EXPECT_NEAR(sq[i], std::abs(in[i]), 1e-12);
        EXPECT_DOUBLE_EQ(ab[i], std::abs(in[i]));
        EXPECT_NEAR(rt[i], in[i], 1e-12);
    }
}

TEST(FunctionCorrectness, PowerWithSignalExponentSameGrid) {
    SignalStore store;
    store.add(seriesAt("B", {2.0, 3.0, 4.0}, 10.0));
    store.add(seriesAt("E", {2.0, 3.0, 0.5}, 10.0));
    FormulaEngine engine(store);
    const auto out = eval(store, engine, "P = Power(B, E)", "P");
    ASSERT_EQ(out.size(), 3u);
    EXPECT_DOUBLE_EQ(out[0], 4.0);
    EXPECT_DOUBLE_EQ(out[1], 27.0);
    EXPECT_DOUBLE_EQ(out[2], 2.0);
}

TEST(FunctionCorrectness, ShiftMovesTimestampsExactlyValuesUntouched) {
    SignalStore store;
    store.add(seriesAt("A", {1.0, 2.0, 3.0}, 1000.0, /*startNs=*/5'000'000'000));
    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("S = Shift(A, 0.25)", &err)) << err.toStdString();
    auto s = store.get("S");
    ASSERT_TRUE(s);
    auto view = s->snapshotForRead();
    ASSERT_EQ(view.count, 3u);
    EXPECT_EQ(view.timestamps[0], 5'250'000'000LL);
    EXPECT_EQ(view.timestamps[1], 5'251'000'000LL);
    EXPECT_EQ(view.timestamps[2], 5'252'000'000LL);
    const auto vs = s->readAsDouble();
    EXPECT_DOUBLE_EQ(vs[0], 1.0);
    EXPECT_DOUBLE_EQ(vs[2], 3.0);

    ASSERT_TRUE(engine.evaluate("N = Shift(A, -1)", &err)) << err.toStdString();
    auto nview = store.get("N")->snapshotForRead();
    EXPECT_EQ(nview.timestamps[0], 4'000'000'000LL);
}

// fmod semantics: result carries the dividend's sign. Users comparing against
// a PLC MOD (also truncated) get matching values.
TEST(FunctionCorrectness, ModSignFollowsDividend) {
    SignalStore store;
    store.add(seriesAt("A", {-3.5, -1.0, 1.0, 3.5}, 10.0));
    FormulaEngine engine(store);
    const auto out = eval(store, engine, "M = Mod(A, 2)", "M");
    EXPECT_DOUBLE_EQ(out[0], -1.5);
    EXPECT_DOUBLE_EQ(out[1], -1.0);
    EXPECT_DOUBLE_EQ(out[2], 1.0);
    EXPECT_DOUBLE_EQ(out[3], 1.5);
}

// CHARACTERIZATION of current behaviour, not an endorsement: x/0 currently
// yields 0 (silently), which can fabricate a plausible-looking value. See
// finding B5 in _PlansAndExecution/20260612_1005_Plans.md — pending a
// decision, this test pins the status quo so any change is deliberate.
TEST(FunctionCorrectness, DivisionByZeroCurrentlyYieldsZero_Characterization) {
    SignalStore store;
    store.add(seriesAt("A", {6.0, 6.0}, 10.0));
    store.add(seriesAt("B", {2.0, 0.0}, 10.0));
    FormulaEngine engine(store);
    const auto out = eval(store, engine, "Q = A / B", "Q");
    ASSERT_EQ(out.size(), 2u);
    EXPECT_DOUBLE_EQ(out[0], 3.0);
    EXPECT_DOUBLE_EQ(out[1], 0.0);   // ← fabricated 0, not NaN/inf
}

TEST(FunctionCorrectness, OperatorPrecedenceAndUnaryMinus) {
    SignalStore store;
    store.add(seriesAt("A", {1.0, 2.0, 3.0}, 10.0));
    FormulaEngine engine(store);
    const auto c = eval(store, engine, "C = -A + 2 * 3", "C");   // 6 − A
    const auto d = eval(store, engine, "D = (A + 1) * 2", "D");
    const auto e = eval(store, engine, "E = 10 - 4 - 3", "E");   // left-assoc
    for (int i = 0; i < 3; ++i) {
        EXPECT_DOUBLE_EQ(c[i], 6.0 - (i + 1.0));
        EXPECT_DOUBLE_EQ(d[i], (i + 2.0) * 2.0);
    }
    EXPECT_DOUBLE_EQ(e[0], 3.0);
}

TEST(FunctionCorrectness, ScientificNotationLiterals) {
    SignalStore store;
    store.add(seriesAt("A", {1000.0, 2000.0}, 10.0));
    FormulaEngine engine(store);
    const auto out = eval(store, engine, "C = A * 1e-3 + 2.5E2", "C");
    EXPECT_DOUBLE_EQ(out[0], 251.0);
    EXPECT_DOUBLE_EQ(out[1], 252.0);
}

// ===========================================================================
// Spectrum functions
// ===========================================================================

// DC must read the true mean — with the rectangular window and (regression
// guard) with the default Hann window, whose coherent-gain correction also
// applies at bin 0.
TEST(FunctionCorrectness, FftDcBinReadsTrueMean) {
    SignalStore store;
    store.add(seriesAt("A", std::vector<double>(1024, 3.0), 1024.0));
    FormulaEngine engine(store);
    const auto rect = eval(store, engine, "R = FFT(A, 2)", "R");
    ASSERT_GE(rect.size(), 1u);
    EXPECT_NEAR(rect[0], 3.0, 1e-9);
    const auto hann = eval(store, engine, "H = FFT(A)", "H");
    EXPECT_NEAR(hann[0], 3.0, 1e-9);
}

// On a grid whose sample period is exact in integer nanoseconds (1 kHz →
// 1 ms), the FFT's frequency axis must be exact: N = 1024, df = 1000/1024 =
// 0.9765625 Hz, bin k at exactly k × 976562500 ns.
//
// (Deliberately NOT tested at fs = 1024 Hz: its 976562.5 ns period can't be
// stored in integer-ns timestamps, so the FFT re-derives fs from the rounded
// grid with a ~5e-10 relative error — physically meaningless, but it would
// make an exact-equality assertion lie about the cause.)
TEST(FunctionCorrectness, FftBinFrequenciesAreExact) {
    SignalStore store;
    store.add(sineAt("A", 1000, 1000.0, 100.0, 1.0));
    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("F = FFT(A)", &err)) << err.toStdString();
    auto f = store.get("F");
    ASSERT_TRUE(f);
    auto view = f->snapshotForRead();
    ASSERT_EQ(view.count, 513u);   // N/2 + 1
    for (std::size_t k = 0; k < view.count; k += 64)
        EXPECT_EQ(view.timestamps[k],
                  static_cast<TimestampNs>(k) * 976'562'500LL)
            << "frequency bin " << k << " misplaced";
    EXPECT_EQ(f->meta().domain, Signal::Domain::Frequency);
}

// On-bin tones with a power-of-two sample count: cos → phase 0, sin → −π/2
// at the tone bin. This is the invariant the RFFTmag/RFFTphase → edit →
// revertFFT workflow rests on.
TEST(FunctionCorrectness, RfftPhaseOfCosineAndSine) {
    SignalStore store;
    const double fs = 1024.0;
    const std::size_t n = 1024;
    const double f = 32.0;          // exact bin 32
    std::vector<double> cosv(n), sinv(n);
    for (std::size_t i = 0; i < n; ++i) {
        cosv[i] = std::cos(2.0 * M_PI * f * i / fs);
        sinv[i] = std::sin(2.0 * M_PI * f * i / fs);
    }
    store.add(seriesAt("C", cosv, fs));
    store.add(seriesAt("S", sinv, fs));
    FormulaEngine engine(store);
    const auto pc = eval(store, engine, "PC = RFFTphase(C)", "PC");
    const auto ps = eval(store, engine, "PS = RFFTphase(S)", "PS");
    ASSERT_GT(pc.size(), 32u);
    EXPECT_NEAR(pc[32], 0.0, 1e-6) << "cosine phase at its bin";
    EXPECT_NEAR(ps[32], -M_PI / 2.0, 1e-6) << "sine phase at its bin";
    // And the magnitude there is N/2 × amplitude (raw, uncalibrated).
    const auto mc = eval(store, engine, "MC = RFFTmag(C)", "MC");
    EXPECT_NEAR(mc[32], n / 2.0, 1e-6);
}

// Keeping the band around one of two tones must return exactly that tone:
// power-of-two length + on-bin tones make the FFT round trip numerically
// exact (~1e-9), so the kept tone's amplitude is preserved and the cut one
// vanishes.
TEST(FunctionCorrectness, FftKeepIsolatesToneWithCorrectAmplitude) {
    SignalStore store;
    const double fs = 1024.0;
    const std::size_t n = 1024;
    std::vector<double> two(n);
    for (std::size_t i = 0; i < n; ++i) {
        two[i] = 0.8 * std::sin(2.0 * M_PI * 16.0 * i / fs)
               + 0.5 * std::sin(2.0 * M_PI * 128.0 * i / fs);
    }
    store.add(seriesAt("A", two, fs));
    FormulaEngine engine(store);
    const auto out = eval(store, engine, "K = FFTKeep(A, 100, 200)", "K");
    ASSERT_EQ(out.size(), n);
    for (std::size_t i = 0; i < n; i += 13) {
        const double expect = 0.5 * std::sin(2.0 * M_PI * 128.0 * i / fs);
        EXPECT_NEAR(out[i], expect, 1e-9) << "sample " << i;
    }
}

// ===========================================================================
// Known issues (DISABLED) — desired behaviour, awaiting an approved fix.
// Reproduce with: --gtest_also_run_disabled_tests
// See _PlansAndExecution/20260612_1005_Plans.md for the findings + fix plan.
// ===========================================================================

// Finding B2: Slice compares the user's seconds against ABSOLUTE timestamps.
// The chart shows seconds relative to the signal's start, so for recorder
// data (epoch-based nanoseconds) Slice(A, 0.2, 0.5) silently keeps nothing.
// Desired: t_start/t_end are relative to the signal's first sample.
TEST(FunctionCorrectnessKnownIssue, DISABLED_SliceSecondsAreRelativeToSignalStart) {
    SignalStore store;
    const TimestampNs t0 = 1'750'000'000'000'000'000LL;   // epoch-style start
    store.add(seriesAt("A", {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}, 10.0, t0));
    FormulaEngine engine(store);
    const auto out = eval(store, engine, "S = Slice(A, 0.2, 0.5)", "S");
    ASSERT_EQ(out.size(), 4u) << "expected samples at +0.2/0.3/0.4/0.5 s";
    EXPECT_DOUBLE_EQ(out.front(), 2.0);
    EXPECT_DOUBLE_EQ(out.back(), 5.0);
}

// Finding B2 (same class): BandZero/BandKeep interpret lo/hi as absolute
// seconds on time-domain signals. Desired: relative to the signal's start.
TEST(FunctionCorrectnessKnownIssue, DISABLED_BandKeepTimeWindowRelativeToSignalStart) {
    SignalStore store;
    const TimestampNs t0 = 1'750'000'000'000'000'000LL;
    store.add(seriesAt("A", {1, 1, 1, 1, 1, 1, 1, 1, 1, 1}, 10.0, t0));
    FormulaEngine engine(store);
    const auto out = eval(store, engine, "K = BandKeep(A, 0.2, 0.5)", "K");
    ASSERT_EQ(out.size(), 10u);
    EXPECT_DOUBLE_EQ(out[0], 0.0);   // outside the kept window → zeroed
    EXPECT_DOUBLE_EQ(out[3], 1.0);   // inside [0.2, 0.5] after the start
    EXPECT_DOUBLE_EQ(out[9], 0.0);
}

// Finding B3: revertFFT rebuilds the waveform starting at t = 0 instead of
// the source signal's first timestamp, so the reconstruction lands ~55 years
// away from a recorded original on the shared time axis. Desired: the
// RFFTmag/RFFTphase → revertFFT round trip preserves the start time.
TEST(FunctionCorrectnessKnownIssue, DISABLED_RevertFftPreservesStartTime) {
    SignalStore store;
    const TimestampNs t0 = 5'000'000'000LL;               // starts at 5 s
    store.add(sineAt("A", 1024, 1024.0, 32.0, 1.0, t0));
    FormulaEngine engine(store);
    QString err;
    ASSERT_TRUE(engine.evaluate("R = revertFFT(RFFTmag(A), RFFTphase(A))", &err))
        << err.toStdString();
    auto r = store.get("R");
    ASSERT_TRUE(r);
    auto view = r->snapshotForRead();
    ASSERT_GE(view.count, 2u);
    EXPECT_EQ(view.timestamps[0], t0)
        << "reconstruction must start where the original started";
}
