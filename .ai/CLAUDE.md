# CLAUDE.md

Project guidance for AI coding agents lives in [AGENTS.md](AGENTS.md) — read it.

Most important rules:
- **Every feature starts as a documented TODO** (in its area backlog, indexed from
  `docs/rimba-todo.md`) before you build it. See
  [AGENTS.md → Plan first](AGENTS.md#plan-first--every-feature-starts-as-a-documented-todo).
- **Do not commit or push automatically** — only when explicitly asked. See
  [AGENTS.md → Committing](AGENTS.md#committing).
- **No AI attribution anywhere** — not in code comments, docs, commit messages, or
  GitHub PR/issue text. Everything reads as the human owner's work; commits use the
  repo's git identity only. See
  [AGENTS.md → Attribution](AGENTS.md#attribution--no-ai-self-reference-anywhere).
- **Code/feature changes → branch + PR; doc-only changes → may push to `main`.**
  See [AGENTS.md → Branching & pull requests](AGENTS.md#branching--pull-requests).
- **Merge PRs with rebase + merge by default** (`gh pr merge --rebase`); keep
  `main` linear. Mind the submodule-SHA caveat. See
  [AGENTS.md → Merging pull requests](AGENTS.md#merging-pull-requests).
- **Verify every change** with a hardware or unit test; if you can't, document
  why. See [AGENTS.md → Verifying changes](AGENTS.md#verifying-changes).
- **When porting Linux code, document the new ↔ Linux mapping.** See
  [AGENTS.md → Porting Linux code](AGENTS.md#porting-linux-code).
- **Cite sources** when finding, researching, or comparing. See
  [AGENTS.md → Research & citations](AGENTS.md#research--citations).
