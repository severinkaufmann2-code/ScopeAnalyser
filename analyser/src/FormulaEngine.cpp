#include "scope/analyser/FormulaEngine.h"
#include "scope/analyser/FunctionRegistry.h"
#include "scope/core/Signal.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <variant>
#include <vector>

namespace scope::analyser {

using scope::core::Signal;
using scope::core::DataType;
using scope::core::TimestampNs;

namespace {

// ---- Tokeniser ---------------------------------------------------------
enum class Tok { End, Ident, Number, Plus, Minus, Star, Slash,
                 LParen, RParen, Comma, Assign };

struct Token {
    Tok    kind{Tok::End};
    QString text;
    double  value{0.0};
    int     pos{0};
};

struct Lexer {
    QString src;
    int p{0};

    explicit Lexer(QString s) : src(std::move(s)) {}

    void skipWs() { while (p < src.size() && src[p].isSpace()) ++p; }

    Token next() {
        skipWs();
        if (p >= src.size()) return {Tok::End, {}, 0.0, p};
        const int start = p;
        QChar c = src[p];
        if (c.isLetter() || c == '_') {
            while (p < src.size() && (src[p].isLetterOrNumber() || src[p] == '_' || src[p] == '.'))
                ++p;
            return {Tok::Ident, src.mid(start, p - start), 0.0, start};
        }
        if (c.isDigit() || c == '.') {
            while (p < src.size() && (src[p].isDigit() || src[p] == '.' || src[p] == 'e' || src[p] == 'E'
                                      || (p > start && (src[p] == '+' || src[p] == '-')
                                          && (src[p-1] == 'e' || src[p-1] == 'E'))))
                ++p;
            bool ok = false;
            double v = src.mid(start, p - start).toDouble(&ok);
            return {Tok::Number, src.mid(start, p - start), ok ? v : 0.0, start};
        }
        ++p;
        switch (c.unicode()) {
            case '+': return {Tok::Plus,   "+", 0, start};
            case '-': return {Tok::Minus,  "-", 0, start};
            case '*': return {Tok::Star,   "*", 0, start};
            case '/': return {Tok::Slash,  "/", 0, start};
            case '(': return {Tok::LParen, "(", 0, start};
            case ')': return {Tok::RParen, ")", 0, start};
            case ',': return {Tok::Comma,  ",", 0, start};
            case '=': return {Tok::Assign, "=", 0, start};
            default:  return {Tok::End,    QString(c), 0, start};
        }
    }

