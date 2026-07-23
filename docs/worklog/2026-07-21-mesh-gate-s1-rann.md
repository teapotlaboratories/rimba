# 2026-07-21 — Mesh-gate port S1 (gate RANN TX): written, compiles, **VERIFIED ON-AIR**

**Status:** S1 firmware WRITTEN + COMPILES + **ON-AIR BYTE-DIFF VERIFIED (PASS)** — the ESP gate's RANN is
byte-for-byte identical to a live Linux gate's RANN, captured simultaneously on chronium's `morse0` monitor.
Still **uncommitted** in the `components/halow` submodule + the new `firmware/test-mesh-gate` fixture is
untracked. *(HTML render: TODO next session, per the worklog-render rule.)*

## ✅ VERIFICATION RESULT (2026-07-21, added after the on-air bench run)

**PASS — gold-standard live A/B.** Both gates emitted RANN on ch27 at once; chronium (monitor) captured both;
the ESP frame is byte-identical to the Linux frame in every field that must match.

Capture (chronium `morse0`, `tools/`-style AF_PACKET reader, full recipe below):

| field | Linux gate (chronite `3c:22:7f:37:51:38`) | ESP gate (board0 `e2:72:a1:f8:ef:a4`) | verdict |
|---|---|---|---|
| FC | `d000` | `d000` | **MATCH** (mgmt/action, protected=0) |
| Duration | `0000` | `0000` | **MATCH** |
| addr1 DA | `ffffffffffff` | `ffffffffffff` | **MATCH** (broadcast) |
| addr2 SA / addr3 BSSID | chronite MAC | board0 MAC | differs — each gate's own MAC (correct) |
| SeqCtrl | `3013` | `b000` | per-frame counter (n/a) |
| category / action | `0d` `01` | `0d` `01` | **MATCH** (MESH=13 / HWMP_PATH_SEL=1) |
| eid / len | `7e` `15` | `7e` `15` | **MATCH** (RANN=126 / len=21) |
| rann_flags | `01` | `01` | **MATCH** (RANN_FLAG_IS_GATE) |
| rann_hopcount / ttl | `00` `1f` | `00` `1f` | **MATCH** (hop 0 / ttl 31) |
| rann_addr | chronite MAC | board0 MAC | differs — root's own MAC (correct) |
| rann_seq | `1c000000` (28) | `0c000000` (12) | differs — independent HWMP sn (correct) |
| rann_interval | `88130000` (5000) | `88130000` (5000) | **MATCH** |
| rann_metric | `00000000` | `00000000` | **MATCH** |
| FCS | `9d2eb9a8` | `5a9537ae` | CRC over content (n/a) |

Machine-checked normalized diff (mask SA/BSSID/rann_addr + the SeqCtrl/rann_seq/FCS counters → 00):
**ALL INVARIANT FIELDS IDENTICAL = True · NORMALIZED FRAMES BYTE-IDENTICAL = True → PASS.**

Raw frames:
- Linux: `d0000000ffffffffffff3c227f3751383c227f37513830130d017e1501001f3c227f3751381c0000008813000000000000` + FCS
- ESP:   `d0000000ffffffffffffe272a1f8efa4e272a1f8efa4b0000d017e1501001fe272a1f8efa40c0000008813000000000000` + FCS

**Bench setup used:** chronium = monitor (`iw dev wlan1 set type monitor` + `set freq 5560`, `morse0` up);
the Linux gate = **chronite** brought up as a `rimba-smesh` mesh via `wpa_supplicant_s1g`
(`docs/reference/captures/wpa-smesh.conf`), then made a gate at runtime with
`iw dev wlan1 set mesh_param mesh_hwmp_rootmode 4 / mesh_gate_announcements 1 / mesh_hwmp_rann_interval 5000`
(readback confirmed `rootmode=4 gate_ann=1 rann_int=5000TUs element_ttl=31` — the live gate uses exactly the
defaults S1 assumes); the ESP gate = **board0** flashed with the new `firmware/test-mesh-gate` fixture.
Both gates emit RANN as a **lone root with zero peers** (no 4th node needed). Capture script:
`scratchpad/mesh_rann_cap.py` (filters mgmt/action cat=13/act=1/eid=126, dumps hex + decoded fields per SA).

**Independent static confirmation:** a 4-lens adversarial verification workflow (byte-layout, root-frame
values, runtime-on-air-emission, regression/timer) returned **MATCH on every lens, zero blockers, go=true** —
in complete agreement with the on-air result. Its runtime lens independently proved the broadcast RANN goes
out **unprotected** (triple-safe: the `common_stad` is PMF-DISABLED so the robust-mgmt encrypt block is never
entered; a multicast `da` bypasses the inner encrypt; and a group-privacy Mesh action is exempt) — so a lone
gate with no peer transmits a capturable, in-the-clear RANN, which the bench confirmed.

