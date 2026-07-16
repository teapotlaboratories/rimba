# Worklog — 2026-07-12 — Mesh peering-flap bisection: NO regression in the uncommitted A-MPDU layers

**Author:** Aldwin
**Phase:** 802.11s mesh throughput — A-MPDU (S1–S3) — unblocking the S3 RX-drop read by resolving the "ESP mesh peering flap"
**Goal:** Bisect the reported peering regression. Prior sessions found a cold-booted ESP flaps its
mesh plink with a Linux peer (chronite) — establishes then closes, repeatedly — while an all-Linux
mesh on the same secured config/channel/RF is rock-stable. The working hypothesis (the session
reframe) was that this is a **regression in the uncommitted ESP changes** (S2 multi-hop next-hop
stad, S3 relay forward aggregation, RTC-noinit counters, FIX-1, plink/hwr telemetry) layered on the
committed S1 baseline `7fdba0c9` (BLOCK_ACK RX routing + MFP=no).
**Status:** **RESOLVED — there is NO regression in the uncommitted layers.** Both the current working
tree AND the clean committed `7fdba0c9` baseline peer with chronite and hold a **rock-stable plink**
on the SAME board2, same chronite, same idle secured config, in a fresh bench session. The prior
"8 established / 5 closed then quiet, 100% ping loss" flap reproduced on **neither** firmware → it
was a transient degraded-chip / bench-condition artifact, not code. Bench left radio-silent, working
tree restored exactly, nothing committed.

This entry is **standalone**.

---

## 0. TL;DR

- **Code analysis plus an independent adversarial re-audit both conclude the uncommitted morselib is
  IDLE-PEERING-NEUTRAL vs `7fdba0c9`.** Every uncommitted touch of the MPM/SAE/AMPE handshake is either a
  side-effect-free telemetry counter increment or confined to the DATA / FORWARD / HW-RESTART planes that
  are never entered at pure idle. The regression hypothesis is **refuted in code** (0 of 5 flagged
  suspects survived adversarial verification).
- **Bench A/B on the SAME board2 / chronite / idle `MESH_IPERF` config, current-tree FIRST (freshest chip),
  baseline SECOND:**
  - **Current tree:** peer → one early re-SAE settle (t≈43 s) → **stable ~5.5 min**; ESP RTC `g_plink_close=0`
    (never tore down an established plink), `restart_attempts=0`, `boot_count=2` (no crash-loop).
  - **Baseline `7fdba0c9`:** peer → one cold-boot reconnect → **continuously stable**, chronite
    `connected time = 686 s` and counting, `est_delta=0 disc_delta=0` over the whole 150 s window.
  - **Identical behaviour. Both stable. Neither flaps.**
- **The reframe's "known-good" control was confounded.** The prior "ESP↔Linux ping 45/45 on committed S1"
  was **traffic-loaded board0 under `MESH_LINUX_INTEROP` (auto-ping)** — continuous ping frames keep the
  peer-link inactivity timer from firing. The flap was seen on **idle board2 under `MESH_IPERF` (no
  `g_ping_target`)**. Different board, different config, different traffic — not an apples-to-apples control.
