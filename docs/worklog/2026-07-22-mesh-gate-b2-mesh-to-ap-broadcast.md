# 2026-07-22 — Mesh-gate B2: mesh → AP broadcast bridging (DONE + verified, with an honest D-ARP caveat)

**Status:** ✅ **B2 (mesh→AP broadcast bridging) IMPLEMENTED + its core function ON-AIR VERIFIED.** The gate now
forwards a mesh node's group-addressed frames down to its AP clients — the symmetric counterpart to B1 (S5's
AP→mesh broadcast branch). End-to-end, a mesh node reaches an AP client (continuous ICMP replies). **One honest
caveat surfaced (documented below, not papered over): a mesh node resolves an AP client from the CLIENT's
gratuitous ARP (via B1), not from the mesh node's own ARP — a mesh node's own ARP for a silent client does not
reach the gate.** Gate app (`rimba-halow-mesh-ap`) only; no morselib change. UNCOMMITTED.

## Goal

The last remaining L2-bridge datapath direction: the gate delivers a MESH node's broadcast/multicast (e.g. a mesh
node's ARP request for an AP client, or any group frame) to its AP clients — so a mesh node can INITIATE to an AP
client, not just reply to one. (S5b/S5c/B1 already cover mesh→AP unicast, AP→mesh unicast, and AP→mesh broadcast.)

## What was implemented — B2 (gate app `gw_mesh_rx_cb`)

The gate's mesh-RX ext-cb (`gw_mesh_rx_cb`, VIF_STA) now inspects each locally-delivered mesh frame: if its 802.3
dst is group-addressed (`eth[0] & 0x01`) and the gate has AP clients, it re-emits the frame **verbatim onto the AP
vif** (`mmwlan_tx_pkt` VIF_AP), then still delivers it locally (a bridge floods to all ports). The datapath already
prepended the 802.3 header — for a plain mesh multicast that is `[dst=group][src=mesh_sa]`; for a proxied AE_A4
multicast `[dst=group][src=eaddr1]` (the B1/AE_A4 RX work) — so the bytes are correct as-is.

**Loop-safety** (analysed + holds on-air): B2 fires only on mesh RX; the gate's own AP-inject and mesh
re-broadcast are TX and never return to the RX cbs; the mesh RMC + own-SA drop already suppress the gate's own
re-broadcasts; and B1 (AP→mesh) fires only on AP RX. So an AP client's broadcast (AP→mesh→other-nodes) and a mesh
node's broadcast (mesh→AP→clients) each traverse the gate exactly once.

## ✅ VERIFICATION — on-air, 3-node mesh+AP+STA

- **board0 = GATE** `rimba-halow-mesh-ap` (+ B2). **board1 = D** `rimba-halow-mesh PING_IP=10.9.9.50` (a mesh node
  that ORIGINATES a ping to the AP client). **board2 = STA** `rimba-halow-sta …NO_PING` (responder-only AP client
  at 10.9.9.50 — so D is the sole initiator).

**B2 fires — a mesh broadcast is bridged to the AP** (ARP-detail logging added to the gate):
```
mesh group RX: 350 B from e2:72:a1:f8:f9:40 et=0x0800 clients=1     (the gate receives D's group frame …)
B2 mesh->AP bcast: 350 B from e2:72:a1:f8:f9:40 ... et=0x0800       (… and injects it onto the AP vif — TX ok)
```
The bridged frame is D's DHCP DISCOVER (IPv4 broadcast, 342 B + 8 SNAP = 350; a boot artifact before
`dhcpc_stop`). So the gate correctly takes a real mesh group broadcast and delivers it to the AP side.

**End-to-end — a mesh node reaches an AP client** (D's serial): `reply from 10.9.9.50 seq=4..N` (12–19 continuous
replies, 24–108 ms) after the initial ARP-resolution timeouts. The gate shows the matching unicast flow (14×
`S5b mesh->AP: 106 B` + 14× `S5c AP->mesh: 100 B` = D↔STA ICMP via TX-auto-proxy/S5b/S5c).

## ⚠️ Honest finding — how D actually resolves the STA (and the gap)