    Token peek() {
        const int save = p;
        Token t = next();
        p = save;
        return t;
    }
};

// ---- AST ----------------------------------------------------------------
// Evaluator returns shared_ptr<Signal>. Constants are produced as a
// single-sample signal that downstream functions detect and lift back to
// a scalar via asScalar() in the function impls.
struct EvalCtx {
    scope::core::SignalStore& store;
    QString*                  err{nullptr};
};

bool setErr(EvalCtx& ctx, const QString& m) {
    if (ctx.err) *ctx.err = m;
    return false;
}

std::shared_ptr<Signal> constantSignal(double v) {
    Signal::Meta m;
    m.name = "const";
    m.dataType = DataType::Float64;
    auto sig = std::make_shared<Signal>(m);
    TimestampNs t = 0;
    sig->append(&t, reinterpret_cast<const std::byte*>(&v), 1);
    return sig;
}

bool isConstant(const std::shared_ptr<Signal>& s) {
    if (!s) return false;
    auto view = s->snapshotForRead();
    return view.count == 1 && view.timestamps[0] == 0
        && s->meta().sourceSymbol.isEmpty();
}

// Linear-interpolate `vals` at time `t`, advancing `cursor` (monotone search
// for sorted query times). Returns value at the nearest endpoint if `t` is
// outside the range; callers should clip to [first, last] beforehand.
double interpAt(const TimestampNs* ts, const std::vector<double>& vals,
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

// Elementwise binary op on two signals. Three paths:
//   * One operand is a constant → broadcast across the other.
//   * Same length AND first/last timestamps match → assume same grid (fast path).
//   * Otherwise → intersection-of-time-ranges, sample on the operand with more
//     samples in that range, linear-interpolate the other.
template <typename Op>
std::shared_ptr<Signal> elementwiseBinary(const std::shared_ptr<Signal>& a,
                                          const std::shared_ptr<Signal>& b,
                                          Op op,
                                          EvalCtx& ctx,
                                          const char* opName) {
    const bool aConst = isConstant(a);
    const bool bConst = isConstant(b);
    auto av = a->readAsDouble();
    auto bv = b->readAsDouble();
    auto avView = a->snapshotForRead();
    auto bvView = b->snapshotForRead();

    // Inherit the domain from the non-constant operand(s) so arithmetic on
    // frequency-domain signals (e.g. FFT magnitude * 2) stays in the
    // Frequency view instead of silently defaulting back to Time.
    using Domain = Signal::Domain;
    const Domain domA = a->meta().domain;
    const Domain domB = b->meta().domain;
    const Domain outDomain =
        aConst ? domB
      : bConst ? domA
      : (domA == Domain::Frequency || domB == Domain::Frequency)
            ? Domain::Frequency : Domain::Time;

    auto makeSig = [&](const std::vector<TimestampNs>& ts,
                       const std::vector<double>& vs) {
        Signal::Meta m;
        m.dataType = DataType::Float64;
        m.name = opName;
        m.domain = outDomain;
        auto s = std::make_shared<Signal>(m);
        s->append(ts.data(),
                  reinterpret_cast<const std::byte*>(vs.data()),
                  vs.size());
        return s;
    };

    // ---- Path 1: constant broadcast ----
    if (aConst || bConst) {
        const std::size_t n = aConst ? bvView.count : avView.count;
        std::vector<double> out(n);
        const TimestampNs* ts = aConst ? bvView.timestamps : avView.timestamps;
        for (std::size_t i = 0; i < n; ++i) {
            const double va = aConst ? av[0] : av[i];
            const double vb = bConst ? bv[0] : bv[i];
            out[i] = op(va, vb);
        }
        std::vector<TimestampNs> outTs(ts, ts + n);
        return makeSig(outTs, out);
    }

    if (avView.count == 0 || bvView.count == 0) {
        setErr(ctx, QString("'%1': empty operand").arg(opName));
        return nullptr;
    }

    // ---- Path 2: same grid (fast path) ----
    if (avView.count == bvView.count &&
        avView.timestamps[0] == bvView.timestamps[0] &&
        avView.timestamps[avView.count - 1] == bvView.timestamps[bvView.count - 1]) {
        const std::size_t n = avView.count;
        std::vector<double> out(n);
        for (std::size_t i = 0; i < n; ++i) out[i] = op(av[i], bv[i]);
        std::vector<TimestampNs> outTs(avView.timestamps, avView.timestamps + n);
        return makeSig(outTs, out);
    }

    // ---- Path 3: different grids → resample on intersection ----
    const TimestampNs tLo = std::max(avView.timestamps[0], bvView.timestamps[0]);
    const TimestampNs tHi = std::min(avView.timestamps[avView.count - 1],
                                     bvView.timestamps[bvView.count - 1]);
    if (tLo > tHi) {
        setErr(ctx, QString("'%1': signals don't overlap in time").arg(opName));
        return nullptr;
    }
    auto countInRange = [&](const Signal::ReadView& v) {
        std::size_t c = 0;
        for (std::size_t i = 0; i < v.count; ++i)
            if (v.timestamps[i] >= tLo && v.timestamps[i] <= tHi) ++c;
        return c;
    };
    const std::size_t aN = countInRange(avView);
    const std::size_t bN = countInRange(bvView);
    const bool useA = aN >= bN;
    const auto& gridView = useA ? avView : bvView;
    const auto& gridVals = useA ? av     : bv;
    const auto& otherView = useA ? bvView : avView;
    const auto& otherVals = useA ? bv     : av;

    std::vector<TimestampNs> outTs;
    std::vector<double>      outVals;
    outTs.reserve(useA ? aN : bN);
    outVals.reserve(useA ? aN : bN);

    std::size_t cursor = 0;
    for (std::size_t i = 0; i < gridView.count; ++i) {
        const TimestampNs t = gridView.timestamps[i];
        if (t < tLo || t > tHi) continue;
        const double here  = gridVals[i];
        const double there = interpAt(otherView.timestamps, otherVals,
                                      otherView.count, t, cursor);
        const double va = useA ? here  : there;
        const double vb = useA ? there : here;
        outTs.push_back(t);
        outVals.push_back(op(va, vb));
    }
    return makeSig(outTs, outVals);
}

// ---- Parser -------------------------------------------------------------
struct Parser {
    Lexer  lex;
    Token  cur;
    EvalCtx& ctx;

    Parser(QString src, EvalCtx& c) : lex(std::move(src)), ctx(c) { cur = lex.next(); }

    bool accept(Tok k) {
        if (cur.kind == k) { cur = lex.next(); return true; }
        return false;
    }

    bool expect(Tok k, const char* msg) {
        if (!accept(k)) {
            setErr(ctx, QString("expected %1 at position %2 (got '%3')")
                            .arg(msg).arg(cur.pos).arg(cur.text));
            return false;
        }
        return true;
    }

    std::shared_ptr<Signal> parseExpr()   { return parseAdditive(); }

    std::shared_ptr<Signal> parseAdditive() {
        auto left = parseMultiplicative();
        if (!left) return nullptr;
        while (cur.kind == Tok::Plus || cur.kind == Tok::Minus) {
            const auto op = cur.kind;
            cur = lex.next();
            auto right = parseMultiplicative();
            if (!right) return nullptr;
            left = (op == Tok::Plus)
                ? elementwiseBinary(left, right, [](double a, double b){ return a + b; }, ctx, "+")
                : elementwiseBinary(left, right, [](double a, double b){ return a - b; }, ctx, "-");
            if (!left) return nullptr;
        }
        return left;
    }

    std::shared_ptr<Signal> parseMultiplicative() {
        auto left = parseUnary();
        if (!left) return nullptr;
        while (cur.kind == Tok::Star || cur.kind == Tok::Slash) {
            const auto op = cur.kind;
            cur = lex.next();
            auto right = parseUnary();
            if (!right) return nullptr;
            left = (op == Tok::Star)
                ? elementwiseBinary(left, right, [](double a, double b){ return a * b; }, ctx, "*")
                : elementwiseBinary(left, right, [](double a, double b){ return b != 0 ? a / b : 0; }, ctx, "/");
            if (!left) return nullptr;
        }
        return left;
    }

    std::shared_ptr<Signal> parseUnary() {
        if (cur.kind == Tok::Minus) {
            cur = lex.next();
            auto inner = parseUnary();
            if (!inner) return nullptr;
            auto neg = constantSignal(-1.0);
            return elementwiseBinary(neg, inner, [](double a, double b){ return a * b; }, ctx, "negate");
        }
        if (cur.kind == Tok::Plus) {
            cur = lex.next();
            return parseUnary();
        }
        return parsePrimary();
    }

    std::shared_ptr<Signal> parsePrimary() {
        if (cur.kind == Tok::Number) {
            auto sig = constantSignal(cur.value);
            cur = lex.next();
            return sig;
        }
        if (cur.kind == Tok::LParen) {
            cur = lex.next();
            auto e = parseExpr();
            if (!e) return nullptr;
            if (!expect(Tok::RParen, ")")) return nullptr;
            return e;
        }
        if (cur.kind == Tok::Ident) {
            QString name = cur.text;
            int pos = cur.pos;
            cur = lex.next();
            if (cur.kind == Tok::LParen) {
                cur = lex.next();

                // ---- filtfilt(FilterName, signal, params...) : zero-phase ----
                // Higher-order special form. The first argument is a *filter
                // function name*, not data (a bare ident would otherwise be a
                // channel ref), so we capture it, evaluate the remaining args
                // as that filter's args, then run it forward → Reverse →
                // forward → Reverse, which cancels the phase shift.
                if (name == "filtfilt") {
                    if (cur.kind != Tok::Ident) {
                        setErr(ctx, "filtfilt: first argument must be a filter "
                                    "name, e.g. filtfilt(Filter, speed, 0.05)");
                        return nullptr;
                    }
                    const QString innerName = cur.text;
                    cur = lex.next();
                    if (!expect(Tok::Comma,
                                ", after the filter name in filtfilt")) return nullptr;
                    FunctionArgs ffArgs;
                    for (;;) {
                        auto a = parseExpr();
                        if (!a) return nullptr;
                        ffArgs.push_back(std::move(a));
                        if (cur.kind == Tok::Comma) { cur = lex.next(); continue; }
                        break;
                    }
                    if (!expect(Tok::RParen, ") in filtfilt")) return nullptr;

                    auto isFilterable = [](const QString& fn) {
                        return fn == "Filter"      || fn == "FilterHP"
                            || fn == "Butterworth" || fn == "Cheby1"
                            || fn == "Cheby2"      || fn == "Mean"
                            || fn == "RMS";
                    };
                    if (!isFilterable(innerName)) {
                        setErr(ctx, QString("filtfilt: '%1' can't be applied "
                            "zero-phase (use Filter, FilterHP, Butterworth, "
                            "Cheby1, Cheby2, Mean or RMS)").arg(innerName));
                        return nullptr;
                    }
                    auto* inner = FunctionRegistry::instance().find(innerName);
                    auto* rev   = FunctionRegistry::instance().find("Reverse");
                    if (!inner || !rev) {
                        setErr(ctx, QString("filtfilt: unknown filter '%1'")
                                        .arg(innerName));
                        return nullptr;
                    }
                    if ((int)ffArgs.size() < inner->minArgs ||
                        (inner->maxArgs > 0 && (int)ffArgs.size() > inner->maxArgs)) {
                        setErr(ctx, QString("filtfilt: %1 expects %2..%3 args, "
                            "got %4").arg(innerName).arg(inner->minArgs)
                            .arg(inner->maxArgs).arg(ffArgs.size()));
                        return nullptr;
                    }
                    QString ferr;
                    auto y1 = inner->impl(ffArgs, &ferr);           // forward
                    if (!y1) { setErr(ctx, ferr); return nullptr; }
                    auto r1 = rev->impl({y1}, &ferr);              // reverse
                    if (!r1) { setErr(ctx, ferr); return nullptr; }
                    ffArgs[0] = r1;
                    auto y2 = inner->impl(ffArgs, &ferr);          // backward
                    if (!y2) { setErr(ctx, ferr); return nullptr; }
                    auto out = rev->impl({y2}, &ferr);             // reverse back
                    if (!out) { setErr(ctx, ferr); return nullptr; }
                    return out;
                }

                FunctionArgs args;
                if (cur.kind != Tok::RParen) {
                    for (;;) {
                        auto a = parseExpr();
                        if (!a) return nullptr;
                        args.push_back(std::move(a));
                        if (cur.kind == Tok::Comma) { cur = lex.next(); continue; }
                        break;
                    }
                }
                if (!expect(Tok::RParen, ") in function call")) return nullptr;
                auto* desc = FunctionRegistry::instance().find(name);
                if (!desc) {
                    setErr(ctx, QString("unknown function '%1' at position %2").arg(name).arg(pos));
                    return nullptr;
                }
                if ((int)args.size() < desc->minArgs ||
                    (desc->maxArgs > 0 && (int)args.size() > desc->maxArgs)) {
                    setErr(ctx, QString("function '%1' expects %2..%3 args, got %4")
                                    .arg(name).arg(desc->minArgs).arg(desc->maxArgs).arg(args.size()));
                    return nullptr;
                }
                QString err;
                auto out = desc->impl(args, &err);
                if (!out) { setErr(ctx, err); return nullptr; }
                return out;
            }
            // bare identifier: channel ref
            auto sig = ctx.store.get(name);
            if (!sig) {
                setErr(ctx, QString("unknown channel '%1' at position %2").arg(name).arg(pos));
                return nullptr;
            }
            return sig;
        }
        setErr(ctx, QString("unexpected token '%1' at position %2").arg(cur.text).arg(cur.pos));
        return nullptr;
    }
};

}  // namespace

struct FormulaEngine::Impl {
    scope::core::SignalStore& store;
    // Derived channel name -> expression (RHS only). `order` keeps the
    // definition sequence so recomputeAll evaluates dependencies first in
    // the common case; it also falls back to a multi-pass loop.
    QHash<QString, QString> formulas;
    QStringList             order;
    bool                    recomputing{false};
    QMetaObject::Connection removedConn;

