# CLAUDE.md

Project guidance for AI coding agents lives in [AGENTS.md](AGENTS.md) — read it.

Most important rules:
- **Every feature starts as a documented TODO** (in its area backlog, indexed from
  `docs/rimba-todo.md`) before you build it. See
  [AGENTS.md → Plan first](AGENTS.md#plan-first--every-feature-starts-as-a-documented-todo).
- **Do not commit or push automatically** — only when explicitly asked, and
  **never during weekday work hours (Mon–Fri 9 AM–5 PM local); no back-dating to
  dodge it.** See [AGENTS.md → Committing](AGENTS.md#committing).
- **No AI attribution anywhere** — not in code comments, docs, commit messages, or
  GitHub PR/issue text. Everything reads as the human owner's work; commits use the
  repo's git identity only. See
  [AGENTS.md → Attribution](AGENTS.md#attribution--no-ai-self-reference-anywhere).
- **Code/feature changes → branch + PR; doc-only changes → may push to `main`.**
  See [AGENTS.md → Branching & pull requests](AGENTS.md#branching--pull-requests).
- **Merge PRs with rebase + merge by default** (`gh pr merge --rebase`); keep
  `main` linear. Mind the submodule-SHA caveat. See
  [AGENTS.md → Merging pull requests](AGENTS.md#merging-pull-requests).
- **No mis-linking `#N` in PR/commit text** — a bare `#N` auto-links to a
  *same-repo* issue/PR, so cross-repo refs must be qualified as `owner/repo#N` and
  internal IDs (task/backlog/bug numbers) backticked in Markdown (`` `#20` ``; in
  commit messages drop the `#`). Scan before pushing. See
  [AGENTS.md → Cross-references in PR and commit text](AGENTS.md#cross-references-in-pr-and-commit-text).
- **Verify every change** with a hardware or unit test; if you can't, document
  why. See [AGENTS.md → Verifying changes](AGENTS.md#verifying-changes).
- **Always on-air-verify radio frames** — capture every frame the ESP transmits on
  chronium's `morse0` monitor and byte-diff it against Linux. Serial logs + a working ping
  are not sufficient. Floor: match the Linux **source layout**; **gold standard** (the bar):
  match what a **live Linux device actually transmits** on the bench — that's what catches
  value/unit/flag deltas the spec check misses. See
  [AGENTS.md → On-air frame verification](AGENTS.md#on-air-frame-verification-always).
- **Every porting effort ships a code-map doc** — a function-level, side-by-side
  new-code ↔ Linux mapping (`file:line` ↔ `file:line`) + a deliberate-divergences
  section. Verify every cited line by grepping both trees; never cite from memory. See
  [AGENTS.md → Porting Linux code](AGENTS.md#porting-linux-code).
- **Every worklog gets an HTML render** — when you add or substantially edit a
  `docs/worklog/*.md`, hand-author its companion `docs/worklog/html/<name>.html`
  (shared `style.css`, visuals + a content-driven diagram; no Markdown→HTML converter)
  and update the index. See
  [AGENTS.md → Worklog HTML renders](AGENTS.md#worklog-html-renders).
- **Cite sources** when finding, researching, or comparing. See
  [AGENTS.md → Research & citations](AGENTS.md#research--citations).
- **Connect to bench nodes by hostname, never raw IP** — `ssh chronium` (or `chronium.local`), not
  `ssh user@192.168.x.x`; the IP lives only in `~/.ssh/config`. See
  [AGENTS.md → Bench access](AGENTS.md#bench-access--connect-by-hostname-never-raw-ip).
