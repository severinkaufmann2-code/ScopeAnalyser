---
name: ScopeAnalyser sampling model and 10us caveat
description: The two client-side acquisition modes and what 10 us actually means
type: project
---

ScopeAnalyser supports exactly two acquisition modes (no PLC-side code, ever):

| Mode | Source | Typical max rate |
|---|---|---|
| `AdsNotify` | any symbol; ADS device notifications at parent task cycle | 1 ms (typical), down to 100 µs |
| `Oversampled` | `ARRAY[0..N-1]` already exposed by a TwinCAT oversampling terminal or fast task | down to ~10 µs |

The 10 µs / 100 channels target from the original brief is **only reachable
via `Oversampled` mode**. For arbitrary internal PLC variables (computed in
PLC code, not from an oversampling terminal), the rate is bounded by their
parent task cycle. The user acknowledged this trade-off on 2026-06-03.

**Why:** Without PLC cooperation there is no client-side trick that produces
sub-task-cycle samples for variables that are only updated once per task.
ADS notifications fire at most once per parent-task cycle.

**How to apply:** When the user asks for "fast" sampling of a specific
variable, first check whether it's an oversampling array. If it isn't, the
honest answer is "task-cycle rate." Don't invent a workaround that would
require PLC changes.