    explicit Impl(scope::core::SignalStore& s) : store(s) {}

    void setFormula(const QString& name, const QString& expr) {
        formulas.insert(name, expr);
        order.removeAll(name);
        order.append(name);
    }
};

// Deep-copy a signal's data + meta into a fresh, independently-owned Signal.
// Used so a bare-reference formula (`Out = speed`) doesn't re-register and
// mutate the store-owned source signal under a new name.
static std::shared_ptr<Signal> cloneSignal(const std::shared_ptr<Signal>& src) {
    auto view = src->snapshotForRead();
    auto copy = std::make_shared<Signal>(src->meta());
    if (view.count > 0) {
        copy->append(view.timestamps, view.values, view.count);
    }
    return copy;
}

FormulaEngine::FormulaEngine(scope::core::SignalStore& store)
    : impl_(std::make_unique<Impl>(store)) {
    FunctionRegistry::instance();  // force builtin registration
    // When a channel is removed from the store, drop any formula we held
    // for it. During a recompute the store emits channelRemoved as part of
    // an insert-or-replace; evaluate() re-records the formula *after* the
    // store add, so the definition isn't lost in that case.
    impl_->removedConn = QObject::connect(
        &store, &scope::core::SignalStore::channelRemoved,
        &store, [this](const QString& name) {
            if (!impl_->recomputing) forget(name);
        });
}

FormulaEngine::~FormulaEngine() {
    QObject::disconnect(impl_->removedConn);
}

bool FormulaEngine::evaluate(const QString& sourceLine, QString* errorOut) {
    EvalCtx ctx{impl_->store, errorOut};
    Lexer pre(sourceLine);
    auto first = pre.next();
    if (first.kind != Tok::Ident) {
        if (errorOut) *errorOut = "expected '<Out> = <expr>'";
        return false;
    }
    auto second = pre.next();
    if (second.kind != Tok::Assign) {
        if (errorOut) *errorOut = "expected '=' after output name";
        return false;
    }
    const QString outName = first.text;
    const QString exprPart = sourceLine.mid(second.pos + 1);

    Parser parser(exprPart, ctx);
    auto result = parser.parseExpr();
    if (!result) return false;

    // A bare channel reference (`Out = speed`) returns the store-owned
    // source signal itself. Re-registering it under `outName` would mutate
    // the original's meta and alias the two — clone first so they stay
    // independent.
    for (const auto& n : impl_->store.channelNames()) {
        if (impl_->store.get(n) == result) { result = cloneSignal(result); break; }
    }

    // Register the result under the user's chosen name.
    auto meta = result->meta();
    meta.name = outName;
    meta.sourceSymbol = sourceLine.trimmed();
    result->setMeta(meta);
    impl_->store.add(result);

    // Record the definition *after* the store add: add() emits
    // channelRemoved (on replace) which our handler turns into forget(),
    // so recording here keeps the entry that the user just (re-)defined.
    impl_->setFormula(outName, exprPart.trimmed());
    return true;
}

QString FormulaEngine::formulaFor(const QString& name) const {
    return impl_->formulas.value(name);
}

void FormulaEngine::rememberFormula(const QString& name, const QString& expr) {
    if (expr.trimmed().isEmpty()) return;
    impl_->setFormula(name, expr.trimmed());
}

void FormulaEngine::forget(const QString& name) {
    impl_->formulas.remove(name);
    impl_->order.removeAll(name);
}

bool FormulaEngine::isRecomputing() const { return impl_->recomputing; }

int FormulaEngine::recomputeAll(QStringList* errorsOut) {
    if (impl_->recomputing) return 0;
    impl_->recomputing = true;

    QStringList pending = impl_->order;  // snapshot; evaluate() mutates order
    QHash<QString, QString> lastErr;
    int total = 0;
    bool progress = true;
    while (progress && !pending.isEmpty()) {
        progress = false;
        QStringList stillPending;
        for (const auto& name : pending) {
            const QString expr = impl_->formulas.value(name);
            if (expr.isEmpty()) continue;  // forgotten mid-loop
            QString err;
            if (evaluate(name + " = " + expr, &err)) {
                ++total;
                progress = true;
            } else {
                lastErr.insert(name, err);
                stillPending.append(name);
            }
        }
        pending = std::move(stillPending);
    }
    if (errorsOut) {
        for (const auto& name : pending)
            *errorsOut << QString("%1: %2").arg(name, lastErr.value(name));
    }

    impl_->recomputing = false;
    return total;
}

}  // namespace scope::analyser
