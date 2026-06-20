# Session handoff — 2026-06-19 (evening)

Supersedes [`2026-06-19-session-handoff.md`](2026-06-19-session-handoff.md) (morning
snapshot). That handoff's NEXT STEP — the N-node addressing change — is **done**,
and the branch is now **pushed**. This captures the end-of-session state.

---

## Where we are

**Phase 1 (open IBSS) merged to `main`.** Current focus unchanged: **validate the
open IBSS thoroughly before Phase 2 (link security).** This session took validation
from "2 boards" to "3 boards, P0 passed" and prepped the Linux interop check (P0.5).

### Git / PR state
- Working branch **`test/open-ibss-validation`** — **pushed** (tracks origin).
  - Tip `72742e5`. Commits this session (on top of `893fce4`):
    - `51e84b5` firmware: N-node IBSS addressing + 3-board P0 bench
    - `fb1eca0` docs: peer age-out backlog item (#14)
    - `72742e5` docs: Linux IBSS interop runbook (P0.5)
- ⚠️ Still untracked, at-risk if wiped: `docs/MM_APPNOTE-24_Linux_Porting_Guide.pdf`
  (5 MB vendor reference; deliberately not committed — decide whether to track it).
- `gh` at `~/.local/bin/gh`, authed as `aldwinhermanudin`.

---

## What this session did

1. **N-node addressing** (`firmware/rimba-halow-ibss/main/app_main.c`): replaced the
   2-board role IPs (`.1`/`.2`) with `my_ip = 192.168.13.<octet(mac)>` and per-peer
   pinging driven off `mmwlan_ibss_get_peers()`. App is now topology-agnostic
   (2/3/N + Linux). Builds clean.
2. **3-board P0 bench** (ACM0/1/2, `proto1-fgh100m`) — see worklog
   [`2026-06-19-p0-multinode-bench.md`](2026-06-19-p0-multinode-bench.md):
   - ☑ P0.1 discovery (exactly 2 records, distinct AIDs) · ☑ P0.2 all-pairs (165
     replies, 0 timeouts) · ☑ P0.3 broadcast · ☑ P0.5 concurrent load · ☑ P0.7 one cell.
   - ◐ P0.6 drop/rejoin: survivor link unaffected + dropped node rediscovered, BUT
     survivor-side re-acquisition under-tested (app `pinged[]` dedup × no-age-out).
   - ◐ P0.4: inferred, not isolated.
3. **Peer age-out** written into the backlog as a tracked P2 item **#14** (mirrors
   `ieee80211_ibss_sta_expire` / 60 s inactivity), cross-linked from #4 (merge).
4. **P0.5 runbook** (`docs/rimba-ibss-linux-interop-runbook.md`) — Linux interop
   bring-up, grounded in the porting-guide PDF + recon worklog.

---

## NEXT STEP (resume here)

**P0.5 — Linux HaLow interop** (needs physical hardware: a Linux MM6108 node).
Follow `docs/rimba-ibss-linux-interop-runbook.md`. The blocking unknown is the
**S1G IBSS join syntax** — there is **no vendor-documented IBSS recipe** (guide is
AP/STA only; Luckfox ref compiles ADHOC but never configures IBSS). Resolve it on
the Linux node (`iw list` → freq; Method A `iw ibss join`, else Method B
`wpa_supplicant_s1g mode=1` OPEN), then back-fill the working command into the
runbook + test-plan §5.

Order after that: I.1 discovery → I.2 beacon (riskiest) → I.3 ping → I.4 monitor
capture (closes #11) → I.5 mixed 4-node cell → **P1 reliability / soak**.

---

## Robustness items that unblock reliability testing
- **#14 peer age-out** (P2) — bounds the heap, enables >8-node cells, prerequisite
  for honest survivor-side drop/rejoin. Best built **with #4 IBSS merge (TSF)**.
- Both derive from the Linux side per the governing rule.

## Phase 2 (still blocked behind validation)
Software peer-link shim, not chip RSN (CCMP-via-chip dead: `SET_STA_STATE` -116 on
ADHOC). Tier A network group key + Tier B per-peer X25519 ECDH. Chain:
#15 crypto foundation → #17 Tier A HELLO → #18 Tier B handshake → #19 CCMP DATA →
#20 lifecycle/rekey.

## Bench state
All three boards radio-silent (`rimba-hello`). Test firmware
`firmware/rimba-halow-ibss`; build `make build APP=rimba-halow-ibss
BOARD=proto1-fgh100m`, flash per port.

## Env note
`pypdf` was installed into the ESP-IDF python env this session (to read the porting
guide PDF) — harmless; remove if you want the env pristine.

## Governing rule (still applies)
Everything IBSS derived from + verified against the Linux implementation
(`net/mac80211/ibss.c`, `morse_driver`) — not improvised from morselib's AP path.