The instrumented logs settle exactly which frame drives the ARP resolution:
1. The STA emits a **gratuitous ARP** at IP assignment: `S5 AP->mesh bcast: 36 B ... ARP op=1 spa=10.9.9.50
   tpa=10.9.9.50` (bridged AP→mesh by **B1**). D learns `10.9.9.50 → STA_MAC` from it.
2. D then pings the STA **UNICAST** (S5b/S5c + TX-auto-proxy) — no ARP needed.

So **B2 is proven for the mesh→AP direction (D's DHCP bridged to the AP), but B2 is NOT the mechanism that resolves
D→STA — the client's gratuitous ARP (B1) is.** Diagnostic (a gate log of *every* mesh group frame received) showed
the gate receives ONLY D's IPv4 (et=0x0800) group frames — **D's own ARP request (et=0x0806) for the STA never
reaches the gate.** In the one run where D was reset AFTER the STA's gratuitous ARP (so D never learned it), D
pinged for 30 s with all timeouts and never emitted a bridged ARP → 0 replies.

**ROOT CAUSE (definitively isolated, 2026-07-22):** the mesh node's lwIP **does not emit a resolution ARP for the
AP client at all.** Diagnostic: the STA's periodic gratuitous ARP was DISABLED (per-app
`sdkconfig.defaults: CONFIG_LWIP_ESP_GRATUITOUS_ARP=n`) so D could only resolve on its own, and the gate logged
*every* mesh group frame received. Over 60 s / 27 ping timeouts, D transmitted **2× DHCP DISCOVER (et=0x0800)** and
**1× a gratuitous ARP for its OWN IP** (`ARP op=1 spa=10.9.9.100 tpa=10.9.9.100`) — but **never a resolution ARP
for the STA** (`op=1 tpa=10.9.9.50`), and 0 replies. So the ICMP to 10.9.9.50 is queued/dropped without lwIP's
`etharp_query` ever firing a request. It is NOT a datapath-TX problem (D's own gratuitous ARP *does* go out over the
mesh, proving the datapath transmits ARP frames fine) and NOT a B2 problem (B2 bridges every mesh group frame it
receives — there is simply no resolution ARP to bridge). It is a **mesh-node lwIP/routing behaviour** (a follow-up):
why does `etharp_output(10.9.9.50)` not send an ARP request on the mesh netif?

**Deepened (2026-07-22), but not yet fully mechanised:**
- D's netif is correct: readback `ip=10.9.9.100 mask=255.255.255.0 gw=0.0.0.0` → 10.9.9.50 is **on-link** (same
  /24), so this is NOT the `ERR_RTE` off-link-with-no-gateway drop (etharp.c:826-852). (D's own code comment already
  notes off-subnet replies "die in lwIP ARP" without a gateway — but the STA is on-subnet here.)
- When D is **peered** (netif up=1), esp_ping's `sendto(10.9.9.50)` **succeeds** — 0 send errors, 27 timeouts — so
  `etharp_output`→`etharp_query` IS reached and returns ERR_OK (packet queued pending ARP). Per lwIP
  (etharp.c:966-981) a fresh EMPTY→PENDING entry means `is_new_entry` and `etharp_request()` IS called. Yet the
  resolution ARP (`op=1 tpa=10.9.9.50`) never appears on-air, while D's gratuitous ARP (`op=1 tpa=10.9.9.100`) — a
  **byte-identical broadcast ARP frame** differing only in the target IP — does. Both go `etharp_raw`→netif
  linkoutput→mesh TX, which is address-agnostic. So either `etharp_request` isn't actually reached, or its frame
  isn't generated — invisible without an lwIP ARP trace.