**Radio-silent cleanup done** ([[radio-silent-after-every-test]]): board0 reflashed to `rimba-hello`;
chronite + chronium `wlan1` down (chronium restored to `type managed`).

## Goal

First implementation stage of the approved 802.11s **Mesh-gate** port (`docs/mesh-ap/rimba-mesh-ap-mesh-gate-discovery-design.md`). The mesh-gate lets a multi-hop HaLow mesh reach off-mesh networks (a backhaul/internet uplink) through a "gate" node discovered the standards way — replacing the mesh node's hard-coded default gateway. S1 is the **gate → mesh announcement**: a root/gate periodically floods a **RANN** (Root Announcement) so every node learns a path back to it. Follow-Linux, byte-diffable against a real Linux gate.

## What was done this session (in order)

1. **Doc reconciliation committed** — `d288ac6` on `main` (8 docs), bringing the backlog/regression/mesh-gate docs in line with the merged 2.12.3 state (see `docs/rimba-todo.md`, `docs/regression/rimba-regression-results.md`). The 2 `components/halow` version-stamps were reverted (deferred to a `mm-esp32-halow` PR — a gitlink bump needs a pushed submodule commit).

2. **Code-map re-pin (morselib side).** The 2.10.4→2.12.3 forward-port moved the morselib sources into `umac/mesh/umac_mesh.c` + `umac/datapath/umac_datapath.c` and shifted line numbers. Re-located every S1–S6 anchor by stable landmark (not line number). Confirmed **RANN is genuinely absent** (only comments reference it) → S1 is fully additive. Key 2.12.3 anchors: IE defines block @353; `HWMP_MPATH_PREQ/PREP` @360-361; `struct hwmp_frame_params` + `umac_mesh_build_hwmp` @2060-2130; `umac_mesh_tx_hwmp` chokepoint; `struct mesh_path_entry` @1847; `umac_mesh_handle_hwmp` RX dispatch @2357; `MESH_PREQ_MIN_GAP_MS` @385 (exact); A4≠TA withhold comment @139-142; SW-CCMP @474-620. *(The design doc §3 TABLE still shows the stale 2.10.4 numbers — re-pinning it fully is a follow-up; the verified anchors live in the S1 code comments.)*

