# Worklog — 2026-06-24 — 802.11s mesh port: recon & effort estimate

**Author:** Aldwin (with Claude Code)
**Goal:** estimate the work to port Linux 802.11s mesh into morselib (ESP32-S3 +
MM6108), following the morse Linux kernel/driver flow exactly. **No code** — recon
only, to size the feature and decide scope before committing.
**Status:** done. **Verdict: this is by far the largest feature in the project** —
~8k lines of kernel-coupled code across 6 subsystems, on a stack that has none of
the kernel primitives (rhashtable / RCU / cfg80211 / host timers) the Linux mesh
code leans on. The firmware, however, **already supports mesh** (commands defined;
ESP32 `.mbin` matches the Linux blob) — which removes the single biggest risk.

Sources: `net/mac80211/mesh*.c` + `morse_driver/mesh.{c,h}` + `morse_commands.h` on
chronium (`~/halow`, `morse_driver` 1.17.8 / `rpi-linux`); morselib at
`components/halow/.../morselib/src` (Explore inventory, full-tree greps + direct reads).

---

## The two-layer Linux structure (and why it matters)

802.11s on Linux is **two layers**, and morselib has the equivalent of *neither*:

1. **Generic protocol — `net/mac80211/mesh*.c` (6,913 lines).** The actual 802.11s:
   peering, routing, path table, sync, power-save. Host-side, in the mac80211 stack.

   | File | Lines | What it is |
   |---|---|---|
   | `mesh.c` | 1,815 | MBSS bring-up, Mesh ID/Config IEs, beaconing, IE parse, glue |
   | `mesh_hwmp.c` | 1,467 | HWMP routing — PREQ/PREP/PERR/RANN, path selection, airtime metric |
   | `mesh_plink.c` | 1,260 | Peer Link Mgmt (MPM) state machine — OPEN/CONFIRM/CLOSE |
   | `mesh_pathtbl.c` | 1,106 | mpath/mpp path tables (built on **rhashtable**) |
   | `mesh_ps.c` | 612 | Mesh power-save (peer-service-period buffering) |
   | `mesh_sync.c` | 215 | TSF neighbour sync |
   | `mesh.h` | 438 | per-vif `ieee80211_if_mesh` state, structs |

2. **Morse S1G glue — `morse_driver/mesh.{c,h}` (1,006 lines).** Adapts the generic
   mesh to S1G/HaLow and to firmware offload: MBCA (beacon-collision avoidance),
   dynamic RSSI-based peering, mesh probe req/resp, Mesh ID IE insertion into S1G
   beacons, beacon-timing element, and the **firmware commands** below.

**Crucial:** on Linux the *protocol* is host-side (mac80211); the *firmware* only does
beaconing/MBCA/peer-link limits via commands. morselib has **its own umac, not
mac80211** — so there is no mesh scaffolding to hook into. Porting "the Linux flow"
means bringing the mac80211 mesh MLME into a stack that lacks mac80211's foundations.

## Firmware already supports mesh — the big de-risk

`morse_driver` issues real firmware commands for mesh (`command.c` / `morse_commands.h`):

- `MORSE_CMD_ID_MESH_CONFIG` (START/STOP opcodes) — create/stop the mesh BSS
- `MORSE_CMD_ID_SET_MESH_CONFIG` — Mesh ID, peer-link min/max, beaconless mode
- `MORSE_CMD_ID_DYNAMIC_PEERING_CONFIG` — RSSI-based auto-peering
- `MORSE_CMD_INTERFACE_TYPE_MESH` (=5) accepted by `ADD_INTERFACE`
- MBCA TBTT selection/adjustment knobs (`MESH_MBCA_CFG_TBTT_*`)

The ESP32 MM6108 `.mbin` (1.17.6) was already found byte-identical to the Linux blob
for the TWT path; mesh commands are defined in the same firmware command set. So the
firmware very likely **already implements mesh** — unlike TWT install (`0x26`), which
the firmware *gates* to STA vifs. This is the inverse of the TWT surprise: the risk
here is host-side volume, not a firmware wall.

