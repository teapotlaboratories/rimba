# 2026-06-26 — Mesh peering (MPM) exchange: decoded reference frames

Captured the **802.11s mesh peering handshake** between two Linux morse nodes
(chronium `3c:22:7f:37:50:42` ↔ chronite `3c:22:7f:37:51:38`, open mesh, `user_mpm=1`) to
spec the ESP (morselib) MPM implementation for P2. Source: `wpa_supplicant_s1g -dd -K` logs
(the supplicant builds/parses the Self-Protected Action frames, so its log carries the exact
bytes); cross-checked on-air via the chronium `morse0` monitor (see
`docs/reference/rimba-linux-halow-monitor.md`).

## Handshake / state machine (the happy path)

```
A=local (chronium, llid=0xc7e1)             B=peer (chronite, llid=0xac4b)
A: Mesh Peering OPEN  (type 1, llid=A)  --> B          # IE75 len4: proto, llid
B: Mesh Peering OPEN  (type 1, llid=B)  --> A          # A learns B's llid
A: state OPN_SNT --event OPN_ACPT--> OPN_RCVD
A: Mesh Peering CONFIRM (type 2, llid=A, plid=B) --> B # IE75 len6: proto, llid, plid
B: Mesh Peering CONFIRM (type 2, llid=B, plid=A) --> A
A: state OPN_RCVD --event CNF_ACPT--> ESTAB            # "mesh plink … established"
```
Peer link IDs (llid/plid) are random per session. A stale plid causes a CLOSE (type 3,
reason 52/55, `peer_lid mismatch`) and a re-OPEN with a fresh llid — normal session churn,
not an error. Frame types: **1=Open, 2=Confirm, 3=Close**; Category = **15** (Self-Protected).

## Reference frame: Mesh Peering OPEN (chronite TX, on-air, len=138 mgmt)

```
d0 00 d0 02                                  FC=action, dur
3c 22 7f 37 50 42                            A1 DA   = chronium
3c 22 7f 37 51 38                            A2 SA   = chronite
3c 22 7f 37 51 38                            A3 BSSID= chronite (own MAC)
50 03                                        seq
-- action body (this is the "DataLen" wpa_supplicant reports) --
0f                                           Category = 15 (Self-Protected)
01                                           Action   = 1  (Mesh Peering Open)
00 00                                        Capability (2)            [Open has cap, no AID]
01 08 02 04 0b 8c 16 98 24 b0                Supported Rates (id 1, len 8)
71 07 01 01 00 01 00 00 09                   Mesh Configuration (id 0x71=113, len 7)
72 0a 72 69 6d 62 61 2d 6d 65 73 68          Mesh ID (id 0x72=114, len 10, "rimba-mesh")
75 04 00 00 4b ac                            *** Mesh Peering Mgmt (id 0x75=117, len 4):
                                                 protocol=0x0000, local link id=0xac4b
2d 1a ...                                    HT Capabilities (id 0x2d=45, len 26)
3d 16 70 00 ...                              HT Operation   (id 0x3d=61, len 22)
bf 0c 66 00 80 03 fd ff ...                  VHT Capabilities (id 0xbf=191, len 12)
c0 05 00 70 00 fd ff                         VHT Operation  (id 0xc0=192, len 5)
```

## Reference frame: Mesh Peering CONFIRM (chronium TX, len=158 incl. CMD_FRAME hdr)

Same IE list, but the fixed fields and IE75 differ:
```
0f 02            Category 15, Action 2 (Confirm)
00 00            Capability
01 00            AID (2)                       [Confirm adds AID; Open does not]
01 08 ...        Supported Rates
32 0e 09 0b 8c ...   Extended Supported Rates (id 0x32=50, len 14)   [present here]
72 0a "rimba-mesh"   Mesh ID
71 07 01 01 00 01 00 00 09   Mesh Configuration
75 06 00 00 e1 c7 4b ac      *** Mesh Peering Mgmt len 6: proto, llid=0xc7e1, plid=0xac4b
2d 1a ... / 3d 16 ... / bf 0c ... / c0 05 ...   HT/VHT
```

## Mesh Peering Management element (IE 117 / 0x75) — the crux

| Frame   | Action | Bytes                              | Fields                         |
|---------|--------|------------------------------------|--------------------------------|
| Open    | 1      | `75 04 <proto:2> <llid:2>`         | protocol, local link id        |
| Confirm | 2      | `75 06 <proto:2> <llid:2> <plid:2>`| + peer link id                 |
| Close   | 3      | `75 08 <proto:2> <llid:2> <plid:2> <reason:2>` | + reason code      |

`protocol` = `00 00` (0 = mac80211 / vendor-neutral MPM). Link IDs are little-endian in the
frame (`4b ac` = 0xac4b).

## Mesh Configuration element (IE 113 / 0x71), 7 bytes: `01 01 00 01 00 00 09`
- Active Path Selection Protocol = `01` (HWMP)
- Active Path Selection Metric   = `01` (Airtime)
- Congestion Control Mode        = `00` (none)
- Synchronization Method         = `01` (neighbour offset) — matches the morselib default we set
- Authentication Protocol        = `00` (none / open)
- Mesh Formation Info            = `00`
- Mesh Capability                = `09`
Both peers must present a matching Mesh Configuration for `mesh_matches_local` to accept the
candidate, otherwise no peering even starts.

## The ESP gap (what board0/board1 send today)

The ESPs DO transmit Mesh Peering Open (Category=15, Action=1) — P2 is partly done — but the
frame is **DataLen=13 and wpa_supplicant logs `No Mesh Peering Management element`**. A correct
Open is DataLen≈23+ (the reference above). So the ESP frame is missing at minimum the
**Mesh Peering Management element (IE 117)** — and likely Supported Rates / Mesh ID too. It
also never advances the state machine (no Confirm), so no plink forms.

