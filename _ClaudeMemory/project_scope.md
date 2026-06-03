---
name: ScopeAnalyser project scope
description: What ScopeAnalyser is, what it does, and the hard scope rules
type: project
---

ScopeAnalyser is a desktop application combining three tools, usable
independently or together in a shell window:

1. Signal Recorder for TwinCAT ADS — captures channels with per-channel
   auto-rate (parent-task cycle) or oversampled-array mode.
2. Analyser — formula language with autocomplete (`Filter`, `Integral`,
   `Derivative`, …) over recorded channels.
3. Data Converter — file-to-signal translator with drag-mapping UI and
   saveable `.scaconv` profiles.

Recorded data is immediately available to the Analyser via a shared in-memory
`SignalStore` (Qt signals on add/remove).

## Hard scope rules

- **No PLC-side code.** Pure client. No helper FB, no ST library, no
  modifications to `Frameworks_For_PLC/PLC_Logger` or any other PLC project.
- Separate GitHub repository, no link to `Frameworks_For_PLC`.

**Why:** The user stated this as a non-negotiable architectural constraint
during planning on 2026-06-03.

**How to apply:** If a feature seems to need PLC cooperation (sub-task-cycle
sampling of internal variables, custom triggers, server-side aggregation),
either solve it client-side or document it as out-of-scope. Do not propose a
PLC-side library.
