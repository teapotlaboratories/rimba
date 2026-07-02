# 802.11s mesh — new-code ↔ Linux map (morselib ↔ `net/mac80211` + `morse_driver`)

The function-level porting map for the ESP32 802.11s mesh (the
[Mesh-gate doc](rimba-mesh-ap-milestones.md) §802.11s mesh point covers status + the feature
comparison; this doc is the **side-by-side code reference** the methodology requires — AGENTS.md
rule "write the new-code ↔ Linux map"). Each row maps an ESP symbol to its Linux equivalent so a
reviewer can check the port against the source.

ESP side: `components/halow/.../morselib/src/umac/` — `mesh/umac_mesh.c` (`M:`) and
`datapath/umac_datapath.c` (`DP:`). Linux side: line numbers are against the pinned reference
revisions —

| Component | Repo / branch | Commit |
|---|---|---|
| Kernel (`net/mac80211/mesh*.c`, `dot11ah`) | `MorseMicro/rpi-linux` `mm/rpi-6.12.21/1.17.x` (Linux 6.12.21) | `372414fd` |
| `morse_driver` | `MorseMicro/morse_driver` 1.17.8 | `3eef5a0a` |

Line numbers drift with edits — they pin *where* in the source the equivalent lives at these
revisions, not a permanent address. *Every cited symbol/line was verified 2026-06-26 by grepping
both trees (ESP working tree + the chronite reference checkout).*

## Interface bring-up & beaconing

| ESP (morselib) | Linux equivalent | Notes |
|---|---|---|
| `mmwlan_mesh_start` `M:1521` (add MESH vif, `mmdrv_cfg_bss` `M:1562`, `set_bssid` `M:1575`, `start_beaconing` `M:1649`, `mmdrv_cfg_mesh` `M:1662`) | `net/mac80211/mesh.c` `ieee80211_start_mesh` `mesh.c:1173`; `morse_driver` `morse_cmd_cfg_mesh_bss` `mesh.c:84` + `morse_cmd_set_mesh_config` `mesh.c:110` | morselib drives the morse fw mesh-config/beacon cmds directly; mac80211 goes via cfg80211 |
| `umac_mesh_build_beacon` `M:117` / `umac_mesh_get_beacon` `M:174` (host-built S1G short beacon: Mesh ID 114 + Mesh Config 113 IEs) | `morse_driver` `morse_dot11ah_beacon_to_s1g` `dot11ah/tx_11n_to_s1g.c:819`; mac80211 `ieee80211_beacon_get_tim` + mesh IEs | morse fw auto-S1G's AP vifs but not MESH vifs, so the host builds the S1G beacon |

## Peering — MPM (`mesh_plink.c`)

| ESP (morselib) | Linux equivalent | Notes |
|---|---|---|
| `umac_mesh_build_peering` `M:387` / `umac_mesh_tx_peering` `M:451` (Open/Confirm/Close + Mesh Peering Mgmt IE 117) | `mesh_plink_frame_tx` `mesh_plink.c:213` | identical IE 117 layout (llid/plid LE) |
| `umac_mesh_handle_action` `M:1354` (peer-link FSM, Open→Confirm→Estab, llid/plid echo, stale-session close) | `mesh_rx_plink_frame` `mesh_plink.c:1223` (+ `mesh_plink_fsm`) | mirrors the LISTEN→OPN_SNT→…→ESTAB states |
| `mmwlan_mesh_peer_open` `M:613` (open a plink on a heard candidate) | `mesh_plink_open` + neighbour add | initiator path |
| `umac_mesh_handle_peer_beacon` `M:559` (same-Mesh-ID candidate discovery) | `ieee80211_mesh_rx_bcn_presp` `mesh.c:1456` → `mesh_neighbour_update` `mesh.c:1507` | |
| `umac_mesh_plink_tick` `M:486` (Open retransmit + holding) | `mesh_plink_timer` `mesh_plink.c:658` | one periodic tick services all peers |
| `umac_mesh_plink_tick` ESTAB-inactivity branch `M:534` → Close + flush paths | `ieee80211_sta_expire` (called from `ieee80211_mesh_housekeeping` `mesh.c:917`) + `mesh_sta_cleanup` `mesh.c:167` | reaps an idle ESTAB peer; the trigger PERR needs |