> **De-risk step 0 (do first, ~1 day):** prove it on hardware. morselib already has the
> probe hook `mmprobe_add_iface_raw(5, …)` (`internal/mmdrv.h:304`) made exactly for
> this. Add the `MESH_CONFIG` START command wrapper and confirm the ESP32 firmware
> ADD_INTERFACE(type=5) + MESH_CONFIG returns success and beacons. If it does, the port
> is "only" a large host-side job. If it doesn't, mesh is firmware-blocked and the whole
> effort changes — so this gate comes before anything else.

## What morselib already has (reusable) vs. what's missing

**Reusable (good news):**
- Generic action-frame build + RX **category dispatch** (`frames/action.c`,
  `umac_datapath.c:236`) — TWT plugged in here; mesh peering/HWMP categories slot in
  the same way. TWT is a working template.
- Host-built beacon with **opaque head/tail** (arbitrary IEs already carriable) +
  firmware-IRQ beacon engine (`umac_ap.c:289`, `driver/beacon/beacon.c`).
- **Generic, id-agnostic IE build/parse** (`ies_common.h`) — Mesh ID/Config/HWMP IEs
  can be emitted/parsed once constants are added.
- **IBSS** (`umac/ibss/`, `MORSE_CMD_ID_IBSS_CONFIG 0x35`) — an association-less,
  self-beaconing vif with an 8-peer table + callbacks. The nearest architectural
  precedent for a mesh vif and neighbour table.
- 4-address header struct (`dot11_data_hdr` + to_ds/from_ds accessors).
- Uniform firmware-command mechanism (`morse_cmd_tx` + `driver/driver.c` wrappers).
- SAE crypto primitives in the hostap shim (for later AMPE).

**Missing — must build (the work):**
1. **Interface plumbing** — MESH in all three type enums (`morse_commands.h`,
   `mmdrv.h`, `umac_interface.h`) + a `umac/mesh/` bring-up module. *Small–med.*
2. **Mesh firmware-command wrappers** — `MESH_CONFIG` / `SET_MESH_CONFIG` /
   `DYNAMIC_PEERING_CONFIG` defs + `driver/driver.c` wrappers (firmware side exists).
   *Small* (gated on de-risk step 0).
3. **Mesh IE constants + structs + parsers** — Mesh ID, Mesh Configuration, Peering
   Management, HWMP PREQ/PREP/PERR/RANN, Beacon Timing. Machinery exists; structs
   don't. *Medium.*
4. **Host-timer scaffolding in umac** — umac uses **zero** `mmosal_timer` today; it's
   all firmware-IRQ/schedule driven. Mesh needs many host timers (plink retry, HWMP
   PREQ retransmit, path expiry, sync). Foundational. *Medium.*
5. **Peer Link Management (MPM)** — port `mesh_plink.c`. Open-mesh peering state
   machine. *Large.* (AMPE/authenticated peering deferred — open mesh first.)
6. **HWMP routing** — port `mesh_hwmp.c` + the airtime link metric. *Large.* Needs (4)
   and (7).
7. **Path table** — port `mesh_pathtbl.c`; **replace rhashtable** with a morselib data
   structure (only fixed arrays / 8-entry LRU exist today). *Med–large.*
8. **4-address mesh data path + forwarding** — mesh control header (absent), 4-addr
   mesh framing, **forwarding/relay + mesh TTL**. morselib's datapath is strictly
   endpoint (STA/AP/IBSS) — no relay logic anywhere. *Large.*
9. **Morse S1G glue** — port the relevant half of `morse_driver/mesh.c`: Mesh ID IE in
   S1G beacon, mesh probe req/resp, beacon-timing element, (MBCA + dynamic peering can
   be minimised for MVP). *Medium.*
10. **Mesh sync / mesh PS** — `mesh_sync.c` (maybe fw/MBCA-assisted) + `mesh_ps.c`.
    *Deferrable for an MVP.*

**Absent kernel foundations the port must stand in for:** `rhashtable` (path table),
RCU, the cfg80211 mesh-config layer, kernel `timer_list`/workqueues, and the airtime
metric — ~170 references across `mesh.c`/`mesh_pathtbl.c`/`mesh_hwmp.c` alone. Each
becomes a morselib equivalent (mmosal_calloc tables, mutexes, `mmosal_timer`).