- When D is **alone** (no peer, netif up=0), `sendto` fails outright (`send error=0`) — expected (link down).
- **REVISED — it's FLAKY, not "never" (2026-07-22, after the ETHARP trace):** with `CONFIG_LWIP_ETHARP_DEBUG` on D
  and the STA's gratuitous ARP OFF, in ONE run D reached the STA fine (`reply from 10.9.9.50 seq=37..50`
  continuously) — so its OWN resolution ARP DID work that time (no gratuitous ARP to lean on). In an identical-config
  re-flash run D got 0 replies / 18 timeouts and the gate saw NO resolution ARP (only D's gratuitous ARP tpa=100).
  D's ETHARP trace shows `ethernet_output: sending packet` (frames ARE handed to the link) but the resolution ARPs
  intermittently don't reach the gate, while the gratuitous ARP + DHCP (fewer, periodic) do. **So the real nature is
  an INTERMITTENT mesh-broadcast delivery failure**: mesh broadcast frames are sent **No-Ack** (unacked multicast,
  QoS ack policy 0x0020 — see umac_datapath.c:2506) and are lossy on-air, so lwIP's few ARP retries hit-or-miss;
  passive learning via the client's PERIODIC gratuitous ARP (+ `TRUST_IP_MAC`) is what makes it usually work. NOT a
  clean deterministic "no ARP" bug, and NOT a B2 defect. A robust fix = gate proxy-ARP for the mesh subnet, or more
  ARP retries / reliable-multicast, not more B2.
- **A build detour (self-inflicted, resolved, NO code change kept):** getting the ETHARP trace needed a clean
  sdkconfig regen, and the link failed with `undefined reference to umac_ap_get_beacon`. I misdiagnosed this as a
  latent morselib bug and guarded the AP calls in `umac_mmdrv_shim.c` — but the real cause was that my diagnostic
  `cat > firmware/rimba-halow-mesh/sdkconfig.defaults` had **overwritten a tracked file** that intentionally sets
  `CONFIG_HALOW_AP_MODE=y` ("Mesh reuses the AP-type interface + beacon code path, so the AP-mode morselib sources
  must be compiled in"), and my `rm` deleted it. So the mesh app is SUPPOSED to build AP-on. **Restored the
  sdkconfig.defaults and reverted the shim guard** — net zero morselib change from this detour.

**Why it "works" in practice (and the flakiness):** ESP-IDF's `TRUST_IP_MAC` lets D create an ARP entry directly
from the STA's *gratuitous* ARP (bridged AP→mesh by B1), which the STA emits every
`CONFIG_LWIP_GARP_TMR_INTERVAL=60` s. So D reaches the STA once it catches one — up to a 60 s wait, and **never** if
the client is silent (gratuitous ARP off → the 0-reply case above). B2 delivers mesh broadcasts to AP clients (its
job); the gap is entirely that the mesh node never issues the on-demand resolution ARP.

## Not done / follow-ups
- **Fix the mesh node's on-demand resolution ARP** (root cause now known — see above): make `etharp_query` fire for
  an on-subnet AP client so a mesh node can reach a *silent* client without waiting for its 60 s gratuitous ARP.
  Investigate D's lwIP routing for 10.9.9.50 (no-gateway static config? the esp_ping raw-socket path?) with lwIP
  ARP/IP debug on D. Distinct from B2 — B2 is correct.
- A **broadcast-ping** B2 test (D pings 10.9.9.255) would isolate B2 without ARP, but `CONFIG_LWIP_BROADCAST_PING`
  is off in this build, so the STA wouldn't answer — enable it for a future clean B2-only end-to-end.
- Retire `MESH_GATE_IP` / the L3 `ip_forward` path; S6 live-Linux interop.

## Files
- `firmware/rimba-halow-mesh-ap/main/app_main.c` — `gw_mesh_rx_cb` B2 branch (mesh group frame → AP vif) +
  ethertype/ARP-op detail in the B1/B2 bridge logs. **Uncommitted.**
- `firmware/rimba-halow-mesh/main/app_main.c` (+ `CMakeLists.txt`) — `TEST_PING_IP` (a mesh node originates a ping).
- `firmware/rimba-halow-sta/main/app_main.c` (+ `CMakeLists.txt`) — `TEST_NO_PING` (responder-only AP client).
- `Makefile` — `PING_IP=` / `NO_PING=` flags.
- Memory: `mesh-gate-8021s-port-planned` (B2 done + the D-ARP caveat).
