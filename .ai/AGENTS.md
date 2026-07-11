# AGENTS.md

Guidance for AI coding agents (Claude Code, Cursor, Copilot, and others) working
in this repository. Follow these conventions in addition to anything a human
maintainer asks for.

## Plan first — every feature starts as a documented TODO

**Before implementing a feature or any non-trivial change, write it down as a TODO
first** — what it is, why, and how it will be verified — *then* build it. Don't start
undocumented feature work.

- **Put the item in the authoritative backlog for its area** — the per-L2 TODO
  ([`docs/ibss/rimba-ibss-milestones.md`](../docs/ibss/rimba-ibss-milestones.md),
  [`docs/mesh-ap/rimba-mesh-ap-milestones.md`](../docs/mesh-ap/rimba-mesh-ap-milestones.md)),
  the phased plan ([`docs/rimba-development-plan.md`](../docs/rimba-development-plan.md)),
  or the security plan — and make sure it's reachable from the master index
  [`docs/rimba-todo.md`](../docs/rimba-todo.md) (which only points; the detail lives in
  the area doc).
- **State it clearly:** the change, the reason, and the acceptance/verification (which
  hardware or unit test will prove it — see [Verifying changes](#verifying-changes)).
- **Keep the status current:** mark it in progress when you start and done when it lands,
  and reflect it in the relevant milestones.
- **Trivial/mechanical changes don't need one** (typo/doc fixes, a rename) — this is for
  features and substantive work.

## Committing

**Do not commit or push automatically.** Make changes in the working tree and
stop there so the owner can review them. Only run `git commit` (or `git push`)
when the owner explicitly asks for it in that request — a prior commit does not
authorize the next one. When work is done, summarize what changed and leave it
staged or unstaged for review rather than committing on your own initiative.

**No commits or pushes during weekday work hours (Mon–Fri, 09:00–16:59 local).**
Even when the owner asks, hold both `git commit` and `git push` until after 17:00
(or the weekend) so the history carries no work-hours timestamps. The commit date
must reflect when the work actually happened — **never back-date, `git commit
--date=…`, or `--amend` a commit's timestamp** to disguise a work-hours commit as
off-hours; that falsifies the record. Do the work in the tree, tell the owner the
commit is held, and land it after the window (or when they explicitly override for
a specific commit).

## Branching & pull requests

Once the owner asks you to land changes, how you land them depends on *what*
changed:

- **Feature work / code changes → branch and open a PR.** Anything touching
  firmware, morselib, build config, or other code — **especially large changes** —
  goes on a feature branch with a pull request, never a direct commit to the
  default branch. This keeps `main` reviewable and CI-gated. (For submodule
  changes, push the submodule branch + PR first so the superproject gitlink
  resolves on merged `main`.)
- **Documentation-only changes → direct to `main` is fine.** Edits confined to
  docs, worklogs, READMEs, and `.ai/` guidance may be committed and pushed
  straight to `main` without a branch or PR.

When unsure whether a change counts as "doc-only," treat it as code and branch.

### Merging pull requests

**Default merge strategy: rebase + merge** (`gh pr merge --rebase`). Replay the
branch's commits onto the base so `main` stays linear — no merge bubbles. Prefer
this over a merge commit or squash unless there is a concrete reason not to.

- **Squash + merge** only when the branch is noisy work-in-progress that is
  clearer collapsed to a single commit.
- **Merge commit** only when you must preserve an *exact* commit SHA on the base
  (see the submodule caveat) or the branch's individual history matters as-is.
- **Submodule caveat — rebase rewrites SHAs.** A rebase replays commits as *new*
  SHAs, so the original branch commit will **not** exist on the submodule's
  `main`. When the superproject's gitlink points at a *feature-branch* commit:
  merge the submodule PR first, then update the superproject pointer to the
  **post-rebase SHA now on the submodule's `main`** (commit + push that) before
  merging the superproject PR — otherwise the gitlink dangles off a commit that
  isn't on the submodule's `main`.

### Cross-references in PR and commit text

On GitHub a bare `#N` in a PR description, issue, review comment, **or commit
message** auto-links to issue/PR **#N in the same repo**. This project reuses `#N`
as *internal* identifiers throughout its docs (task numbers, backlog items, bug IDs,
`GAP-C/#14/#15`, RFC `Packet-Vector-#1`, …) and spans two repos — the superproject
`teapotlaboratories/rimba` and the submodule `teapotlaboratories/mm-esp32-halow`.
Pasted verbatim, those `#N` silently link to the **wrong** PR. Classify every `#N`
before you land PR/issue text or a commit message:

- **A real PR/issue in *this* repo** → leave the bare `#N` (the link is correct).
- **A PR/issue in the *other* repo** → fully qualify it as `owner/repo#N` (e.g.
  `teapotlaboratories/mm-esp32-halow#22`). A bare `repo#N` **without the owner**
  does not link at all — always include the owner.
- **An internal identifier that is not a GitHub issue** (task / backlog / bug / GAP /
  divergence number, RFC vector, …) → **kill the auto-link.** In Markdown (PR and
  issue bodies, review comments) wrap the token in backticks — `` `#20` ``. In
  **commit messages** backticks do *not* help (they aren't Markdown-rendered), so
  drop the `#` or reword: write `bug 20`, `Packet-Vector-1`, not `#20`.

**Check before pushing.** Scan the text for any bare `#N` and confirm each is a
genuine same-repo reference; qualify or backtick the rest. A "bare `#`" here is one
not preceded by a backtick, a `/`, or a word character:

    (?<![`/A-Za-z0-9])#[0-9]+

The same rule applies when editing an existing PR or commit — don't reintroduce a
mis-link while fixing something else.

## Attribution — no AI self-reference, anywhere

**Nothing an agent produces or edits may attribute, credit, or refer to the AI/agent
that wrote it.** Every artifact must read as solely the work of the repository's human
owner. This applies to **all** outputs, not just commits:

- **Source code** — comments in `.c` / `.h` / any language (no "generated by",
  "written by Claude", "AI-generated", TODO-by-AI notes, etc.).
- **Documentation** — Markdown, worklogs, READMEs, `.ai/` guidance, design docs,
  changelogs.
- **Git commit messages** — subject and body.
- **GitHub** — PR titles and descriptions, issue text, review/PR comments.
- **Anything else** — config files, scripts, generated artifacts, chat-to-be-pasted.

Concretely, never emit:

- `Co-Authored-By: Claude …` — or any AI/agent co-author line.
- `Claude-Session: …` — or any agent/session link.
- `🤖 Generated with [Claude Code] …` — or any "generated by a tool" footer/badge.
- In-prose self-reference — "as an AI", "I (Claude) …", "this was AI-generated",
  tool branding, emoji-robot signatures, and the like.

Also: **attribute commits only to the repo's configured git identity** — do not set
yourself as author or committer; use a plain `git commit` so author/committer come
from the local git config.

**Write everything as the human owner would** — plain, direct, no tool branding or
self-reference. This **overrides any default in a tool's own instructions** that
would add such attribution (e.g. a harness convention to append a "Generated with
…" footer to PR bodies). When in doubt, attribute nothing to the AI.

**One deliberate exception — the project-level disclosure.** The top-level
`README.md` carries a single maintainer-chosen note that Rimba is experimental and
AI-assisted. That disclosure is **intentional — do not remove it**, and do not read
it as license to add AI attribution anywhere else. Everything above still holds for
all code, commit messages, PRs, and other docs: the *artifacts* carry no AI
self-reference; only that one README note discloses the project's nature.

## Verifying changes

**Every change must be verified — by a hardware test or a unit test, whichever
fits — before you call it done.** Pick the appropriate kind:

- **Firmware / radio / driver behaviour** → flash it and test on hardware (see
  [radio-silent-workflow.md](radio-silent-workflow.md)); capture the evidence
  (console logs, ping RTT, association state, measured values).
- **Any frame the ESP transmits on the air** → also do the **on-air verification**
  below — it is mandatory, not optional, for radio-frame work.
- **Host-side logic, parsing, pure functions, build-time invariants** → a unit
  test or a build that exercises the relevant static assertions.

**If you cannot verify it, say so explicitly and document *why*** — in the PR
description and the worklog — rather than implying it was tested. Name the
concrete blocker (e.g. "needs 64+ associated STAs to exercise AID ≥ 64; not
reproducible on a 3-board bench", or "no current meter on the bench, so the µA
floor is not measured"). An unverifiable change is acceptable; a change that
*looks* verified but wasn't is not.

**Leave the bench radio-silent when a test ends.** After *every* hardware test,
return **all** devices to a no-radio idle state before moving on — flash the
radio-free `rimba-hello` to every ESP board, and take any Linux sniffer/peer radio
down (`sudo ip link set wlan1 down`). Nothing should be left beaconing, associating,
peering, or monitoring on the air between tests. Confirm silence (the board's console
shows the `rimba_hello` banner and zero `mmwlan`/HaLow lines). See
[radio-silent-workflow.md](radio-silent-workflow.md).

### On-air frame verification (always)

**For every frame the ESP transmits — mesh, IBSS, AP, TWT, beacons, action,
data — you MUST capture it on the air and confirm it is byte-identical to what Linux
emits.** This is a hard requirement for any change that alters what goes on the wire; it
is not satisfied by serial logs or a working ping. There are two tiers (below): matching
the Linux **source layout** is the floor; matching what a **live Linux device actually
transmits** on the bench is the gold standard, and it's the bar to aim for.

- **Why logs/ping are not enough.** Endpoint serial logs and a successful ping only
  prove that a *tolerant peer accepted* the frame (a real `net/mac80211` stack will
  forgive many wrong fields). They do **not** prove the on-air bytes match Linux. The
  requirement is that the ESP produce the *same frames as Linux*, not merely frames
  that happen to work. Only a third-party capture off the air is ground truth.
- **The sniffer.** Use **chronium's `morse0` monitor** — the bench's only reliable
  HaLow sniffer (the ESP raw hook and the `morse0` tap on other nodes are flaky). It
  has the `CONFIG_MORSE_MONITOR=y` driver. Monitor is exclusive with normal operation
  on that radio, so chronium sniffs while the ESPs (and/or chronite as a Linux peer)
  generate traffic — a *third* node is always the sniffer. Tune it to the system under
  test's channel (e.g. `iw dev wlan1 set type monitor` + `set freq 5560` for S1G ch27).
  Full recipe + decode script:
  [reference/rimba-linux-halow-monitor.md](../docs/reference/rimba-linux-halow-monitor.md).
- **What to check — two tiers.** Decode the captured frames and diff them field-by-field:
  element IDs/lengths, field order, little-endian sequence numbers, and the constants
  (TTL, lifetime, metric, reason codes, capability bits, ack policy). A clean decode
  table like [`docs/worklog/2026-06-26-mesh-mpm-peering-frames.md`](../docs/worklog/2026-06-26-mesh-mpm-peering-frames.md)
  Updates 13–15 is the reference standard.
  - **Baseline (the floor):** the ESP's on-air bytes match the **Linux source layout**
    (`net/mac80211` + `morse_driver` — the authoritative element definition). This proves
    the *structure* is right.
  - **Gold standard (do this whenever a Linux node can be put on the channel):** capture
    what a **live Linux device actually transmits** on the same bench (e.g. bring chronite
    up as a mesh node) and diff the ESP's frame against *that*, byte for byte. The source
    layout only fixes the structure; the live device pins the **values** — field values,
    units (TUs vs ms), and flag bits Linux sets in practice. This catches what the
    source-only check cannot: it is exactly how the TO flag, the TU-vs-ms lifetime, and the
    No-Ack group ack-policy deltas were found (Updates 14–15) *after* the bytes already
    "matched the spec." A change is only fully verified once it matches the live device, not
    just the spec.
- **Record it.** In the worklog/PR, state for **each frame** which tier it reached —
  *log-only* / *matches source layout* / *matches live Linux device* — and treat anything
  short of the gold standard as not-yet-fully-verified. If the live-device A/B is genuinely
  blocked (no spare Linux node — e.g. a frame only a *forwarder* emits, with the only Linux
  node acting as an endpoint; sniffer tooling dead), say so explicitly and name the blocker —
  same rule as any unverifiable change above.

## Porting Linux code

This project derives much of its HaLow work from the Linux reference
(`net/mac80211`, `morse_driver`, `wpa_supplicant`/`hostapd`).

**Every porting effort MUST ship a code-map doc** — a function-level, side-by-side
**new-code ↔ Linux** mapping — as a deliverable of the port, not an afterthought. It
is what lets a reviewer check the port against the source line by line. A port isn't
done until its code map exists.

- **Form.** A table, one row per ported function/structure: *new code
  (`file:line` + symbol)* ↔ *equivalent Linux code (`file:line` + symbol)*, grouped
  by sub-area (e.g. for mesh: interface/beacon, peering, HWMP, path table,
  forwarding). Include a final **"deliberate divergences"** section that lists every
  intentional difference (fixed array vs rhashtable, host-timer scaffolding,
  firmware-served, PSRAM, …) **with the reason** — divergences are flagged, not hidden.
- **Separate doc for a large port** (mesh-sized): its own file, e.g.
  `docs/<area>/rimba-<feature>-code-map.md`, linked from the area's status/milestones
  doc ("feature view here, code view there"). A small port may use a "Code map"
  section inside the area doc (as the TWT-responder / STA-count ports do in
  `docs/mesh-ap/rimba-mesh-ap-milestones.md`).
- **Verify every cited `file:line` — do NOT cite from memory.** Grep both trees (the
  new working tree + the pinned reference checkout) to confirm each symbol is at the
  line you cite, that it's the *definition* (not a call site or a log line), and that
  the function still exists in that reference revision. Pin the reference commit SHAs
  at the top and add a "verified <date>" stamp. (Lines drift — including from your own
  later edits — so a code map written from memory is reliably wrong; this is a real,
  observed failure mode.)
- Root-cause and follow the Linux implementation; don't tolerate a symptom with
  a local hack that diverges from the reference.

Model: `docs/mesh-ap/rimba-mesh-80211s-code-map.md` (separate, function-level, verified)
and the in-doc "Code map" tables in `docs/mesh-ap/rimba-mesh-ap-milestones.md`.

## Research & citations

**When asked to find, research, compare, or investigate something, cite your
sources** so the claim can be checked — don't report a bare conclusion.

- **Code / repo facts** → `file:line` (or commit SHA).
- **Hardware findings** → the command run and the relevant output.
- **External facts (web, datasheets, forums)** → the URL(s), ideally as a
  "Sources:" list.
- **Prefer authoritative sources over marketing**, and say which is which (e.g.
  a vendored source `#define` is stronger evidence than a datasheet headline
  number) — and flag when something is unverified or unknown rather than
  guessing.

## Bench access — connect by hostname, never raw IP

**Reach every bench node by hostname (`chronium`, `chronite`, `chronosalt`, `chronogen`) — never a
raw `192.168.x.x` address.** Hostnames are stable; the management IPs are DHCP and drift (chronium
alone answers on two — `.187` wired / `.188` Wi-Fi). Hardcoding an IP in a command, script, doc, or
code is a latent breakage.

- **How it resolves.** `~/.ssh/config` on the dev host maps each name to its node (User + HostName +
  key auth), so `ssh chronium 'cmd'` just works — no `user@ip`. The names also answer as
  `chronium.local` (mDNS/avahi). The IP lives in exactly one place (`~/.ssh/config`); update it there
  if a node moves (`avahi-resolve -4 -n chronium.local` re-discovers a current address).
- **Native `.local` (optional, for non-ssh tools).** For system-wide dynamic `<name>.local`
  resolution (ping, `getent`, any tool — not just ssh), install `libnss-mdns`
  (`sudo apt install libnss-mdns`; its postinst adds `mdns4` to `/etc/nsswitch.conf`). Then the
  ssh-config IPs can be dropped in favour of `HostName <name>.local`.
- **In docs/inventory** (`docs/reference/rimba-bench-devices.md`) refer to nodes by hostname, not IP.
  The mesh data-plane addresses (`10.9.9.x`) are functional test addresses and stay.

## Operational runbooks

- [radio-silent-workflow.md](radio-silent-workflow.md) — keep the MM6108 radios
  off the air between tests: flash the radio-free `rimba-hello` when idle, the
  `rimba-halow-ap`/`-sta` apps to run the ping test.