## Effort verdict & suggested phasing

**Size:** the largest single feature in Rimba to date — porting ~8k lines of tightly
kernel-coupled code, plus building the timer + table foundations mesh assumes. Bigger
than the TWT responder, STA-scaling, and action-frame TWT efforts combined. Not a
one-PR job; it needs to be staged.

Suggested phases (each its own branch + PR + HW validation, following Linux exactly):

- **P0 — Firmware feasibility probe** (de-risk step 0). Gate. *~1 day.*
- **P1 — Mesh vif up + beacon.** Interface plumbing, fw command wrappers, Mesh
  ID/Config IEs in an S1G beacon; a mesh node that beacons + is seen by a Linux mesh
  node (interop oracle: chronium `iw mesh`). No peering yet.
- **P2 — Peering (open MPM).** Port `mesh_plink.c` (open, no AMPE) + the peer table +
  the timer scaffolding. Two ESP32 nodes establish a peer link; Linux node peers with
  an ESP32 node.
- **P3 — Path table + HWMP.** Port `mesh_pathtbl.c` + `mesh_hwmp.c`. Routing converges
  on a 3-node line; PREQ/PREP observed.
- **P4 — 4-addr forwarding datapath.** Mesh header + relay + TTL. End-to-end ping
  across a 3-node mesh where the middle node forwards.
- **P5 — Hardening.** MBCA, dynamic peering, mesh sync, mesh PS, AMPE (encrypted mesh).

**Decision input:** this is the all-ESP32-relay enabler, but it's a multi-PR
undertaking. If the goal is only "a relay that leaves can TWT-sleep under," the
**Mesh-gate already works with the AP half on ESP32 + mesh on a Linux relay** today —
full ESP32 mesh (P1–P5) is what removes the Linux relay dependency. Worth weighing P0
(cheap) before committing to P1+.

## P0 result (executed 2026-06-24) — ✅ firmware recognizes MESH(5)

Built a diagnostic app `firmware/rimba-halow-mesh/` that asks the blob directly. It
sends `ADD_INTERFACE` for a matrix of interface types (each add immediately removed
via the new `mmprobe_iface_type_supported()` helper, so slots aren't exhausted) and
reports the firmware verdict per type. Flashed to one board, MM6108 fw **1.17.6**:

| Type | `fw_status` | Verdict |
|---|---|---|
| STA (1) | 0 | accepted (known-good control) |
| AP (2) | 0 | accepted (known-good control) |
| ADHOC (4) | 0 | accepted (known-good — matches IBSS recon) |
| **MESH (5)** | **0** | **accepted** |
| BOGUS (99) | 4294967274 = `0xFFFFFFEA` = **-22 (EINVAL)** | rejected (known-bad control) |

Stable across two passes. The firmware **actively rejects** an undefined type
(BOGUS → -22), so MESH(5) returning `fw_status=0` like the real types — and unlike
BOGUS — means the firmware genuinely recognizes a mesh interface. This is the inverse
of the TWT `0x26` gating: mesh is **not** firmware-blocked at the interface level.

**Gate passed → P1 unblocked.** (Caveat: this only proves `ADD_INTERFACE(MESH)` is
accepted. The next firmware unknown is whether `MESH_CONFIG`/`SET_MESH_CONFIG` are
honoured and the node actually beacons — that's the first task inside P1, but the
expensive risk, "does the firmware know mesh at all", is now retired.)

Evidence: `/tmp/mesh-p0.log`; app `firmware/rimba-halow-mesh/main/app_main.c`; helper
`morselib/src/driver/driver.c` `mmprobe_iface_type_supported()`.

## Next

- **P1 — mesh vif up + beacon.** Interface-type plumbing (3 enums) + `MESH_CONFIG`
  command wrapper + Mesh ID/Config IEs in an S1G beacon; confirm a Linux `iw mesh`
  node sees the ESP32 node. First in-P1 step: verify `MESH_CONFIG` START is honoured
  (the remaining firmware unknown).
