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

// Elementwise binary op on two signals. If either is a constant (size 1),
// broadcast it across the other.
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

    if (!aConst && !bConst) {
        if (avView.count != bvView.count) {
            setErr(ctx, QString("'%1': operands must have the same length (%2 vs %3)")
                            .arg(opName).arg(avView.count).arg(bvView.count));
            return nullptr;
        }
    }
    const std::size_t n = aConst ? bvView.count : avView.count;
    std::vector<double> out(n);
    const TimestampNs* ts = aConst ? bvView.timestamps : avView.timestamps;
    for (std::size_t i = 0; i < n; ++i) {
        const double va = aConst ? av[0] : av[i];
        const double vb = bConst ? bv[0] : bv[i];
        out[i] = op(va, vb);
    }

    Signal::Meta m;
    m.dataType = DataType::Float64;
    m.name = opName;
    auto sig = std::make_shared<Signal>(m);
    sig->append(ts, reinterpret_cast<const std::byte*>(out.data()), n);
    return sig;
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
    explicit Impl(scope::core::SignalStore& s) : store(s) {}
};

FormulaEngine::FormulaEngine(scope::core::SignalStore& store)
    : impl_(std::make_unique<Impl>(store)) {
    FunctionRegistry::instance();  // force builtin registration
}

FormulaEngine::~FormulaEngine() = default;

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

    // Register the result under the user's chosen name.
    auto meta = result->meta();
    meta.name = outName;
    meta.sourceSymbol = sourceLine.trimmed();
    result->setMeta(meta);
    impl_->store.add(result);
    return true;
}

}  // namespace scope::analyser
