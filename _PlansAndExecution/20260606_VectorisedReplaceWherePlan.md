# Vectorised replace / where / fill — roadmap

Source decision: rather than add Python-like `if`/`for` statements
to the formula language (which would require a real interpreter,
mutable signal buffers, indented blocks, comparison + boolean
operators, ~10× engine complexity, and slow per-sample evaluation),
add focused vectorised primitives that cover the common imperative
patterns in one line each.

This file is a roadmap. Pieces get built as they're needed.

---

## Tier 1 — covered by this PR

| Function | Signature | Notes |
|---|---|---|
| `Replace(s, oldV, newV)` | scalar-or-signal args | Replace samples equal to `oldV` with `newV`. |
| `ForwardFill(s)` / `ForwardFill(s, fillValue)` | fillValue defaults to 0 | Replace fill samples with the previous non-fill value. Leading run of fill values is preserved (no preceding good value yet). |

## Tier 2 — to add when first needed

| Function | Signature | Notes |
|---|---|---|
| `BackwardFill(s)` / `BackwardFill(s, fillValue)` | mirror of ForwardFill | Walk backwards. |
| `Where(cond, ifT, ifF)` | element-wise ternary | All three args can be signal or scalar. Truthy = non-zero. Once we have comparison ops (Tier 3) this covers most `if-then-else` cases. |
| `Coalesce(s1, s2, …)` | n-ary, first non-zero | Pick first non-zero/non-NaN value across N signals per sample. |
| `Replace` extended to signal-valued newV | already done in Tier 1 via elementwiseBinary | confirm it works |

## Tier 3 — to add for `Where` to be expressive

Comparison operators: `==`, `!=`, `<`, `<=`, `>`, `>=` — element-wise,
return a 0/1 signal.

Boolean operators: `and`, `or`, `not` — element-wise on 0/1 signals,
short-circuit not needed (no side effects).

Lexer + parser changes:
- New tokens for the comparison operators (two-character: `==`, `!=`,
  `<=`, `>=`)
- Precedence: comparisons below `+`/`-`, booleans below comparisons
- `not` as a unary

Tests: a couple per operator.

## What we explicitly do NOT do

- `if` / `for` / `while` statements
- `signal[n]` subscript syntax
- Mutable signal-element assignment
- Multi-line statement blocks
- Indentation- or `end`-delimited bodies

If a use case emerges that absolutely needs imperative control flow,
revisit with concrete motivating examples. So far every case we've
hit reduces cleanly to a vectorised primitive.

## Performance rationale

`ForwardFill` on a 400 000-sample signal: one C++ pass with one
conditional per sample, ~0.5 ms wall-clock.

The same logic via a hypothetical interpreted `for n in range(s)`
loop: 400 000 dispatched ops, ~hundreds of ms to seconds — enough
to feel laggy in the formula dialog.
