# Fork migration handoff — mm-esp32-halow → esp-halow (scoping + strategy)

**2026-07-18 · planning / handoff (migration NOT started)**

The owner wants to move all of our morselib work from `teapotlaboratories/mm-esp32-halow` (the current
`components/halow` submodule) onto `teapotlaboratories/esp-halow` — the proper fork of the **official**
`MorseMicro/esp-halow` SDK — preserving commit history + PRs if possible, then repoint `components/halow`
at esp-halow. This is the long-deferred **"fork migration"** backlog item, now unblocked (regression suite
green, rimba#36 merged). This worklog captures the repo analysis so the fresh session starts from facts,
not a blank page.

## The two repos (verified with `gh` + `git`, 2026-07-18)

| | `mm-esp32-halow` (ours) | `esp-halow` (target) |
|---|---|---|
| GitHub | `teapotlaboratories/mm-esp32-halow` | `teapotlaboratories/esp-halow` |
| Fork? | **standalone** — `isFork:false`, `parent:null` | **fork of `MorseMicro/esp-halow`** (official) |
| Tip | `935c6b82` (merge of PR #24) | `7a97b8ec mm-iot-sdk: update to 2.12 tag` |
| Base SDK | **morselib 2.10.4-esp32-2** (ESP Component Registry) | **Morse SDK 2.12** (mm-iot-sdk layout) |
| Our work | 69 commits over base, **24 merged PRs (#1–#24)** | none of ours yet |

Import boundary in mm-esp32-halow: `68ddbd8d Import morsemicro/halow 2.10.4-esp32-2 (pristine from ESP
Component Registry)`. **Everything above `68ddbd8d` is ours** (IBSS, 802.11s mesh + SAE/AMPE/CCMP security,
mesh-gate AP concurrency, TWT responder+requester, SW-CCMP bulk-AES, A-MPDU, real-RC airtime metric,
mmwlan accessors) — i.e. the range `68ddbd8d..origin/main` = 69 commits.

## The three decisive findings

1. **Disjoint histories.** `git merge-base origin/main esphalow/main` → **no common ancestor**.
   mm-esp32-halow was a fresh squashed import from the ESP Component Registry, so it shares **no commit
   objects** with esp-halow even though both descend from the Morse SDK. **Consequence:** there is no
   shared base to rebase onto; our commits must be **replayed** (cherry-pick, or `git format-patch` +
   `git am -3`) onto a branch off `esphalow/main`.

2. **Version gap 2.10.4 → 2.12.** Our work was written against 2.10.4; the target is 2.12. So this is a
   **forward-port across SDK versions**, not a clean transplant — expect conflicts where 2.12 changed the
   code our patches touch.

3. **Possible layout mismatch.** mm-esp32-halow imported the *ESP Component Registry package* of 2.10.4;
   esp-halow is the *mm-iot-sdk source tree* at 2.12. The directory layouts may differ, so patches may not
   apply by path. **The fresh session's FIRST step must be to diff the `68ddbd8d` tree against
   `esphalow/main` and establish the path mapping BEFORE choosing a replay strategy.**

## What "preserve history + PRs" can and cannot mean here

- **Commit history:** per-commit author / date / message **survive** cherry-pick / `git am`, but they land
  as **new SHAs on a new base (2.12)**, and conflict resolution alters the diffs. It will **not** be a
  SHA-preserving transplant — be honest about that with the owner.
- **PRs:** GitHub PRs **cannot move across repos**. "Preserve PRs" realistically means **recreate** them —
  replay each of the 24 PRs as its own branch/PR on esp-halow, or land the forward-port as a smaller set of
  logical PRs. PR numbers and review threads won't transfer.

## Strategy options (decide WITH the owner before executing)

- **(a) Cherry-pick the range** `68ddbd8d..origin/main` onto a branch off `esphalow/main` — keeps per-commit
  history; heaviest conflict load.
- **(b) `git format-patch 68ddbd8d..origin/main` → `git am -3`** — same history preservation, better 3-way
  conflict tooling.
- **(c) Replay per-PR** as 24 fresh branches/PRs on esp-halow — maps best to "preserve PRs"; more overhead.
- **(d) One cumulative forward-port PR** — simplest for the big version jump; loses per-commit granularity.

## PR inventory to preserve (mm-esp32-halow, all MERGED)

`#1–#4` IBSS bring-up · `#5–#6,#9` TWT responder/requester · `#7–#8` AP STA-cap + TIM · `#10–#11` 802.11s
mesh control/data + secured (SAE/AMPE/CCMP) · `#12–#13` mesh toggles + SAE hardening · `#14–#19` HWMP /
airtime metric / table caps / real-RC · `#20` AP WNM-sleep · `#21` mesh+AP concurrency · `#22` SW-CCMP
bulk-AES · `#23` A-MPDU + defrag-before-decrypt + HWMP D1 · `#24` mmwlan accessors.

## Footguns for the fresh session

- **Submodule gitlink SHA caveat** — when landing on esp-halow, merge the submodule PR(s) so the
  superproject gitlink resolves on merged `main` (rebase rewrites SHAs; see `.ai/AGENTS.md` merge rules).
- **Re-run the WHOLE regression suite against 2.12** after repointing `components/halow` — a stack bump is
  exactly what the suite exists to guard; nothing is "done" until T0/T1/T2/tp/dscycle are re-verified.
- **Standing rules:** no commit/push weekday 09:00–16:59 local, owner-attributed only, code → branch +
  PR, and **/review before merge** (now a rule). Bench radio-silent after tests; connect by hostname.
- **fw/driver parity** — bumping the SDK (2.10.4→2.12) may force a matching Linux driver / MM6108 fw bump on
  the bench so the ESP and Linux sides stay on the same version (see the "Morse FW same version" rule).
- I added an `esphalow` remote inside `components/halow` and fetched `esphalow/main` during this analysis —
  a harmless local-only remote, left in place so the fresh session can `git log esphalow/main` immediately.

## Next step

Owner will clear context and restart. Fresh session's first actions: (1) diff `68ddbd8d` tree vs
`esphalow/main` to map the layout, (2) pick a replay strategy with the owner, (3) forward-port + resolve
conflicts, (4) repoint `components/halow` and re-run the full suite.