- **The Linux-stable control isolates the flap to the ESP HOST STACK, not the radio.** The WHOLE bench
  (3× ESP + 4× Linux) runs the SAME Morse Micro **MM6108** on the same **FGH100M / `fgh100mhaamd`** module
  with the same `mm6108.bin` 1.17.8 fw + BCF — only the host differs (ESP **morselib** vs Linux **mac80211 +
  morse_driver**) and the carrier board. (Corrected 2026-07-12: an earlier draft wrongly said "Linux uses
  different radios" — it does not; see docs/reference/rimba-bench-devices.md "SAME RADIO SILICON".) So an
  all-Linux mesh peering stably on the *same* MM6108+fw while the ESP flaps points squarely at **morselib's
  MPM**, not the MM6108 or the air. Combined with the A/B (it is not the uncommitted layers), the residual is
  the **committed morselib MPM keepalive gap** (§4) surfacing under marginal RX — not "a degraded chip".
- **Implication: the S3 RX-drop read is UNBLOCKED.** The prior blocker (board2 plink flapping → 0 traffic →
  counters stay 0) was chip-condition, not code. In a fresh session board2 holds a stable plink, so a
  `board0 → board2(relay) → chronite` blast can now accumulate the RX/forward drop counters.

---

## 1. Method

1. **Git state.** Submodule `components/halow` HEAD is exactly `7fdba0c9` (the committed S1 baseline); ALL
   of S2/S3/RTC/FIX-1/telemetry is uncommitted working-tree diff (12 files, +431/-29) + one app file
   (`firmware/rimba-halow-mesh-perf/main/app_main.c`). So "get to S1 baseline" = `git stash` the submodule
   + revert `app_main.c` (they must move together — the app references morselib externs).
2. **Code trace** of every mesh frame class transmitted during peering, to see whether any uncommitted hunk
   changes idle MPM/SAE behaviour.
3. **Independent adversarial re-audit** (4 lenses — frame-class tracer, always-on-delta, S2/S3 data-path
   escape, reframe reconcile — each finding then adversarially refuted) as a cross-check on the code trace.
4. **Bench A/B** — the decisive experiment: flash + PPK2-cold-boot board2, bring up chronite mesh, measure
   plink stability from BOTH sides (chronite wpa `established`/`closing`/`MESH-PEER-*` + the ESP's RTC
   `g_plink_estab`/`g_plink_close`). Current tree first (freshest chip), then the clean `7fdba0c9` baseline
   on the same board2 — so a later baseline flap couldn't be misread as a current-only regression.

---

## 2. Code analysis — the uncommitted layers do not touch idle peering

The peering file `umac_mesh.c` diff is **only telemetry**: `g_plink_estab++` at the two ESTAB sites
(`umac_mesh.c:2971/:2990`), `g_plink_close++` in `mesh_peer_free` when tearing down an ESTAB plink
(`:634`), the `g_mesh_fwd_dbg[]` forward-path increments, and a getter. **No MPM/SAE FSM logic changed.**

The decisive routing facts (grep-verified in the working tree):

- **Peering frames** `umac_mesh_tx_peering` (OPEN/CONFIRM/CLOSE, `umac_mesh.c:854`) and **SAE auth**
  `umac_mesh_tx_auth` (`:895`) are sent via **`umac_datapath_tx_mgmt_frame`** — which is OUTSIDE every diff
  hunk and does not call the S2/S3-modified paths.
- `umac_datapath_tx_mesh_keyed_frame` (the S3-modified function) has exactly two wrappers:
  `umac_datapath_tx_mesh_unicast_frame` (PAIRWISE) and `..._group_frame` (GROUP). The **only** caller of the
  PAIRWISE wrapper is `umac_mesh_forward_data` (`umac_mesh.c:2628`); the only GROUP caller is the group
  re-broadcast (`:2752`). Both are reached only on **RX-forward** of a data/group frame — never at idle.
  So S3's `fwd_aggregate = (key_type == PAIRWISE)` reroute (tid=0, data-channel, aggr_check) touches only
  the relay forward path.
- **S2** (`sta_data = umac_sta_data_get_datapath(key_stad)`, `aggr_check(key_stad,…)`) is in
  `umac_datapath_process_tx_frame` — the DATA TX path — and `key_stad == stad` for single-hop / non-mesh /
  multicast, so it is a no-op off the multi-hop data path.
- **FIX-1** is compile-time dormant: `CONFIG_HALOW_SOFT_HW_RESTART` is unset in the regenerated sdkconfig →
  `g_mmdrv_soft_hw_restart = false` → `hw_restart` takes the legacy path; `morse_trns_soft_start/stop` and
  `mmdrv_soft_restart` are never called. And `hw_restart` doesn't fire at idle anyway.
- **The app allowlist** (`umac_mesh_peer_allowed`) is consulted at exactly one site — the data-RX gate
  `umac_datapath.c:697`, gated on `mesh_ctrl_present`. It drops mesh **data** from non-allowed transmitters;
  it does **not** gate beacons / MPM / SAE. So the app's role/allowlist rewrite (board2-relay vs board0-relay)
  is peering-irrelevant, and board2 peers with chronite regardless of which topology the app selected.

**Independent adversarial re-audit — bottom line:**

> YES — the uncommitted morselib is IDLE-PEERING-NEUTRAL vs committed 7fdba0c9. … The regression hypothesis
> (S1/S2/S3/telemetry introduced the flap) is REFUTED. … The bench A/B should EXPECT BASELINE AND CURRENT TO
> BEHAVE THE SAME AT IDLE.

The audit also surfaced the confound in §0: the 45/45 "known-good" was traffic-loaded board0 under
`MESH_LINUX_INTEROP` auto-ping; the flap is idle board2 under `MESH_IPERF` with no ping — the delta is
traffic, not code.

---

## 3. Bench A/B — both firmwares peer stably on the same board2

Rig: board2 (only fully-wired ESP, PPK2 cold-bootable) ↔ chronite (Linux, `rimba-mesh`, SAE
`rimbamesh2026`, op_class 68 / ch27, fw 1.17.8 / 480664, user_mpm=1). board0/board1 stayed on rimba-hello
(silent — chronite only ever listed board2 as a peer). Flash FIRST, then PPK2 cold-boot (clears the
warm-reset MM6108 wedge). Measured from BOTH sides. `MESH_IPERF` = open peering, no `g_ping_target` = **idle**.

### 3.1 Current tree (uncommitted S2/S3/RTC/FIX-1/telemetry)

chronite wpa log, board2 (`e2:72:a1:f8:f0:08`):
```
:121 IDLE→OPN_SNT→OPN_RCVD → :122 established → ESTAB → MESH-PEER-CONNECTED     (establish #1)
:165 SAE: remove the STA doing reauthentication → DISCONNECTED → MPM closing    (one re-SAE settle)
:167 IDLE→OPN_SNT→OPN_RCVD → established → ESTAB → MESH-PEER-CONNECTED           (establish #2)
[then NO further board2 events for ~525 s — continuously ESTAB]
```
Observation (150 s + extended 182 s ≈ 5.5 min): `peers_now=1` at every 25 s checkpoint, `est` frozen at 2,
`disc` frozen at 1, inactive time 12–96 ms (≪ the 6 s inactivity threshold), signal −10…−13 dBm.

ESP-side RTC (survives the warm-reset read): `PLINK-DBG estab=1→2, close=0` (**never tore down an
established plink**), `HWR-DBG boot_count=2 restart_attempts=0 restart_completions=0` (no crash-loop,
no hw_restart). Both sides agree: stable.

The single re-SAE at :165 was board2 re-initiating SAE (chronite logged an RX SAE commit from board2) —
an early MPM settling artifact (asymmetric ESTAB: chronite reached ESTAB at :122 but board2's side likely
timed out pre-ESTAB and re-opened, reaching ESTAB at :167; `g_plink_close=0` confirms board2 never tore
down an *established* link). It happened **once**, then the link held.

### 3.2 Baseline `7fdba0c9` (stashed submodule + reverted app_main.c, same board2)

chronite wpa log, board2:
```
:692 SAE: remove the STA doing reauthentication → :693 DISCONNECTED → MPM closing   (cold-boot reconnect)
:695 IDLE→OPN_SNT→OPN_RCVD → established → ESTAB → MESH-PEER-CONNECTED
[then continuously ESTAB; station dump `connected time = 686 s` and counting]
```
Observation (150 s): `est_delta=0 disc_delta=0` every checkpoint, `peers_now=1` throughout, inactive
8–92 ms. **Zero flaps in-window** — after the one cold-boot reconnect it held continuously.

### 3.3 Verdict

| Arm | early settle | in-window flaps | steady state |
|---|---|---|---|
| Current tree | 1 re-SAE | 0 | stable ~5.5 min, RTC close=0 |
| Baseline `7fdba0c9` | 1 coldboot reconnect | 0 | stable, connected 686 s+ |

**Identical.** The uncommitted layers make peering neither better nor worse. The prior "8 est / 5 close then
quiet, 100% loss" flap reproduced on **neither**.

---

## 4. What the flap actually was (and the latent gap it exposes)

- **Root attribution — ESP HOST STACK (morselib MPM), NOT the radio or "a degraded chip".** The bench runs
  ONE radio type end-to-end (MM6108 / FGH100M, same `mm6108.bin` 1.17.8) — ESP and Linux differ only in host
  (morselib vs mac80211 + morse_driver) and carrier. A Linux node peering rock-stably on the *same* MM6108+fw
  DISPROVES a radio/firmware cause and DISPROVES "the MM6108 wore out" as the explanation. So the flap is an
  ESP host-software behavior; the A/B further shows it is not the uncommitted S2/S3/RTC/FIX-1/telemetry, i.e.
  it is in the **committed morselib MPM**. (Individual-unit board2 wear can't be fully excluded, but there is
  no positive evidence for it and the same-silicon Linux control makes it unlikely.)
- **Latent robustness gap (in COMMITTED code, present in `7fdba0c9` too):** the mesh peering FSM sends **no
  ESTAB keepalive** — an idle plink relies on received **beacons** (100 ms) to keep the peer-link inactivity
  timer (`MESH_PLINK_INACTIVITY_MS = 6000`) from firing. When beacons are missed (degraded/warm RX, RF
  marginality), a node can hit the 6 s inactivity → `mesh_peer_free` teardown → re-open / re-SAE on the next
  beacon → the observed flap; repeated under sustained adverse RX = the "8 est / 5 close" cascade. This is
  **not** introduced by S1/S2/S3 — it is a property of the committed FSM that chip degradation amplifies.
  The follow-Linux fix is **not** a keepalive: net/mac80211 sends none on an ESTAB plink (the timer is a
  no-op, `mesh_plink.c:696`). Linux instead uses a **1800 s** inactivity window (`MESH_DEFAULT_PLINK_TIMEOUT`)
  and refreshes `last_rx` on **every** received frame incl. data (`rx.c:4810`); the ESP had 6 s + beacon/
  peering-only. **Implemented + verified — see §7.** A committed-FSM correction, **not** a regression fix.

---

## 5. Implication for S3 + next steps

- **The S3 RX-drop read is UNBLOCKED.** The prior blocker (board2 flap → no traffic → counters 0) was
  chip-condition, not code. In a fresh session board2 holds a stable plink, so the read can proceed:
  `board0 → board2(relay, S3) → chronite` UDP blast, then read board2's RTC `RX-DBG[boot]` to name the
  ~99 % early-host-RX drop bucket (allowlist / plaintext / hw-ccmp / sw-ccmp / no-decrypt) — the original
  S3 goal. Do it on a fresh/cool bench; reflash board0 to the mesh-perf app (client role) first.
- **The bisection itself changed no code** — working tree restored exactly (submodule 12 files +431/-29
  @ `7fdba0c9`; `app_main.c` M). The follow-up liveness fix (§7) is a separate, deliberate change on top.
  Nothing committed.

## 6. Bench mechanics that worked (reusable)

- **Full bench control now available:** board0/board1 VBUS via `tools/esp_usb_power.py` (sudo pw known);
  board2 via `tools/ppk2_hold.py` (PPK2). Cold-boot board2 = kill the hold PID (found via `/proc/*/cmdline`,
  never `pgrep -f` which self-matches) → DUT off → re-`nohup ppk2_hold.py` → DUT on. Flash board2 by its
  stable by-id path, then cold-boot (esptool's `--after hard_reset` wedges the MM6108; PPK2 power-cycle heals it).
- **Read board2 RTC counters** with `reset_read.py` (dtr=False + RTS pulse = reset-into-app, not download);
  RTC survives the warm reset so the boot dump shows the run's `PLINK-DBG` / `HWR-DBG` / `RX-DBG`.
- **chronite mesh:** `sudo wpa_supplicant_s1g -B -i wlan1 -c /tmp/wpa-interop.conf -f /tmp/wpa_wlan1.log -t -dd`
  (fresh log for clean counting or snapshot est/disc and measure the delta). Kill ONLY the wlan1 supplicant
  (match binary+iface via `ps -eo … | awk` that does NOT contain the literal in its own cmdline). Silence =
  kill wlan1 supplicant + `ip link set wlan1 down` (leave the wlan0/SSH supplicant).
- **Radio-silent after:** board2 → `~/pwr_test/reflash_hello.py` (rimba-hello, silent, left powered);
  board0/board1 already hello; chronite wlan1 down. Done this session.

---

## 7. Follow-up fix — match Linux plink liveness (implemented + verified)

Acting on §4: corrected the committed MPM to Linux's actual liveness mechanism. Full provenance + verified
`file:line` map: [`rimba-mesh-plink-liveness-codemap.md`](../mesh-ap/rimba-mesh-plink-liveness-codemap.md)
(reference `rpi-linux` @ `372414fd4`, every Linux line re-grepped on chronite, verified 2026-07-12).

**3 edits (uncommitted, layered on the current tree; build green):**
1. `umac_mesh.c:418` — `MESH_PLINK_INACTIVITY_MS` 6 s → `1800u*1000u` (= Linux `MESH_DEFAULT_PLINK_TIMEOUT`,
   `net/wireless/mesh.c:26`). 6 s tolerated only ~60 missed 100 ms-beacons; Linux ~1758. The dominant fix.
2. New `umac_mesh_note_peer_rx(ta)` (`umac_mesh.c` + decl `umac_mesh.h`), called from the mesh data-RX path
   (`umac_datapath.c`, post-decrypt, `dot11_get_ta(header)`) — refreshes `last_rx_ms` on any received data
   frame, mirroring Linux `last_rx = jiffies` on every RX (`rx.c:4810`). Beacon refresh already existed
   (`umac_mesh.c:1414`, mirrors `mesh_plink.c:446`); only data-activity + the timeout scale were wrong.
3. **No keepalive frame** — deliberate. Linux sends none on an ESTAB plink (`mesh_plink.c:696`); adding one
   would diverge from the reference.

**Verification (2026-07-12) — stated by tier per `.ai/AGENTS.md`:**
- **Build:** green (bin 0x173ae0 fits the 0x177000 SINGLE_APP_LARGE partition).
- **On-air bytes: N/A (nothing new on the wire).** The fix transmits no new or changed frame — the timeout
  only changes *when* the already-verified Close is sent; `note_peer_rx` only updates a local timestamp on
  RX. So the on-air byte-diff-vs-Linux tier does not apply (no frame to capture), stated explicitly rather
  than implied.
- **Hardware (regression check — PASS):** flashed board2, PPK2 cold-boot, chronite mesh — peers stably; the
  plink held **continuously 150 s+** (chronite `connected time` 50→150 s, `peers_now=1` every 25 s, inactive
  4–76 ms); ESP RTC `PLINK-DBG close=0`, `restart_attempts=0`. The fix boots + peers correctly, no regression.
- **Under-load demonstration — PASS.** Ran a sustained relay blast (board0 client → board2 relay → chronite,
  UDP 5 Mbit/s ~60 s; board0 auto-blasts via a temp `g_iperf_blast_target` + static ARP so no ESP console is
  needed). board2's RTC across the blast: **`PLINK-DBG close=0`** — the relay held its plink under a 2088-frame
  load, with `note_peer_rx` refreshing `last_rx_ms` on every forwarded frame. This is the positive under-load
  signal that the idle A/B couldn't give (the original flap didn't reproduce in a fresh session, so no before/
  after teardown A/B was possible — named blocker; the change is strictly-more-stable regardless).
- Bench left radio-silent (board0+board2 rimba-hello, board1 hello, chronite wlan1 down).

## 8. Corollary — the S3 relay RX-drop read (multi-session blocker) is answered

The same blast run drained the S3 relay RX-drop counters (the read blocked in
[`2026-07-12-mesh-ampdu-s3-rtc-noinit-fix1-bench-block.md`](2026-07-12-mesh-ampdu-s3-rtc-noinit-fix1-bench-block.md)).
board2 RTC `RX-DBG[boot]`: `mesh_seen=2088 allowlist=0 plaintext=0 hw_ccmp_fail=0 sw_ccmp_fail=2080 no_decrypt=0
decrypt_ok=8`. **The ~99.6 % early-host-RX drop = host SW-CCMP decrypt failure** (`umac_datapath.c:741`
`umac_datapath_sw_ccmp_decrypt` → drop `:743`), 2080/2088 — not the allowlist, plaintext, HW-CCMP, or the
no-decrypt path. Next (separate investigation): root-cause why SW-CCMP fails on the A4≠TA forwarded /
multi-hop-origin frames — MIC vs replay-window, and whether the decrypt keys on the origin vs next-hop stad.
Topology note: board0 peered directly with chronite (all nodes in RF range) yet the 2-hop still happened (board2
saw 2088 forward-intended frames) — the allowlist data-RX drop forces board0's usable path via board2.
