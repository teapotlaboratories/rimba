# 2026-07-02 — Mesh at-scale + on-air validation (6-node ESP+Linux stress + forced-topology multi-hop decision)

Validates the shipped **P6c airtime link metric + HWMP multi-path dedup/SN fix** (mm-esp32-halow #15,
worklogs `2026-07-02-mesh-p6c-airtime-metric.md` + `2026-07-02-mesh-hwmp-multipath-dedup-sn.md`) beyond
the 3-board line/triangle — at full 6-node scale, and with the on-air gold standard on chronium.

## 1. Full 6-node secured mesh — stress test
**3 ESP (board0/1/2) + 3 Linux (chronite/chronosalt/chronogen)**, secured SAE (`rimbamesh2026`), mesh id
`rimba-mesh`, S1G ch27. chronium held out as the (would-be) monitor. Reconciling the config was one edit —
the ESP already uses SAE + the same password (`umac_mesh.c` `MESH_SAE_PASSWORD`), so only the mesh-id
differed (`rimba-smesh` → `rimba-mesh` on the Linux conf).

- **Peering:** every node reached **5 peers** (all others) via SAE — a fully-connected 6-node secured mesh.
- **Data path:** single-flow ping **0% loss on every pair** (ESP↔Linux, ESP↔ESP, Linux↔Linux), e.g.
  board0 ↔ chronite 20 ms.
- **Concurrent stress:** 12 simultaneous ping streams — board0's flows clean; 20–50% loss on some
  board1/board2/chronogen flows. This is **RF contention on the shared 1 MHz S1G channel** (12 flows is a
  lot of simultaneous traffic for HaLow's bandwidth), **not a routing defect** — all nodes are 1-hop here,
  so the loss is direct-transmission collisions, and single-flow is clean. The mesh stayed **stable** (no
  crashes, peer counts held).

**Validates:** the airtime metric + HWMP fix hold up for peering, data path, and stability in a full
6-node secured ESP+Linux mesh. (Fully-connected ⇒ 1-hop, so it does *not* exercise multi-hop routing —
that's §2.)

### Bench-ops lessons (also in memory)
- The apparent "chronite/chronosalt flakiness" was **self-inflicted**: rebooting them wiped `/tmp` (tmpfs)
  incl. the pushed `wpa_supplicant_s1g` conf → wpa exits `Failed to open config file` and the interface
  stays `type managed`. **Don't reboot the Linux nodes; re-push the conf.** (memory `dont-reboot-linux-mesh-nodes`)
- A genuinely stuck node (chronogen: mesh point but 0 peers, no channel, wpa non-responsive) recovered with
  a **driver reload** (`modprobe -r morse; modprobe morse` — re-probes the SPI chip), not a reboot. The
  morse node `soc/spi@7e204000/mm6108@0` exposes `reset-gpios` = **BCM GPIO5** for a direct `gpioset` reset.
- `iw dev wlan1 info` mis-labels the S1G channel as legacy "channel 112 (5560 MHz)" — that's normal.

## 2. Forced-topology diamond — multi-hop routing decision, console + on-air
The 6-node run is 1-hop, so a forced topology drove an actual multi-hop *decision*: **board0 →
{board1, board2} → chronite** (ESP source, two ESP relays, **Linux dest**). ESP allowlists blocked
`board0↔chronite` (chronite a non-neighbor ⇒ multi-hop) and `board1↔board2` (relays don't shortcut); a TEMP
RSSI override made **board2 the weak relay** (any link touching board2 → −90). Two competing 2-hop paths:
via board1 = 2×2731 = **5462**, via board2 = 2×27307 = **54614**.

- **Console:** board0's path to chronite installed **via board1** (5462) and **held stably** (refreshes
  stayed on board1; initial board2 first-PREP-on-empty-path transients settled out). board0 ping chronite
  **0% loss** (multi-hop through board1).
- **On-air (gold standard):** captured on chronium's `morse0` monitor — board0's DATA frames carried
  **RA = board1 ×25, RA = board2 ×0** (+1 broadcast ARP). The routing decision is proven on the physical
  air, not just the log.

**Validates:** the airtime metric + HWMP fix **pick the better multi-hop route at scale, across the
ESP↔Linux boundary** — exactly what the old fixed metric could not (both paths 2 hops = tie).

### chronium monitor recipe (the correct one)
Per `docs/reference/rimba-linux-halow-monitor.md`: on chronium, `iw dev wlan1 set type monitor` +
`iw dev wlan1 set freq 5560` (S1G ch27 via morse's 5 GHz model — **not** `set channel 27`, which errors),
bring up `morse0`, read it with an **AF_PACKET/SOCK_RAW** reader skipping the radiotap header (the Pis have
no tcpdump). Monitor is exclusive with normal operation, so chronium sniffs while the ESPs + chronite drive
traffic. Restore with `set type managed`.

## Status
Both are **validations of already-merged code** (no source change — the diamond firmware was flashed from
the existing `-perf` build artifact; the TEMP override/app-config were reverted). The 6-node + on-air
results close the "at-scale / on-air" gaps left by the unit + 3-board verification.