## Path selection — HWMP (`mesh_hwmp.c`)

| ESP (morselib) | Linux equivalent | Notes |
|---|---|---|
| `umac_mesh_start_discovery` `M:896` (originate PREQ, rate-limited) | `mesh_queue_preq` `mesh_hwmp.c:1080` | source role |
| `umac_mesh_build_hwmp` `M:832` / `umac_mesh_tx_hwmp` `M:879` (build PREQ/PREP) | `mesh_path_sel_frame_tx` `mesh_hwmp.c:100` | same element layout (PREQ len 37 / PREP len 31) |
| `umac_mesh_handle_hwmp` PREQ branch `M:1010` (install reverse path, reply PREP if target, else flood) | `hwmp_preq_frame_process` `mesh_hwmp.c:660` | |
| `umac_mesh_handle_hwmp` PREP branch | `hwmp_prep_frame_process` `mesh_hwmp.c:796` | install forward path, forward toward originator |
| `umac_mesh_handle_hwmp` PERR branch | `hwmp_perr_frame_process` `mesh_hwmp.c:858` | tear down path if via the PERR TA, flood on |
| `umac_mesh_build_perr` `M:944` / `umac_mesh_tx_perr` `M:968` / `umac_mesh_invalidate_paths_via` `M:984` | `mesh_path_error_tx` `mesh_hwmp.c:238` (+ `mesh_plink_broken`) | one dest per PERR; `++sn` before announce |
| `mesh_path_update` fresh-info rule `M:768` | `hwmp_route_info_get` `mesh_hwmp.c:426` | take if inactive / newer SN / equal SN + better metric |
| `umac_mesh_lookup_next_hop` **preemptive refresh** `M:1885` (added 2026-07-01) — active path within `MESH_PATH_REFRESH_MS` (6 s) of expiry → `umac_mesh_start_discovery`, path **not** invalidated | `mesh_path_refresh` `mesh_hwmp.c:1335` (from `mesh_nexthop_lookup`; trigger `now > exp_time − path_refresh_time` `:1338`, path RESOLVED; `mesh_queue_preq PREQ_Q_F_REFRESH` `:1345`) | keeps an actively-used path alive so it never expires mid-transfer then re-resolves reactively (the stall); the returning PREP renews `expiry_ms` via `mesh_path_update`. Deliberately omits Linux's separate extend-on-use (`dot11MeshHWMPactivePathTimeout` `:1350`) — the refresh PREQ keeps the path both fresh **and** topology-adaptive |

_Note: the 2026-07-01 preemptive-refresh row is grep-verified against the current tree **and on-air A/B verified** — a 75-ping multi-hop run (board1→board0→board2) timed out at ping seq **32 + 62** (the 30 s / 60 s path expiry) on the baseline (no refresh) but ran clean through both boundaries on the hardened build (73/75 replies, median ~48 ms, both builds; only the two expiry-boundary stalls differ). The older `M:` line refs elsewhere in this doc predate the SAE/AMPE growth (`umac_mesh.c` ~doubled) and need a refresh pass._

## Path table (`mesh_pathtbl.c`)

| ESP (morselib) | Linux equivalent | Notes |
|---|---|---|
| `mesh_path_find` `M:724` | `mesh_path_lookup` `mesh_pathtbl.c:268` | fixed array vs rhashtable (divergence below) |
| `mesh_path_get_or_add` `M:736` | `mesh_path_add` `mesh_pathtbl.c:680` | |
| `mesh_path_update` next-hop assign `M:768` | `mesh_path_assign_nexthop` `mesh_pathtbl.c:115` | |
| `umac_mesh_lookup_next_hop` `M:792` | mpath `next_hop` deref + active/expiry check | |

