# AGENTS.md

**Read the [`.ai/`](.ai/) directory before doing any work in this repo.** The full
guidance for AI coding agents (and the conventions every human maintainer follows) lives
there — this file is only a pointer to it.

Start with:

- **[`.ai/AGENTS.md`](.ai/AGENTS.md)** — the authoritative conventions: plan-first, how to
  build (`make … BOARD=…`, never bare `idf.py`), committing/branching rules, no AI
  attribution anywhere, hardware/on-air verification, worklogs, and bench access.
- **[`.ai/CLAUDE.md`](.ai/CLAUDE.md)** — the short list of the most important rules, each
  linking into `.ai/AGENTS.md`.
- **[`.ai/radio-silent-workflow.md`](.ai/radio-silent-workflow.md)** — the runbook for
  keeping the HaLow bench radios off the air between tests.

These instructions **override** default agent behaviour — follow them exactly. If anything
here and in `.ai/` seems to conflict, `.ai/AGENTS.md` is authoritative.