3. **Linux reference obtained from the bench** (SIGN-OFF #3 partial). The bench's exact interop tree is on `chronium`: kernel **6.12.21** at `/home/chronium/halow/rpi-linux/net/mac80211/` (`mesh.c`, `mesh_hwmp.c`, `mesh_pathtbl.c`, `mesh.h`, `mesh_plink.c`) + `morse_driver`. Copied the mesh sources + `include/linux/ieee80211.h` locally to grep/cross-reference. Landmarks: `mesh_hwmp.c` `enum mpath_frame_type` @91, `mesh_path_sel_frame_tx` @100 with `case MPATH_RANN:` @146, `mesh_path_tx_root_frame` @1434; `ieee80211.h` `struct ieee80211_rann_ie` @1092, `RANN_FLAG_IS_GATE` @1103, `WLAN_EID_RANN=126` @3665. **NB: the local copy was in this session's scratchpad — gone next session; re-`scp` from the bench.**

4. **S1 written** — 85 lines (umac_mesh.c +77, umac_mesh.h +8), byte-derived from the above:
   - Defines: `DOT11_IE_RANN(126)`, `HWMP_MPATH_RANN(2)`, `HWMP_RANN_IE_LEN(21)`, `RANN_FLAG_IS_GATE(1<<0)`.
   - `umac_mesh_build_hwmp`: a `HWMP_MPATH_RANN` early-return branch. **Wire layout** (== `mesh_path_sel_frame_tx` MPATH_RANN): category `DOT11_CATEGORY_MESH(13)` / `WLAN_MESH_ACTION_HWMP_PATH_SEL`, then IE `126`, len `21`, `flags, hop_count, ttl, orig_addr[6], orig_sn(le32), interval(le32), metric(le32)`. No PREQ/PREP tail. The PREQ/PREP path is byte-untouched (RANN returns before it).
   - `umac_mesh_tx_root_frame()` (static) == `mesh_path_tx_root_frame` PROACTIVE_RANN: `orig_addr`=our mesh MAC, `orig_sn`=`++mesh_hwmp_sn`, `hop_count`=0, `ttl`=`HWMP_ELEMENT_TTL(31)`, `lifetime`=`g_mesh_rann_interval_ms` (the RANN interval — Linux passes the raw value), `metric`=0, `flags`=`RANN_FLAG_IS_GATE` iff gate. Gated on `active && multihop && root_rann`.
   - `umac_mesh_rann_tick()` (static) == `ieee80211_mesh_path_root_timer`: self-reschedules at the RANN interval; emits a RANN each tick while root mode is on (no-op otherwise). Armed at mesh start, cancelled at teardown — mirrors the existing `umac_mesh_plink_tick`. (Diverges from Linux, which arms the timer only in root mode; here it stays armed + cheaply no-ops so a runtime mode change needs no rearm.)
   - Public API `mmwlan_mesh_set_root_announcements(bool root_rann, bool is_gate, uint32_t interval_ms)` (umac_mesh.h), off by default. `interval_ms=0` keeps the current/default 5000.
   - Globals `g_mesh_root_rann` / `g_mesh_gate_announce` / `g_mesh_rann_interval_ms` (mirror `dot11MeshHWMPRootMode` / `GateAnnouncementProtocol` / `RannInterval`).

5. **Compile-verified:** `make build APP=rimba-halow-mesh BOARD=proto1-fgh100m` → **exit 0**, zero errors/warnings. (App partition 2% free — near-full, pre-existing; watch it as later S-stages add code.)

## Next step — S2 (RANN RX)

S1 (gate RANN **TX**) is now written + on-air verified (above). The next stage is **S2: RANN RX** — an
ESP node receives a RANN, records the root/gate in a `known_gates` table, and re-floods it (decrementing
TTL, adding hop/metric), mirroring Linux `hwmp_rann_frame_process` (`mesh_hwmp.c:914`) + the
Formation-Info gate bit. Design: `docs/mesh-ap/rimba-mesh-ap-mesh-gate-discovery-design.md` (S2 ~2-3d).
The S1 verification method (this fixture + `mesh_rann_cap.py` + a live Linux gate on chronite) is reusable
for S2 — inject a Linux-gate RANN and assert the ESP learns/re-floods it byte-correctly.

**S1 done; the earlier "verify S1 on-air" plan below was executed — kept for the recipe.** RANN-emitter
fixture = `firmware/test-mesh-gate` (calls `mmwlan_mesh_set_root_announcements(true, true, 5000)` after
`mmwlan_mesh_start`). Bench Linux gate = chronite via `wpa_supplicant_s1g` + runtime `iw set mesh_param`
(NOT `iw mesh join` — [[morse-linux-mesh-via-supplicant]]). Capture + byte-diff on chronium `morse0`
([[verify-onair-chronium-monitor]]). Set the ESP `interval_ms` == the bench gate's `dot11MeshHWMPRannInterval`
(default 5000) so the interval bytes match; the live chronite gate used ttl=31, metric=0, hop=0, flags=0x01.

## Footguns

- **Linux reference is not persistent** — re-`scp` from `chronium:/home/chronium/halow/rpi-linux/net/mac80211/` (+ `include/linux/ieee80211.h`) each session.
- **S1 is uncommitted** in the submodule (`git -C components/halow diff`). It lands as a `mm-esp32-halow` PR + a superproject gitlink bump (the deferred 2.12.3 version-stamps can ride along). No auto-commit; no Claude trailers.
- **Interval units:** Linux puts the raw `dot11MeshHWMPRannInterval` numeric value on the wire (labelled TU but not converted from ms). S1 does the same, so the byte-diff matches as long as both sides use the same numeric value — do NOT apply `MESH_MSEC_TO_TU` to the RANN interval.
- Don't reboot the Linux mesh nodes ([[dont-reboot-linux-mesh-nodes]]) — wipes the pushed `/tmp` wpa config.

## Files
- `components/halow/.../umac/mesh/umac_mesh.c` (+77), `.../umac/mesh/umac_mesh.h` (+8) — S1, **uncommitted** in the submodule.
- `firmware/test-mesh-gate/` — the RANN-emitter fixture (app_main.c + CMakeLists + sdkconfig.defaults), **untracked**. Derived from `rimba-halow-mesh`; calls `mmwlan_mesh_set_root_announcements(true, true, 5000)`. Emits no `TEST|` line (on-air interop fixture like `test-mesh-linux`; the verdict is the off-line byte-diff).
- `scratchpad/mesh_rann_cap.py` — the RANN capture/decoder for chronium `morse0` (session scratchpad — copy into `tools/` or `docs/reference/captures/` if kept; re-usable for S2 RX verification).
- `docs/mesh-ap/rimba-mesh-ap-mesh-gate-discovery-design.md` — the design + code-map (status updated; §3 table re-pin pending).
- Memory: `mesh-gate-8021s-port-planned` (updated: S1 verified).

## Landing (not done — no auto-commit)
S1 lands as a `mm-esp32-halow` PR (the `components/halow` submodule diff) + a superproject gitlink bump that
also brings in `firmware/test-mesh-gate` (+ the deferred 2.12.3 README/VERSION version-stamps can ride along).
No auto-commit; no Claude trailers ([[commit-attribution]]). Off-hours (commits OK time-wise) — left to the user.