### morselib work items for P2 (next)
1. **Build the Mesh Peering Management element (IE 117)** into the Open/Confirm/Close TX path —
   `75 04 00 00 <llid>` (Open), `75 06 …<plid>` (Confirm), `75 08 …<reason>` (Close), llid/plid
   little-endian.
2. Include **Supported Rates (1)**, **Mesh ID (114)**, **Mesh Configuration (113)** with the
   exact values above in every peering frame.
3. Implement the **MPM state machine**: on RX Open → reply Open(if needed)+Confirm; track
   llid/plid; OPN_SNT→OPN_RCVD→ESTAB; handle Close/timeout. Mirror `net/mac80211/mesh_plink.c`.
4. Generate a random **local link id** per peer; echo the peer's llid back as plid.

Build directly from the decoded frames above + `net/mac80211/mesh_plink.c`
(`mesh_plink_frame_tx`) as the canonical reference.

## ESP implementation status (2026-06-26)

Implemented in `morselib/src/umac/mesh/umac_mesh.c` (+ `umac_mesh.h`,
`firmware/rimba-halow-mesh`):
- **Proper peering frame builder** `umac_mesh_build_peering()` — Open/Confirm/Close with
  fixed fields (Capability; +AID for Confirm), S1G Capabilities, Mesh ID (114), Mesh
  Configuration (113), and the **Mesh Peering Management element (117)** with little-endian
  link ids, per the table above.
- **Peer table + FSM** `umac_mesh_handle_action()` — LISTEN/OPN_SNT/OPN_RCVD/CNF_RCVD/ESTAB,
  derived from `mesh_plink.c`. Responder (answer Open with Open+Confirm) and the link-id
  convention (peer's local-link-id -> our `plid`; we echo it back as the Confirm's plid).
