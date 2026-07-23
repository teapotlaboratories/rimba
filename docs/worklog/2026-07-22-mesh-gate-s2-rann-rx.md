# 2026-07-22 — Mesh-gate port S2 (RANN RX + gate list + Connected-to-Gate beacon bit): VERIFIED ON-AIR

**Status:** S2 firmware WRITTEN + COMPILES + **ON-AIR VERIFIED (PASS, all 3 deliverables)** — an ESP mesh
node receives a live Linux gate's RANN, records the gate, re-floods it byte-correctly, and advertises the
Connected-to-Gate beacon bit. **Uncommitted** in the `components/halow` submodule; new `firmware/test-mesh-gate-rx`
fixture + `tools/mesh_beacon_cap.py` untracked. *(HTML render: TODO next session, per the worklog-render rule.)*

## Goal

Second stage of the approved 802.11s **Mesh-gate** port (`docs/mesh-ap/rimba-mesh-ap-mesh-gate-discovery-design.md`),
following S1 (gate RANN TX, verified 2026-07-21). S2 is the **RANN receive side, on ALL nodes**: a node that hears
a Root Announcement learns a path back to the root, records it as a **gate** if `RANN_FLAG_IS_GATE` is set, kicks a
root-confirmation PREQ to build a real forwarding path, **re-floods** the RANN (TTL−1, hop+1, accumulated metric),
and advertises "Connected to Mesh Gate" in its beacon. Follow-Linux, byte-diffable — a port of
`net/mac80211/hwmp_rann_frame_process` (kernel 6.12.21, the bench interop tree).

## What was implemented (components/halow, uncommitted — `git -C components/halow diff`)

`umac_mesh.c` (+~187 for S2; combined S1+S2 = 264 ins) + `umac_mesh.h` (+6 for S2):
- **`mesh_path_entry` gate/root fields:** `is_root`, `is_gate`, `rann_snd_addr[6]`, `rann_metric`,
  `last_preq_to_root_ms` (== fields on Linux `struct mesh_path`). memset-zero init → non-gate/non-root by default.
- **RANN RX handler** (a `DOT11_IE_RANN` branch in `umac_mesh_handle_hwmp`, before PREQ) — the port of
  `hwmp_rann_frame_process`: parse `flags/hop/ttl/rann_addr/seq/interval/metric`; `hopcount++`; ignore our own
  RANN; require the sender be a known peer (== Linux `sta_info_get`); `new_metric = orig_metric + last_hop`
  (overflow-clamped); `mesh_path_get_or_add`; **freshness gate** `!MESH_SN_GT(orig_sn, sn) && !(sn==orig_sn &&
  new_metric<rann_metric)` (== Linux `!SN_LT(mpath->sn,orig_sn) && !(…)`); update `sn/rann_metric/is_root/
  rann_snd_addr/expiry_ms`; **root-confirmation PREQ** (`umac_mesh_start_discovery`, gated to
  `MESH_ROOT_CONFIRM_INTERVAL_MS`=2000); `mesh_path_add_gate` if gate; **re-flood** (TTL≤1 stop, else TTL−1,
  hop+1, `new_metric`, orig addr/sn/interval/flags kept) via `umac_mesh_tx_hwmp`.
- **Gate list:** `mesh_path_add_gate` (idempotent, `g_mesh_num_gates++` == Linux `-EEXIST`/`num_gates++`);
  `mmwlan_mesh_gate_count()` public accessor (== `mesh_gate_num`); eviction decrement in `mesh_path_get_or_add`;
  reset in `mesh_path_tbl_reset`.
- **Beacon Mesh Config IE now dynamic** (`mesh_formation_info_byte` + `mesh_capability_byte`, forward-declared
  before `mesh_build_config_ie`): Formation Info = `(neighbors<<1) | connected_to_gate(bit0)` (== Linux
  `mesh_add_meshconf_ie`), Capability = `ACCEPT_PLINKS(0x01) | FORWARDING(0x08 if multihop)`.

**Fixture:** `firmware/test-mesh-gate-rx` — a plain (non-root) `rimba-smesh` secured-mesh relay node that peers
with the Linux gate (SAE, password `rimbamesh2026` compiled into morselib), logs `estab_peers` + `known_gates`.
Derived from `test-mesh-linux` (peering trigger: beacon cb → `mmwlan_mesh_peer_open`) + `test-mesh-gate`.

Compile-verified `make build APP=test-mesh-gate-rx BOARD=proto1-fgh100m` → exit 0 (app partition ~2% free — the
pre-existing near-full state; S2 fit).

## ✅ VERIFICATION — on-air, all 3 S2 deliverables PASS

Bench: **chronite** = Linux `rimba-smesh` gate (`wpa_supplicant_s1g` + `iw dev wlan1 set mesh_param
mesh_hwmp_rootmode 4 / mesh_gate_announcements 1 / mesh_hwmp_rann_interval 5000`), MAC `3c:22:7f:37:51:38`, mesh
`10.9.9.2`; **board0** = ESP `test-mesh-gate-rx`, mesh MAC `e2:72:a1:f8:ef:a4`; **chronium** = `morse0` monitor
(ch27/5560). Captured with `tools/mesh_rann_cap.py` (RANN) + `tools/mesh_beacon_cap.py` (beacon Mesh Config IE).

**1. RANN RX → gate learned.** board0 peered with chronite (SAE+AMPE, `estab_peers=1 peer[0]=3c:22:7f:37:51:38`)
and logged **`known_gates=1`** within ~15 s — it received chronite's RANN, passed the freshness gate, and ran
`mesh_path_add_gate`.

**2. RANN re-flood — byte-exact Linux relay.** For the SAME RANN (sn=65) captured from both emitters:

| field | chronite ORIGIN | board0 RE-FLOOD | rule (Linux `hwmp_rann_frame_process`) |
|---|---|---|---|
| category/action/eid/len/flags | `0d 01 7e 15 01` | `0d 01 7e 15 01` | preserved (flags keeps IS_GATE) |
| hop_count | `00` | `01` | **+1** |
| ttl | `1f` (31) | `1e` (30) | **−1** |
| rann_addr | chronite | chronite | preserved (the ORIGIN, not the relay) |
| rann_seq | `41000000` (65) | `41000000` (65) | preserved (not bumped) |
| interval | `88130000` (5000) | `88130000` | preserved |
| metric | `00000000` (0) | `ab0a0000` (2731) | **accumulated** (+last_hop airtime) |
| MAC-hdr FC / DA / SA | d000 / bcast / chronite | d000 / bcast / **board0** | SA = the forwarder |

Machine-checked: **TRANSFORM MATCHES `hwmp_rann_frame_process` = True**. Raw relay action_hex
`0d017e1501011e3c227f3751384100000088130000ab0a0000`; full frame
`d0000000ffffffffffffe272a1f8efa4e272a1f8efa440040d017e1501011e3c227f3751384100000088130000ab0a0000<FCS>`.

**3. Beacon Connected-to-Gate bit.** board0's S1G beacon Mesh Config IE: **Formation Info = `0x03`** → bit0
Connected-to-Gate = 1, peer_count (bits1-6) = 1 — *byte-identical to the Linux gate's `0x03`*; Capability = `0x09`
→ ACCEPT_PLINKS(0x01) + FORWARDING(0x08). (The Linux gate additionally sets bit4 `0x10` → cap `0x19`; that bit is
out of S2 scope and not compared in peer matching.)

**Bonus (bidirectional):** chronite's `iw dev wlan1 mpath dump` shows an ACTIVE path to `e2:72:a1:f8:ef:a4`
(metric 2410, hop 1) and `station dump` shows it ESTAB — the ESP's root-confirmation PREQ reached chronite and
built the return path.

**Independent static verification:** a 4-lens adversarial workflow (rx-parse-reflood, freshness-sn, gate-list-beacon,
regression-loops) + synthesis returned **MATCH on every lens, zero blockers** — confirming the RX offsets vs
`struct ieee80211_rann_ie`, the freshness SN math (proven identical incl. wraparound), the re-flood byte string, the
gate-counter integrity, the beacon bits (no peering regression — `mesh_matches_local` ignores those bytes), and no
re-flood ping-pong loop (freshness + own-RANN ignore + TTL terminate it).

**Two fixes applied** (workflow risks, then re-verified on-air — behavior-neutral for the ESTAB scenario, no
regression): (a) relaxed the sender check from `!= ESTAB → drop` to `not-a-known-peer → drop` (== Linux
`sta_info_get`: a non-ESTAB peer is processed at MAX metric, not dropped); (b) set `expiry_ms = now +
MESH_PATH_LIFETIME_MS` on an accepted RANN so a gate path learned purely from RANN isn't left at `expiry_ms=0`
(the first eviction victim).

**Radio-silent cleanup done** ([[radio-silent-after-every-test]]): board0 → `rimba-hello`; chronite + chronium
`wlan1` down (chronium `type managed`; used the SAFE `pkill -9 -f wpa_supplicant_s1g` — a bare `pkill wpa_supplicant`
earlier knocked chronium off the LAN by killing its management supplicant, it recovered).

## Next step — S3 (6-address AE datapath, the hard 20%)

S3 emits/parses 6-address Address-Extension mesh data frames (proxied MACs) — the piece that makes an off-mesh
host reachable through the gate. **⚠ CCMP offset/AAD math** must be re-derived for the +12 AE bytes (design §4);
**probe on-air** that the MM6108 FW delivers RX AE frames to the host cb + accepts AE forwards before building it.
Design: `docs/mesh-ap/rimba-mesh-ap-mesh-gate-discovery-design.md` (S3, ~3-5 d).

## Footguns
- **Linux reference** re-`scp` each session from `chronium:/home/chronium/halow/rpi-linux/net/mac80211/`
  (+ `include/linux/ieee80211.h`); kernel 6.12.21.
- **chronium radio silence:** use `pkill -9 -f wpa_supplicant_s1g` (the mesh one), NOT `pkill wpa_supplicant` —
  the bare form kills chronium's LAN management supplicant and drops it off the network.
- **S2 fixture is `rimba-smesh`** (peers with the Linux gate), unlike the S1 `test-mesh-gate` which was a lone
  `rimba-mesh` gate. The RANN bytes are independent of mesh ID.
- **Interval units:** the RANN interval is the RAW numeric value on the wire (label TU, no ms→TU convert) — the
  re-flood preserves it verbatim; keep both sides at the same numeric value for the byte-diff.
- **Uncommitted** in the submodule; lands as a `mm-esp32-halow` PR + a superproject gitlink bump carrying
  `firmware/test-mesh-gate-rx` + `tools/mesh_rann_cap.py` + `tools/mesh_beacon_cap.py`. No auto-commit; no Claude
  trailers ([[commit-attribution]]).

## Files
- `components/halow/.../umac/mesh/umac_mesh.c` (+264 S1+S2), `.../umac_mesh.h` (+14) — **uncommitted**.
- `firmware/test-mesh-gate-rx/` — S2 RX/relay fixture (untracked).
- `tools/mesh_rann_cap.py`, `tools/mesh_beacon_cap.py` — RANN + beacon Mesh-Config capture/decoders (untracked).
- `docs/mesh-ap/rimba-mesh-ap-mesh-gate-discovery-design.md` — the design + code-map (§3 table re-pin still pending).
- Memory: `mesh-gate-8021s-port-planned` (updated: S2 verified).