## Forwarding & data path (`mesh.c`, `umac_datapath.c`)

| ESP (morselib) | Linux equivalent | Notes |
|---|---|---|
| `construct_80211_data_header_mesh` `DP:3037` (4-addr unicast / 3-addr group addresses) | `ieee80211_fill_mesh_addresses` `mesh.c:851` | A1=nexthop A2=us A3=DA A4=SA (unicast); fromDS bcast (group) |
| Mesh Control header + QoS "Mesh Ctrl Present" insert `DP:1896-1961` | `ieee80211_new_mesh_header` `mesh.c:884` | flags/ttl(31)/seqnum |
| `process_rx_data_frame_after_reorder` strip + dispatch `DP:500` (`mesh_ctrl_present` `DP:542`, strip `DP:660`, forward `DP:692/704`) | `ieee80211_rx_mesh_fast_forward` `rx.c:2762` (+ the mesh-data RX fwding path `rx.c:~2900-2990`) | |
| `umac_mesh_forward_data` `M:1214` / `umac_mesh_build_forward` `M:1188` (relay unicast, A4 preserved) | mac80211 mesh forward (path lookup → 4-addr re-TX) | |
| `umac_mesh_handle_group_data` `M:1322` / `umac_mesh_build_rebcast` `M:1295` (re-broadcast group) | mac80211 mesh group forward | |
| `mesh_rmc_seen` `M:1266` (per-(SA,seqnum) dup cache) | `mesh_rmc_check` `mesh.c:223` | suppresses bcast loops |

## The three on-air value fixes (live-Linux A/B, worklog Updates 14–15)

| ESP (morselib) | Linux equivalent | What it aligns |
|---|---|---|
| `umac_mesh_start_discovery` `M:896` `target_flags = (refresh) ? MESH_PREQ_TO_FLAG : 0` | `mesh_queue_preq` `mesh_hwmp.c:1206-1207` (`PREQ_Q_F_REFRESH → IEEE80211_PREQ_TO_FLAG`) | PREQ Target-Only flag on refresh |
| `MESH_MSEC_TO_TU(MESH_PATH_LIFETIME_MS)` in the PREQ | `MSEC_TO_TU` `mesh_hwmp.c:69` + `default_lifetime` `mesh_hwmp.c:82` | PREQ lifetime field in TUs, not raw ms |
| group QoS `|0x0020` (No-Ack) — `umac_mesh_build_rebcast` `M:1314` + originate path `DP:1966` | `ieee80211_set_qos_hdr` `wme.c:193` (multicast → No-Ack at `wme.c:226`) | group/broadcast ack policy |

## Deliberate divergences (platform/embedded, not ports)

- **No rhashtable / RCU.** The path table (`mesh_paths[8]`), peer table, and RMC (`mesh_rmc[16]`)
  are small fixed arrays under the single umac event-loop, not mac80211's RCU rhashtables. Sized
  for a small bench mesh; a real deployment would grow/replace these.
- **Host-timer scaffolding.** umac has no per-object timers; one periodic `umac_mesh_plink_tick`
  (`M:486`) services all peers (retransmit + inactivity), vs mac80211's per-sta `mesh_plink_timer`
  + housekeeping timer.
- **Firmware-served beaconing / timing.** TBTT, MBCA, and beacon TX are handled by the morse
  firmware (`mmdrv_cfg_mesh`); mac80211 owns this in software (TSF). The host only builds the
  beacon contents.
- **Fixed metric (not airtime).** `MESH_PATH_LINK_METRIC` is a fixed per-hop cost; mac80211 uses
  `airtime_link_metric_get`. Accumulates correctly but isn't rate-derived (P6c).
- **No proxy/gate, RANN/root, AMPE/SAE, mesh-PS.** Open mesh only; those `mesh_hwmp.c` /
  `mesh_ps.c` / proxy paths are not ported (P6c backlog).