- **Initiator** `mmwlan_mesh_peer_open()` — sends an Open (-> OPN_SNT) on a heard peer beacon
  (driven from the app's beacon callback for same-Mesh-ID neighbours), mirroring mac80211
  opening a plink on a candidate beacon. The old broadcast `send_test_action` probe is retired.

**On-air verification** (chronium `morse0` monitor) — the frames are now correct; the original
"No Mesh Peering Management element" rejection is gone:
```
board0 Open  (act1): 0f 01 |0000| d90f.. (S1G caps) |72 0a "rimba-mesh"|
                     71 07 01 01 00 01 00 00 01 (Mesh Cfg)| 75 04 0000 e208   (IE117 llid=0x08e2)
board1 Cnf   (act2): 0f 02 |0000|0100| .. same IEs .. | 75 06 0000 e16d e208 (IE117 llid=0x6de1,
                                                                              plid=0x08e2 = echo)
```
board1 answering board0's Open with a **Confirm** proves the Open is well-formed and accepted,
and that the llid/plid echo is correct.

**Not yet reaching ESTAB — next increment.** A lost frame stalls the handshake (board0 lands in
CNF_RCVD waiting for board1's Open, which was lost) because there are **no retransmit/holding
timers** yet. mac80211 uses `mesh_plink_timer` to retransmit Open/Confirm and to handle
link-id mismatch with a reason-coded Close. TODO for the next increment:
1. Add a per-peer retransmit timer (resend Open in OPN_SNT, Confirm in OPN_RCVD) with a small
   retry count — `dot11MeshRetryTimeout` / `dot11MeshMaxRetries`.
2. Validate the echoed peer-link-id against our `llid`; on mismatch send Close (reason 52/55)
   and restart, instead of stalling.
3. Add the per-peer stad (`umac_sta_data_alloc`) + 4-address mesh data path so an ESTAB link
   can carry traffic (the P4 data path).
4. Re-test ESP↔ESP and ESP↔Linux to ESTAB; confirm with `iw dev wlan1 station dump` (plink
   ESTAB) on the Linux side and a mesh ping.

Diagnostic `MMLOG_ERR` traces (OPN_SNT tx status, action RX) are currently left in
`umac_mesh.c` for bring-up; demote to `MMLOG_INF`/remove once ESTAB is reliable. Note: on ESP
serial only `MMLOG_ERR` (prefix `E`) is visible — `MMLOG_INF` is filtered out.

## Update 2 — retransmit timers added; ESP↔ESP reaches ESTAB ✓

Added the peer-link retransmission tick (`umac_mesh_plink_tick`, mirrors mac80211
`mesh_plink_timer`) on the umac-core timeout queue (`umac_core_register_timeout`, runs in the
RX context so TX is safe; self-reschedules while the mesh is active):
- OPN_SNT / OPN_RCVD / CNF_RCVD → **retransmit Open** until ESTAB or max retries; resending the
  Open is what recovers a lost frame (a peer stuck in CNF_RCVD reaches ESTAB when the other
  side retransmits its Open).
- max retries → reason-coded **Close** → HOLDING → free the slot (next beacon re-opens).
- Retry counter resets on any received peering frame (forward progress).
- Added a **stale-session guard**: a peer-link-id echo ≠ our current llid (peer rebooted across
  an established link) → Close + free + re-open with fresh ids.

First cut (`RETRY=300ms`, `MAX_RETRIES=5`, fixed interval) reached ESTAB but churned for ~23 s:
the two boards' Opens collided in lockstep (half-duplex) and a 5-retry budget tore down active
handshakes. Fixes: **`MAX_RETRIES=16`** (tolerate ~5 s of transient silence) + **interval
jitter** (`+rand(0..200ms)`) to break lockstep. Result:

```
board0 from boot:  MESH peer e2:72:a1:f8:f9:40 ESTABLISHED   @ ~1.5 s   (no churn, stable)
```
**ESP↔ESP 802.11s mesh peering now reaches ESTAB cleanly in ~1.5 s and holds.** Bidirectional
by protocol (board0 ESTAB requires board1's Confirm, and board0 sent board1 its Confirm).

## Update 3 — ESP↔Linux peering blocked by a beacon-format asymmetry (separate issue)

The ESP does not peer with the Linux node yet, but **not because of the MPM** — the ESPs never
*hear* the Linux node, so they never initiate. chronium `morse0` monitor, all three on ch27:
```
S1Gbcn sa=3c:22:7f:37:51:38 x94   <- chronite (Linux): S1G SHORT beacon (ext type3/sub1)
bcn    sa=e2:72:a1:f8:ef:a4 x94   <- board0 (ESP):    LEGACY beacon (type0/sub8)
bcn    sa=e2:72:a1:f8:f9:40 x95   <- board1 (ESP):    LEGACY beacon (type0/sub8)
```
The ESP mesh beacon goes out **legacy (type0/sub8)**, while morse Linux mesh emits **S1G short
beacons**. The ESPs hear each other (both legacy) but log no `3c:22:7f…` beacon — so they never
`mmwlan_mesh_peer_open()` the Linux node, and chronite receives 0 ESP peering frames. (chronite
*does* hear the ESP legacy beacons — it adds them as a scan BSS — so the asymmetry is one-way.)

Two hypotheses for "ESP doesn't hear the Linux S1G short beacon", to investigate next:
1. **Beacon delivery/parse** — the firmware should convert a received S1G short beacon to a
   legacy beacon for the host (dot11ah `s1g_to_beacon`); if the mesh beacon RX filter
   (`MMWLAN_FRAME_BEACON`) doesn't surface S1G short beacons, the app's `peer_beacon_cb` never
   sees it. Also worth checking: should the ESP itself be emitting an **S1G** mesh beacon (like
   its AP beacons / the Linux reference) rather than a legacy one?
2. **RF asymmetry** — ESP↔ESP is strong (co-located modules); ESP↔Linux may be marginal.
   Cross-check signal via the monitor / swap which node initiates.

This is independent of the MPM (which is done). Next: characterize #1 (instrument the ESP mesh
beacon RX path; confirm whether S1G short beacons reach the host) and decide whether the ESP
mesh beacon should be S1G.

## Update 4 — ESP↔Linux peering RESOLVED ✓ (S1G-beacon peer discovery)

Diagnosis #1 was correct (not RF). Instrumenting `umac_datapath_process_s1g_beacon` showed
chronite's S1G beacon **does** reach the ESP host (~10/s, `src=3c:22:7f:37:51:38` — its real
MAC, since a mesh BSSID equals the sender's own MAC). The two reasons it never triggered
peering:
1. **S1G beacons bypass the app callback.** Legacy beacons (type0/sub8) flow through the
   `rx_frame_cb` path where the app's `peer_beacon_cb` runs; S1G beacons (ext type3/sub1) branch
   to `process_s1g_beacon`, which never calls `rx_frame_cb`. That's why the ESPs only "saw"
   each other (both emit legacy beacons) and not the Linux node.
2. **`process_s1g_beacon` dropped foreign-BSSID beacons** at the `addr_matches_bssid` early
   return — correct for STA/IBSS (where `source_addr` = BSSID ≠ sender), but wrong for mesh
   (where `source_addr` = the peer's own MAC and is exactly what we need).

Fix (`umac_datapath.c` + `umac_mesh.c`): in `process_s1g_beacon`, when a mesh vif is active and
the beacon is foreign-BSSID, strip the optional S1G beacon fields and call the new
`umac_mesh_handle_peer_beacon(source_addr, ies, ies_len)`, which matches the Mesh ID (114) and
calls `mmwlan_mesh_peer_open()` — the same initiator path used for legacy peer beacons. Result:

```
board0:    MESH open -> 3c:22:7f:37:51:38  OPN_SNT
board0:    MESH peer  3c:22:7f:37:51:38  ESTABLISHED   @ ~1.5 s
chronite:  Station e2:72:a1:f8:ef:a4   signal -9 dBm   mesh plink: ESTAB
```

**ESP↔Linux 802.11s mesh peer link established, confirmed on both sides.** Note we did *not*
need to change the ESP's beacon to S1G — the ESP still emits a legacy mesh beacon (which the
Linux side accepts as a scan BSS), and now also *consumes* the Linux S1G beacon for discovery.
(Whether the ESP mesh beacon should additionally be S1G for airtime efficiency / spec
conformance is a separate, lower-priority question.)

## Update 5 — ESP mesh beacon converted from legacy to S1G ✓

On-air audit (chronium `morse0`) found the ESP mesh beacon was a **legacy PV0 beacon**
(mgmt type0/sub8, 3-address, ~36 B header) while morse **Linux** mesh and the ESP's own **AP**
mode both emit **S1G short beacons** (ext type3/sub1, 1-address, ~15 B header). The ESP mesh was
the only node sending legacy — non-conformant for HaLow and ~2× the beacon airtime.

Mechanism: the morse firmware auto-generates S1G short beacons only for an **AP-type** vif; for
a **MESH-type** vif it transmits the host's beacon as-is (morselib has no host-side
legacy→S1G conversion, unlike the Linux driver's `morse_dot11ah_beacon_to_s1g`). Using AP vif
type isn't an option (MESH_CONFIG/MBCA need MESH type). So the fix is to **build the S1G beacon
host-side** in `umac_mesh_build_beacon`:
- 15-byte S1G short-beacon header (`dot11_s1g_beacon_hdr`): FC `0x081c` (ext/S1G_BEACON, no
  optional TBTT/CSSID/ANO), `source_addr` = our mesh MAC, `time_stamp`/`change_sequence` 0.
- IEs: **S1G Beacon Compatibility (213)** carrying the beacon interval (S1G beacons have no
  legacy Beacon-Interval fixed field), S1G Capabilities (217), S1G Operation (232),
  Mesh ID (114), Mesh Configuration (113). Dropped the legacy fixed fields + zero-length SSID.

The firmware fills the real TSF at the S1G `time_stamp` offset (verified: I wrote 0, on-air it
carried a live TSF), and beaconing/peering keep working. Verified on-air — all nodes now S1G,
zero legacy:
```
board0/board1 (ESP mesh)  S1G-Beacon       (was legacy)
board2 (ESP AP)           S1G-Beacon
chronite (Linux mesh)     S1G-Beacon
chronite station dump: board0 + board1 both mesh plink ESTAB
```
board0's S1G beacon decodes structurally identical to chronite's. Open question (low priority):
the ESP could also send periodic **long** beacons (full IE set) like an AP; the short beacon
alone is sufficient for discovery + peering here.

### Remaining for a usable mesh (P4)
Peering (control plane) is done both ESP↔ESP and ESP↔Linux, and beacons are now S1G. Still
needed to pass user traffic: the mesh **data path** + link-up.

## Update 6 — P4 data-path design (grounded in code + a captured on-air frame)

**Link-up works.** Added `umac_mesh_link_up_once()` (signals `MMWLAN_LINK_UP` on first ESTAB,
mirroring IBSS). Confirmed on board0: `MESH link up (first peer established)` fires at ESTAB
with **no crash** — the old "link-up crashes in tx_frame" warning is resolved by the P2 datapath
setup. lwIP produced no observable traffic yet (no IP configured / no active sender).

**Authoritative on-air mesh data frame** (chronite broadcast ARP, via chronium `morse0`):
```
88 02 |0000| ffffffffffff | 3c227f375138 | 3c227f375138 |90 09|20 01| 00 1f 0c000000 |
FC0x0288 dur  A1=DA(bcast)   A2=TA=chronite  A3=meshSA=chronite seq   QoS   MeshControl
  aaaa03 000000 0806 | 0001 0800 0604 0001 3c227f375138 0a090902 ...
  LLC/SNAP  eth=ARP   <ARP: sender=chronite / 10.9.9.2>
```
Decoded structure (verified, not inferred):
- **QoS Data** (subtype 8) — has a 2-byte QoS Control field after the addresses.
- **Group-addressed (bcast/mcast) = 3-address, fromDS=1, toDS=0**: A1=DA, A2=TA(us), A3=mesh-SA(us).
  (Unicast to a peer = **4-address, toDS=fromDS=1**: A1=peer/nexthop, A2=us, A3=mesh-DA, A4=mesh-SA.)
- **Mesh Control header** (after QoS ctrl, before LLC/SNAP): `flags(1)=0, ttl(1)=31
  (dot11MeshTTL), seqnum(4, le, incrementing)`; +6/+12 bytes only with address extension
  (proxy/multi-hop). Ref: `ieee80211_new_mesh_header`, `net/mac80211/tx.c`.
- Then LLC/SNAP (`aa aa 03 00 00 00` + ethertype) + payload.

**Architecture (verified):** morselib builds the 802.11 frame, the firmware converts to S1G
(no host-side S1G data conversion). `dot11_data_hdr` has `addr4`; the AP datapath already fills
it (`umac_datapath_ap.c`). The TX assembly (`umac_datapath_tx_frame`) does
`construct_80211_data_header` → strip 802.3 MACs → prepend SNAP → prepend 802.11 header; the
**Mesh Control header is a new insertion between the 802.11 header and SNAP**.

### Concrete P4 steps (next)
1. `umac_datapath_construct_80211_data_header_mesh`: group-addr → 3-addr QoS data fromDS=1
   (A1=DA, A2=A3=us); unicast → 4-addr toDS=fromDS=1 (A1=peer, A2=us, A3=DA, A4=us).
2. Insert the 6-byte Mesh Control header (flags=0, ttl=31, mesh seqnum++) in the mesh TX path,
   after QoS ctrl / before SNAP.
3. RX: parse QoS + Mesh Control, strip to 802.3, deliver to lwIP (the mesh RX data handler).
4. Per-peer stad at ESTAB (`umac_sta_data_alloc`, bind peer/bssid) for unicast lookups + RX.
5. App: static IP on the mesh netif (e.g. `10.9.9.3/24`) so it can join the Linux mesh ping.
6. **Verify the firmware S1G-converts a mesh QoS-data + Mesh-Control frame** (the one real
   unknown) — capture the ESP's TX on `morse0` and confirm it reaches air as S1G.
7. (Later) HWMP path selection for multi-hop; for now next-hop = the directly-peered dest.

## Update 7 — P4 data path BUILT + verified to the frame layer; ping gated on HWMP

Implemented and verified on hardware:
- **TX header** `umac_datapath_construct_80211_data_header_mesh` (group→3-addr fromDS=1;
  unicast→4-addr toDS=fromDS=1) + **Mesh Control header** insertion in `umac_datapath_tx_frame`
  (flags=0, ttl=31, mesh seqnum) + QoS **Mesh Control Present** bit (0x0100).
- **RX**: strip the Mesh Control header (gated on the QoS bit; AE-aware length) before LLC/SNAP.
- **Link-up** at first ESTAB; **app**: static mesh IP + ping; **MAC sync** (`esp_netif_set_mac`
  to the mesh vif MAC) so L2(vif)==L3(ARP).

On-air verification (chronium `morse0`):
- board0 emits **consistent mesh data frames** (fromDS + Mesh Control), e.g.
  `88 02 … A1=bcast A2=A3=board0 QoS=0x0100 MeshCtrl=00 1f <seqnum> SNAP …` — matches the
  chronite reference exactly.
- **chronite receives them** (station `rx bytes` climbs to ~75 KB; **learns board0's ARP**
  `10.9.9.136 → e2:72:a1:f8:ef:a4`, the synced MAC). So broadcast data board0→chronite reaches
  L3, and the TX format is accepted.

**Remaining blocker: HWMP path resolution (the final piece).** chronite's `mpath dump` shows
board0 with `NEXT_HOP 00:00:00:00:00:00`, `FLAGS 0x2 = MESH_PATH_RESOLVING`. mac80211 mesh
forwards ALL unicast via the mesh path table, even to a direct peer: it sends a PREQ targeting
board0, but morselib has no HWMP, so no PREP is returned, the path never goes ACTIVE, and the
unicast ARP-reply / ICMP-reply is never delivered to board0 (hence ping timeout; board0's own
ARP never resolves so it sends no 4-addr unicast either).

### Next: minimal HWMP (mirror net/mac80211/mesh_hwmp.c)
1. **Respond to PREQ** targeting us with a **PREP** (so chronite resolves its path to us → can
   unicast to us). This alone should let chronite deliver the ARP reply + ICMP echo reply.
2. **Originate a PREQ** + accept PREP (so we resolve our path to chronite → can send our own
   unicast). For a single-hop direct peer, the path is one hop (next hop = the peer).
3. Airtime-metric link metric element + the HWMP IE formats (PREQ/PREP/PERR action frames,
   category 13/15 mesh path selection). Verify with chronite's `mpath dump` going ACTIVE and the
   ESP joining the Linux `10.9.9.x` ping.
Data path otherwise complete; this is the last gate for routable mesh IP.

## Update 8 — ESP32 pings the Linux mesh ✅ (minimal HWMP + RX vif fix)

Two fixes closed it:
1. **HWMP target role** (`umac_mesh.c`): on a received PREQ (Mesh Action cat 13 / action 1)
   that targets us, build + send a **PREP** (EID 131: flags, hop_count, ttl, target=us,
   target_sn, lifetime, metric=0, orig=PREQ-originator, orig_sn). Mirrors
   `net/mac80211/mesh_hwmp.c` `mesh_path_sel_frame_tx`. After this, chronite's `mpath dump` for
   board0 went `FLAGS 0x5` (ACTIVE|RESOLVED), `NEXT_HOP=board0` — chronite could finally unicast
   to the ESP. (We don't originate PREQs; our own TX is direct single-hop.)
2. **RX vif fix** (`umac_datapath.c`): the RX data path's vif determination only matched
   `STA` and `AP|ADHOC`, so mesh frames hit `"Invalid RX VIF"` and were dropped *after* the
   Mesh Control strip. Added `UMAC_INTERFACE_MESH` to the `AP|ADHOC` branch (mesh reuses the
   AP per-vif slot, like IBSS). A diagnostic confirmed board0 received chronite's unicast data
   (`sa=chronite da=board0 mctrl=1`) but dropped it here.

Result — board0 (ESP 10.9.9.136) ↔ chronite (Linux 10.9.9.2):
```
reply from 10.9.9.2: seq=2 time=93 ms ... seq=10 time=11 ms ... seq=13 time=13 ms
```
**Single-hop ESP↔Linux mesh IP is working end-to-end.** P4 complete. Feature status +
Linux/ESP comparison table: `docs/mesh-ap/rimba-mesh-ap-milestones.md`. Remaining (P5): HWMP source
role (PREQ originate) + mesh path table + multi-hop forwarding.

## Update 9 — P5: HWMP source role + path table + multi-hop endpoint ✅

Implemented in `umac_mesh.c` (derived from `mesh_hwmp.c` / `mesh_pathtbl.c`):
- **Mesh path table** (`mesh_path_*`): dest → next_hop, SN, metric, hop_count, expiry. Install
  rule mirrors `hwmp_route_info_get` fresh-info (take if inactive / newer SN / same SN + better
  metric). Fixed per-hop metric (100); 5 s lifetime.
- **HWMP source role**: `umac_mesh_start_discovery()` originates a PREQ (broadcast, rate-limited)
  when the data path has no route; `umac_mesh_lookup_next_hop()` returns the resolved next hop.
- **Path-based TX** (`construct_80211_data_header_mesh`): A1 = HWMP next hop; falls back to direct
  + kicks off discovery if unresolved (so a directly-peered dest still works immediately).
- **PREQ/PREP RX**: install reverse/forward path from the immediate sender; reply with PREP if we
  are the target (existing); **forward** PREQ (flood, ttl--) and PREP (toward orig via our path).

**Multi-hop demonstrated** (ESP as endpoint, Linux as relay). Forced a line topology with a
per-node **peer allowlist** (`mmwlan_mesh_set_peer_allowlist`, app toggle `MESH_MULTIHOP_DEMO`)
so board0 peers ONLY with chronite, plus an HWMP-RX transmitter filter so board0 only learns
paths via chronite (all bench nodes are in RF range, else HWMP finds a 1-hop path):
```
board0 (peers only with chronite) --PREQ--> chronite --fwd--> chronium
chronium --PREP--> chronite --fwd--> board0 :  MESH path to chronium via chronite (metric 5952)
board0 ping 10.9.9.1 (chronium):  reply seq=10..17  ~14-20 ms   (2 hops, chronite relays the data)
```
board0 reaches a node it has **no direct peer link with**, over a 2-hop HWMP path. P5 done for
the ESP-as-endpoint case.

**Remaining (P5b):** ESP **data-frame relay** — forward a received mesh data frame whose mesh DA
isn't us toward the next hop (re-inject preserving A4=mesh-SA). Needs custom frame re-injection;
the demo above uses Linux (chronite) for the relay. Table: `docs/mesh-ap/rimba-mesh-ap-milestones.md`.

## Update 10 — P5b: ESP relays others' traffic ✅ (full ESP multi-hop)

`umac_mesh_forward_data` (umac_mesh.c) + an RX hook (umac_datapath.c): when a mesh data frame's
mesh DA (A3, 4-addr unicast) isn't us, build a **fresh** forwarded frame — 4-addr header
(A1=HWMP next hop to the DA, A2=us, A3=DA unchanged, **A4=mesh SA preserved**), QoS
mesh-control-present, Mesh Control, and the copied LLC/SNAP+payload — and TX it; the original is
dropped. Built via `build_mgmt_frame` (measures + allocs a right-sized txbuf). **First attempt
re-used the rxbuf and crashed (StoreProhibited) — the RX buffer lacks headroom for tx_frame's
header prepends; copying into a fresh txbuf fixed it.**

Demoed a 3-ESP line `board1 → board0(ESP relay) → board2` (forced via per-node peer allowlists
+ HWMP/data TA filters + static ARP, since the bench is all-in-range and ARP can't yet traverse
a relay):
```
board0 (relay): MESH relay e2:72:..f9:40 -> e2:72:..f0:08    (forwards board1->board2)
board1: MESH path to board2 via board0;  reply from 10.9.9.108  27 replies / 29 (~50 ms)
```
**An ESP32 now both originates multi-hop traffic AND relays it.** P5 + P5b complete. Limits +
full Linux/ESP feature table: `docs/mesh-ap/rimba-mesh-ap-milestones.md`. Next (P6): group/multicast
forwarding (so ARP traverses relays), PERR, airtime metric, security, power save, proxy/gate.

## Update 11 — P6a: group/multicast forwarding ✅ (ARP traverses a relay)

`umac_mesh_handle_group_data` + a mesh RMC (`mesh_rmc_seen`, per-(mesh-SA, mesh-seqnum) cache,
2 s window), wired into the RX path. On a group-addressed mesh frame (mesh DA multicast): drop
duplicates / our own echoes via the RMC, else **re-broadcast** it (3-address fromDS, A1=bcast,
A2=us, A3=mesh-SA unchanged, ttl-1, **seqnum preserved** so other nodes dedup) AND deliver
locally. Mirrors `net/mac80211` mesh group forwarding + `mesh_rmc_check`. The RX path now reads
the Mesh Control ttl/seqnum before stripping it.

Result: removed the static-ARP crutch from the relay demo — board1's broadcast ARP for board2
now propagates through board0 (group re-broadcast), board2 replies, and the unicast ping flows:
```
board1 -> board0(ESP relay) -> board2 (10.9.9.108), NO static ARP:  28 replies / 29 (~50 ms)
```
**Full ESP mesh forwarding now: unicast relay (P5b) + group/multicast (P6a).** Remaining (P6b):
PERR, airtime metric, RANN/root, proxy/gate, security, power save.

## Update 12 — P6b: PERR broken-link teardown + peer-inactivity link-failure detection ✅

Two pieces, both grounded in `net/mac80211`:

**1. PERR (`mesh_hwmp.c` mesh_path_error_tx / hwmp_perr_frame_process).** PERR element
(EID 132): `ttl, num_dest(=1), {flags=0, target_addr(6), target_sn(4), reason(2)}` — one
destination per frame, like `mesh_path_error_tx`. New in `umac_mesh.c`:
- `umac_mesh_build_perr` / `umac_mesh_tx_perr` — Mesh-Action cat 13 / action 1 carrying the
  PERR element.
- `umac_mesh_invalidate_paths_via(next_hop)` — when a link to `next_hop` breaks, mark every
  path through it inactive, `++sn` (so a stale cached PREP can't reinstate it — mirrors
  `mesh_plink_broken` doing `++mpath->sn` before `mesh_path_error_tx`), and announce each
  now-unreachable destination with a broadcast PERR.
- PERR RX in `umac_mesh_handle_hwmp`: for each listed target, tear our path down **only if**
  it goes via the PERR sender (`next_hop == TA`) and the announced SN isn't older than ours
  (`hwmp_perr_frame_process`), then flood the PERR onward (`ttl-1`).

**2. Link-failure detection (the trigger PERR needs).** The implementation had no way to notice
an established peer going silent — `mesh_plink_timer` only retransmits during the handshake, and
a rebooting peer's fresh Open (llid echo = 0) doesn't trip the stale-llid Close, so PERR could
never actually fire. mac80211 detects this in housekeeping: `mesh.c:916`
`ieee80211_sta_expire(sdata, mshcfg.plink_timeout * HZ)` reaps peers idle past `plink_timeout`
(`last_rx` updated on every RX, `mesh_plink.c:446`). Mirrored here:
- `struct mesh_peer` gains `last_rx_ms`, bumped on every beacon from the peer
  (`umac_mesh_handle_peer_beacon`) and every peering frame (`handle_action`).
- `umac_mesh_plink_tick` ESTAB case: idle > `MESH_PLINK_INACTIVITY_MS` (6 s ≈ 60 missed
  ~100 ms beacons) → send Close, `umac_mesh_invalidate_paths_via(peer)`, free the slot.

Both teardown paths (ESTAB-inactivity, and OPN-retry-exhaustion → HOLDING → free) funnel into
`invalidate_paths_via`, so a lost next hop always flushes paths + PERRs regardless of the peer's
state at the moment it dies.

**Verified** (3-ESP line `board1 → board0(relay) → board2`, board2 dropped into ROM download
mode so it stays truly silent past the timeout):
```
board1: 34 ping replies, then board2 goes silent
board0 (relay) t=51.4s: MESH peer ...f0:08 detected gone
board0          t=52.3s: MESH PERR tx: dest ...f0:08 unreachable (next-hop ...f0:08 lost)
board1 (source) t=52.3s: MESH PERR rx: path to ...f0:08 via ...ef:a4 torn down
board2 returns  t=66s  : re-ESTABLISHED -> ping resumes (seq 78-80, ~35 ms)
```
No false teardowns: the healthy `board1↔board0` link (52 replies) never tripped the inactivity
timeout. Remaining (P6c): airtime metric, RANN/root, proxy/gate, SAE/AMPE, mesh power save.
Plus a testing TODO: mesh throughput with iperf (see milestones doc).

Reference revisions this was matched against (checked out on `chronite`): kernel
`MorseMicro/rpi-linux` branch `mm/rpi-6.12.21/1.17.x` (Linux 6.12.21) @
`372414fd42cdd4d8bfcf888cac62db9da947fdb6` — `net/mac80211/mesh_hwmp.c`, `mesh.c`; driver
`MorseMicro/morse_driver` 1.17.8 (`0-rel_1_17_8_2026_Mar_24`) @
`3eef5a0a43645808e501ff4b83f29d675588bd9b`.

## Update 13 — On-air verification on chronium's `morse0` monitor ✅ (third-party byte-diff)

Closed the verification gap: the ESP mesh frames were captured **on the air by a third node**
(chronium as a `CONFIG_MORSE_MONITOR` sniffer on S1G ch27 / freq 5560) and decoded byte-for-byte
against both the ESP encoders (`umac_mesh.c`) and the Linux `mesh_hwmp.c` element layouts. Setup:
`docs/reference/rimba-linux-halow-monitor.md` (`iw dev wlan1 set type monitor` + `set freq 5560`,
read `morse0` via AF_PACKET, skip radiotap); capture filtered to the ESP TA prefix `e2:72:a1`.
Scenario: the `board1 → board0(relay) → board2` line with a forced link break (board2 → ROM
download mode) to generate PERR + re-discovery. Window counts: **DATA-4addr 355, DATA-3addr 18,
PREQ 314, PREP 27, PERR 3, peering 8**.

**4-addr mesh DATA** (board1→board0→board2), on-air after-header bytes:
`00 01 | 00 1f ad 09 00 00 | aa aa 03 00 00 00 08 00 | 45 00 …`
- `00 01` QoS Control = 0x0100 → **Mesh Control Present** bit ✓
- `00 1f ad 09 00 00` Mesh Control = flags 0x00, **ttl 0x1f=31** (dot11MeshTTL), seqnum LE ✓
- `aa aa 03 …08 00` LLC/SNAP IPv4, then the IP header. Matches `umac_mesh_build_forward`.
- A1/A2/A3/A4 on the two hops: `TA=board1 A1=board0 A3=board2 A4=board1`, then the relay
  `TA=board0 A1=board2 A3=board2 A4=board1` — **A4 (mesh-SA) preserved through the relay** ✓.

**PREQ** (EID 130, len 37) — `00 00 1f 6f000000 | board1 | fd000000 | 30 75 00 00 | 00000000 | 01 00 | board2 | 16000000`
= flags, hop 0, ttl 31, preq_id, orig=board1, orig_sn, **lifetime 0x7530=30000ms**, metric 0,
target_count 1, target_flags, target=board2, target_sn. The flood shows hop_count 0→1→2→3 with
**metric 0 → 0x64 → 0xc8 → 0x12c (per-hop +100 = MESH_PATH_LINK_METRIC)** and ttl decrementing
0x1f→0x1e→0x1d ✓.

**PREP** (EID 131, len 31) — `00 00 1f | board2 | 01000000 | 30 75 00 00 | 00000000 | board1 | 10010000`
= flags, hop 0, ttl 31, target=board2, target_sn, lifetime 30000, metric 0, orig=board1,
orig_sn. Relay forwards at hop 1 / ttl 0x1e / metric 0x64 ✓ (toward the originator).

**PERR** (EID 132, len 15) — `1f 01 00 | board2 | 16000000 | 3e 00`
= **ttl 0x1f=31, num_dest 1, flags 0, target=board2, target_sn=0x16=22, reason 0x3e=62**
(`WLAN_REASON_MESH_PATH_NOFORWARD`). board0 originates it; **board1 re-emits the same PERR at
ttl 0x1e=30 (ttl−1)** → PERR RX + forward-on confirmed on air. Exact match to
`umac_mesh_build_perr` and `mesh_path_error_tx`.

**Verdict:** every ESP mesh frame on the air (data, PREQ, PREP, PERR, peering) is byte-identical
to the Linux `net/mac80211` element layouts (field order, little-endian SNs, and the constants
dot11MeshTTL=31 / lifetime 30000 / per-hop metric 100 / reason 62). Per
[[verify-onair-chronium-monitor]] this third-party capture — not endpoint logs — is the
ground-truth check, and it now backs P4–P6b. Gold-standard follow-up still open: a *live* Linux
node (chronite on ch27 mesh) originating the same HWMP/PERR for a same-capture A/B (the peering
frames already have that from earlier in this worklog; the element formats here are from the
authoritative `mesh_hwmp.c` source). The `morse0` tap worked cleanly this time (451 frames/12 s).

## Update 14 — Live Linux-device A/B on the air (chronite TX vs ESP TX) ✅ + 3 deltas found

Per [[verify-onair-chronium-monitor]], went past "matches the Linux source layout" to "matches
what a Linux node actually transmits." Brought **chronite** up as a live 802.11s mesh node
(`wpa_supplicant_s1g`, `ssid=rimba-mesh`, ch27/5560, `MESH-GROUP-STARTED`, 10.9.9.2) and captured
its frames on **chronium's monitor** (filter TA `3c:22:7f`), then byte-diffed against the ESP
frames from Update 13. Bring-up notes: chronite's morse stack was wedged (every `wlan1` op
dropped ssh, `wpa` exited pre-`MESH-GROUP-STARTED`) and needed a **reboot**; after reboot,
**NetworkManager + the system `wpa_supplicant` grab `wlan1`** (→ `nl80211 ... mode: -16 busy`),
so `nmcli dev set wlan1 managed no` is required, and `wpa_supplicant_s1g` must run **foreground**
(detached/`systemd-run` starts the mesh but it doesn't stay up). To force a Linux PREQ, give
chronite a static `ip neigh` for the ESP MAC (else its ARP never resolves → no path discovery).

The element **structure** matches perfectly (same field order, lengths, little-endian SNs,
Mesh Control header, addressing). But the live capture surfaced **3 value-level deltas the
source-format check missed**:

| # | Frame / field | Linux (chronite) on-air | ESP on-air | Notes |
|---|---|---|---|---|
| 1 | **Group/bcast QoS Ack Policy** | `20 01` → **No-Ack** (0x20) | `00 01` → **Normal-Ack** (0x00) | bcast/group frames should be No-Ack; ESP requests normal ack |
| 2 | **PREQ per-target flags** | `01` → **Target-Only (TO) set** | `00` → TO clear | TO=1 makes only the target answer; ESP lets intermediate nodes reply with possibly-stale paths |
| 3 | **PREQ lifetime** | `0xbebc` = 48828 **TU** (≈50 s) | `0x7530` = 30000 | ESP writes raw ms into a field 802.11s defines in TUs (no ms→TU convert); also a different magnitude |

Captured live from Linux: **beacon** (1000s), **group-addressed data (ARP)** [delta #1],
**PREQ** [deltas #2/#3]. Decoded bytes:
```
Linux PREQ : 00 00 1f 00000000 3c227f375138 01000000 bcbe0000 00000000 01 01 e272a1f8efa4 00000000
ESP   PREQ : 00 00 1f 6f000000 e272a1f8f940 fd000000 30750000 00000000 01 00 e272a1f8f008 16000000
Linux grp  : 20 01 | 00 1f <sn> | aa aa 03 00 00 00 08 06 (ARP)
ESP   grp  : 00 01 | 00 1f <sn> | aa aa 03 00 00 00 08 06 (ARP)
```
Not yet captured live from Linux: **PREP** (needs chronite to be a PREQ target) and **PERR**
(needs an active Linux path to break) — both blocked by the ESP peer-allowlist (no Linux↔ESP
plink), so they remain verified against the `mesh_hwmp.c` source layout only. A full peered
Linux↔ESP HWMP exchange would need an ESP flashed without the forced-topology allowlist.

**These 3 deltas are now P6c fix items** (set the PREQ TO flag, use No-Ack on group frames,
convert the PREQ lifetime ms→TU) — found precisely because the comparison was against the live
device, not the spec. Vindicates the [[verify-onair-chronium-monitor]] rule.

## Update 15 — Fixed the 3 live-Linux deltas + verified on air; Linux PREP captured

Fixed the three deltas from Update 14, each grounded in `net/mac80211` source, then re-verified
on chronium's monitor against the live Linux device.

**Fixes** (`umac_mesh.c`, `umac_datapath.c`):
1. **PREQ Target-Only flag** — `umac_mesh_start_discovery` now sets `target_flags =
   MESH_PREQ_TO_FLAG (1<<0)` on a **refresh** (path already known: `p->sn != 0`), 0 on first
   discovery — mirrors `mesh_queue_preq` (`PREQ_Q_F_REFRESH → IEEE80211_PREQ_TO_FLAG`).
2. **PREQ lifetime in TUs** — `MESH_MSEC_TO_TU(ms) = ms*1000/1024` (mesh_hwmp.c `MSEC_TO_TU`);
   PREQ lifetime is now `MESH_MSEC_TO_TU(MESH_PATH_LIFETIME_MS)` = 29296 (was raw 30000).
3. **No-Ack on group/broadcast** — QoS Ack Policy bits 5-6 = `01` (`|0x0020`) for group frames,
   in both the re-broadcast builder (`umac_mesh_build_rebcast`) and the originate path
   (`umac_datapath` `construct`/QoS, gated on `is_multicast`). Unicast keeps Normal-Ack.

**On-air verification** (board0 flashed with the fix + a `MESH_LINUX_INTEROP` app mode =
open peering + ping chronite; board1/board2 left on the *old* firmware as a live control):

| Field | board0 (fixed) | board1 (old) | live Linux (chronite) |
|---|---|---|---|
| PREQ target_flags | **`01`** (TO) | `00` | `01` |
| PREQ lifetime | **`70 72`** = 29296 TU | `30 75` = 30000 | `70 72`/`bc be` (TU) |
| group QoS | **`20 01`** (No-Ack) | `00 01` | `20 01` |

board0's originated PREQ for chronite: `00 00 1f … 70 72 00 00 00000000 01 01 3c227f375138 …`
— TO set + lifetime in TUs, byte-matching what Linux emits. The old-firmware board1 frames in
the same capture still show `00`/`30 75`/`00 01`, a clean before/after control.

**Linux PREP captured** (the 4th frame type, live from chronite as a PREQ target):
`00 00 1f | 3c227f375138 | 03000000 | 70 72 00 00 | 00000000 | e272a1f8efa4 | 03000000` —
hop 0, ttl 31, target=chronite, lifetime **`0x7270`** (it echoes board0's *fixed* TU lifetime),
orig=board0. Structure + the now-matching lifetime confirm the ESP↔Linux PREP interop.

**Linux PERR — not capturable on this bench (a finding, not a gap).** PERR is fundamentally a
*forwarder's* frame (`mesh_path_error_tx` on a relay hitting a dead next hop). Here chronite is
always an *endpoint* (it only peers board0; no node routes through it), so it never forwards. And
when board0 (chronite's next hop to board1/board2) was dropped to download mode while chronite
kept pinging board2, chronite sent **136 DATA-4addr frames to the dead next hop, 0 PERR, 0 PREQ**
— the morse driver gives mac80211 no TX-failure feedback to fire `mesh_plink_broken`, and
`mesh_plink_timeout` expiry didn't emit a PERR either. So a live Linux PERR needs a multi-Linux
forwarding mesh (a second non-monitor Linux node) — beyond this single-mesh-node + monitor bench.
The ESP PERR stays verified against the `mesh_path_error_tx` source layout + its own on-air
capture (Update 13); notably the ESP's peer-inactivity PERR is *more* proactive than what Linux
emitted here. All on-air work follows [[verify-onair-chronium-monitor]].
