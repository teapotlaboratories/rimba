# Session handoff — 2026-06-19

Snapshot for resuming after a device re-setup. Covers the Phase-1 merge, the
Phase-2 plan, the open-IBSS test plan, and the exact next step.

---

## Where we are

**Phase 1 (open 802.11ah IBSS foundation, RISK-01) is DONE and merged to `main`**
on both repos. Phase 2 (link security) is planned but **not started**. Current
focus: **validate the open IBSS thoroughly before building Phase 2**.

### Git / PR state
- **rimba** `main` tip: `197d7cc` (Phase-1 merged via PR #1 + submodule re-point PR #2).
- **mm-esp32-halow** (submodule) `main`: includes IBSS support (PR #1 merged).
  rimba `main` submodule gitlink = `da66ef6` (on submodule `main`, durable).
- All three PRs merged: rimba#1, mm-esp32-halow#1, rimba#2.
- **`test2` branches** (both repos) are now redundant — safe to delete (the
  pointer no longer depends on them).
- **Current working branch: `test/open-ibss-validation`** (off `main`), holds:
  - `docs/rimba-ibss-test-plan.md` (committed `39b18f6`)
  - this handoff doc.
- ⚠️ **`test/open-ibss-validation` is NOT pushed yet** — local only. Push before
  wiping the device or it's lost.
- ⚠️ Untracked, at-risk if wiped: `docs/MM_APPNOTE-24_Linux_Porting_Guide.pdf`
  (Morse Linux Porting Guide reference PDF).

### Tooling note
- `gh` CLI installed at `~/.local/bin/gh`, authenticated as `aldwinhermanudin`
  (token scopes: repo). A new setup will need `gh` reinstalled + re-auth.

---

## Key decisions made this session

1. **CCMP via chip 802.11 RSN is the WRONG layer / blocked.** `SET_STA_STATE`
   returns `-116` on the ADHOC interface and blocks the core task from RX context.
   The spec's "AES-128-CCM" is a **software peer-link shim above the open IBSS**,
   not chip RSN.
2. **Phase-2 link security = two tiers** (Thread/802.15.4 shape, in software):
   - **Tier A — network group key** `HKDF(deployment_root, "HMDP-v1-netkey")`
     encrypting the whole broadcast control plane (HELLO, routing, PEER handshake).
   - **Tier B — per-peer ECDH** (X25519 → HKDF session key) on unicast DATA.
3. **Topology privacy is IN scope** (Tier-0 threat-model decision, recorded in
   `docs/rimba-hardening-plan.md` §1.2) → both tiers get built. Residuals accepted:
   group key is outsider-only (captured node exposes it); RF localization + coarse
   traffic analysis remain; rotate the group key.

---

## Open-IBSS test plan (the current focus)

Full plan: **`docs/rimba-ibss-test-plan.md`**. Topology: 3× ESP32-S3 + HaLow
(ACM0/1/2) + a 4th **Linux MM6108** node (`morse_driver`) as the reference-impl
interop check. Priority order: P0 multi-node ESP32 → P0.5 Linux interop →
P1 reliability → P2 RF/edge.

Common cell params: SSID `rimba-ibss`, fixed BSSID `02:12:34:56:78:9a`, S1G ch27 /
1 MHz / op-class 68 / US, OPEN. IP convention: `192.168.13.<octet(mac)>`
(octet = `mac[5]`, clamp 0→1 / 255→254) on every node incl. Linux.

---

## NEXT STEP (resume here)

**(b) N-node addressing app change** in `firmware/rimba-halow-ibss/main/app_main.c`
(prerequisite for ≥3-node testing — current `.1`/`.2` role IPs collide at 3 nodes):
- `my_ip = 192.168.13.<octet(my_mac)>`.
- Ping every discovered peer: for each `mmwlan_ibss_get_peers()` entry,
  `peer_ip = 192.168.13.<octet(peer_mac)>`, ping it; log per-peer result.

Then: bench 3 ESP32 (ACM0/1/2) → run P0 → bring up Linux node → P0.5 → P1.

---

## Task backlog (harness task IDs — may not persist across setup)

- ✅ #13 raw `0x88B5` exchange · ✅ #14 per-peer station records
- ☐ #16 IBSS merge + real create/join (drops MAC-role + hardcoded-BSSID heuristics)
- **Phase 2 (blocked-by chain):**
  - ☐ #15 P2.1 crypto + key foundation + `0x88B5` security framing
  - ☐ #17 P2.2 Tier A — encrypted HELLO discovery + neighbor table (needs #15)
  - ☐ #18 P2.3 Tier B — PEER_OPEN/CONFIRM ECDH handshake (needs #15, #17)
  - ☐ #19 P2.4 Tier B — AES-128-CCM on unicast DATA (needs #18)
  - ☐ #20 P2.5 lifecycle — close, re-key, rotation (needs #19)

## Bench state
Both boards currently flashed with `rimba-hello` (radio-silent). Test firmware is
`firmware/rimba-halow-ibss`; build via `make build APP=rimba-halow-ibss
BOARD=proto1-fgh100m`, flash per port with `make flash ... PORT=/dev/ttyACMx`.

---

## Governing rule (still applies)
Everything IBSS is derived from + verified against the Linux implementation
(`net/mac80211/ibss.c`, `morse_driver`) — not improvised from morselib's AP path.
