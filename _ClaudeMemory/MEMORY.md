# ScopeAnalyser — Project Memory

## Memory Rules
- Memory goes in `_ClaudeMemory/` inside this project (never in `.claude/projects/`).
- This project has its own git repo, so `_ClaudeMemory/` is committed and
  pushed with that repo.
- Plans and execution logs go in `_PlansAndExecution/` (same level as
  `_ClaudeMemory/`). Two files per session with matching timestamps:
  - `YYYYMMDD_HHMM_Plans.md` — the plan for the session
  - `YYYYMMDD_HHMM_executedTasks.md` — what was actually executed

## Index
- [project_scope.md](project_scope.md) — what ScopeAnalyser is and its hard scope rules
- [stack.md](stack.md) — language, framework, library, build-system decisions
- [sampling_model.md](sampling_model.md) — the two acquisition modes and the 10 µs caveat
