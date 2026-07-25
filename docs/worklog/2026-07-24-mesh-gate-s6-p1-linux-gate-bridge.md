# 2026-07-24 — S6 P1 (Case A): a Linux mesh gate L2-bridges an off-mesh host to an ESP node

**Status:** ✅ **DONE + ON-AIR PROVEN.** The **wildcard is resolved — positively**: the morse driver
**does permit bridging the 802.11s mesh vif**, and an off-mesh host behind a **Linux** gate's L2 bridge
reaches an ESP mesh node end-to-end. This is S6 P1 (Case A) from `rimba-mesh-ap-s6-interop-plan.md` — the
one leg of the mesh-gate interop that had never been attempted on this bench and whose feasibility
(can the morse mesh vif even be bridged?) was the genuine unknown.

## Result

**Off-mesh DS host `10.9.9.60` ↔ ESP mesh node `10.9.9.108`, through a Linux gate's L2 bridge:
14/14 replies, 0% loss, 9–21 ms** (a warm-up run before it was 7/8 while ARP resolved through the bridge).
The datapath is **bidirectional** in one ping: the request goes DS → gate (proxied into the mesh) → ESP
(the ESP's AE-RX), and the reply goes ESP → gate (the ESP's `tx_proxied`/`send_to_gates`) → DS. And
`iw dev wlan1 mpp dump` on the gate shows the plan's exact proof:
```
DEST ADDR         PROXY NODE        IFACE
e2:72:a1:f8:f0:08 e2:72:a1:f8:f0:08 wlan1      <- the ESP-proxied source, learned by Linux mpp_path_add
```
i.e. the Linux gate's `mpp_path_add` correctly parsed the ESP's on-wire AE frames (`mesh.c` /
`mesh_pathtbl.c:722`) — the ESP↔Linux datapath is on-wire compatible, not just ESP↔ESP.

## THE wildcard, answered: the morse mesh vif IS bridgeable

The plan flagged "whether the morse driver permits bridging the mesh vif is unverified" as the load-bearing
unknown. **It does**, with a specific, non-obvious recipe (each clause cost a failed attempt):

1. **Interface TYPE matters.** Enslaving `wlan1` while it is `type managed` is rejected
   (`Device does not allow enslaving to a bridge` — the classic can't-bridge-a-wifi-station limitation).
   Set it to **`type mesh point`** first — a mesh vif *is* bridgeable (that is the whole basis of
   802.11s gateway support; the kernel `mpp_path_add` learns DS-side hosts from bridge-forwarded frames).
2. **Enslave-FIRST, while DOWN.** You cannot enslave the mesh vif once the mesh is UP (also rejected). So:
   `wlan1` down → `iw set type mesh` → `ip link set wlan1 master mesh-br` → *then* start
   `wpa_supplicant_s1g`. **The bridge membership survives the mesh join** (`brif` still shows the mesh vif
   after `MESH-GROUP-STARTED`).
3. **Warm the radio first.** A **cold** bridged mesh-join fails at the channel set
   (`morse_cmd_vendor_set_channel … rc -32768`, `Failed to init mesh`). Bring a **bare** mesh up once
   (tunes the MM6108 to ch27), tear the daemon down (keep the mesh-point type), *then* do the enslave-first
   join — the re-set of the already-tuned channel succeeds. (chronite happened to be pre-warmed from an
   earlier session, which is why its first enslave-first join "just worked"; chronogen needed the explicit
   warm-up.)
4. **veth must be DOWN to enslave.** `ip link set ds0 master mesh-br` returns rc=0 but **silently no-ops if
   ds0 is UP** — set the veth down, `master`, then up. (Cost ~3 confused debug passes; `ip -d link show ds0`
   showing *no* master despite rc=0 is the tell.)

## Bench setup (as built)

```
             mesh-br (10.9.9.2/24)  [Linux gate: chronogen]
             /            \
   wlan1 (mesh, ch27)     ds0 ── veth ── ds0p @ netns dsns (10.9.9.60)   <- the off-mesh "DS host"
        |                                                                    (eth0 has NO carrier, so
   802.11s rimba-smesh                                                        the DS is a veth+netns)
        |
   board2 = ESP mesh node (test-mesh-gate-node, MESH_ID=rimba-smesh, NO_PING responder @ 10.9.9.108)
```
- Gate mode: `iw dev wlan1 set mesh_param mesh_hwmp_rootmode 4 / mesh_gate_announcements 1 /
  mesh_hwmp_rann_interval 5000 / mesh_fwding 1`.
- The DS host is a **veth pair into a network namespace** (`dsns`, `10.9.9.60`) because chronite/chronogen
  `eth0` has **no carrier** — there is no wired device on the segment. A netns is a clean, separate L2
  endpoint *behind* the gate (exactly what an off-mesh DS host is), and needs no extra hardware.
- Discovery re-confirm (Case A step 1, on the LIVE Linux gate): the ESP peered with the gate
  (`estab_peers=1`) and learned it via RANN (`known_gates=1`, `GATE DISCOVERED via RANN`) — through the
  bridged mesh, proving the control-plane survives bridging.

## Bench reality: chronite wedged; chronogen the fallback gate

- The plan named **chronite** as the Linux gate. It came up as a `rimba-smesh` gate + bridge fine and the
  ESP peered — but **~2–3 min into bridged-mesh operation its management WiFi (`wlan0`) dropped**
  (`No route to host`; chronium on the same AP stayed reachable, so chronite-specific). It did **not**
  recover in 4.5 min (a Pi 5, so not the chronosalt radio-up brownout; matches the "chronite mesh flaky"
  footgun). Its MM6108 **mesh** radio stayed up (still a peer of chronogen in the station dump) — only the
  Pi's management link died, so I couldn't ssh in to reload the driver. **It needs a physical power-cycle**
  (no remote power control exists on the bench); that also recovers it.
- Pivoted to **chronogen** (the plan's documented fallback) — a Pi Zero 2 W. Bare mesh worked immediately;
  the bridged-mesh needed the warm-radio + veth-down gotchas above. Once set up, the datapath was rock
  solid (14/14, 0% loss). So a Pi Zero can serve as the bridged Linux gate for a low-rate test.

## ESP-side change

`firmware/test-mesh-gate-node` gained a **`TEST_MESH_ID` override** (`app_main.c` `#ifndef TEST_MESH_ID`
default `"rimba-mesh"`, + a `CMakeLists.txt` `target_compile_definitions` forward), mirroring
`test-mesh-gate-rx`/`test-mesh-ae`. This lets its NO_PING responder join `rimba-smesh` to be the far mesh
endpoint reached through a live Linux gate (`make flash APP=test-mesh-gate-node MESH_ID=rimba-smesh
NO_PING=1` → static `10.9.9.108` responder). Fixture-only, split-rule compliant, no morselib change.
Key re-usable fact: the mesh SAE password `rimbamesh2026` is compiled into morselib and identical across
`rimba-mesh`/`rimba-smesh`, so a `MESH_ID=` override is all that is needed to point a fixture at a live
Linux gate.

## Verification tier
**Live-device gold standard** — a real ESP node exchanges IP traffic with a real off-mesh host through a
real Linux 802.11s gate's L2 bridge, with the Linux `mpp` table showing the ESP-proxied source. Per
`[[verify-onair-chronium-monitor]]` (the RANN both-directions byte-diff, P3, is still to do).

## Cleanup (radio-silent, mostly)
board2 → `rimba-hello` + PPK2 powered OFF; chronogen mesh daemon killed, bridge + netns torn down, `wlan1`
down, `type managed` (its `wlan0` mgmt intact). **chronite is the exception — unreachable, still needs a
power-cycle** (which silences its lingering mesh radio too).

## Files
- `firmware/test-mesh-gate-node/main/app_main.c` (+`TEST_MESH_ID`) + `main/CMakeLists.txt` (+forward) —
  the fixture can now join `rimba-smesh` for S6 interop.
- Memory: `mesh-gate-8021s-port-planned` (S6 P1 done — Linux-gate L2 bridge proven; the wildcard is resolved).

## Next — S6 P2 / P3
- **P2 (Case B):** ESP gate ↔ Linux node — with Part 0 landed the ESP gate emits RANN; bring a Linux node
  up (needs a healthy gate node) and confirm it discovers the ESP gate (`iw dev wlan1 mpath dump`), then the
  AE/MPP leg (`test-mesh-ae LINUX_MAC=…`).
- **P3:** RANN both-directions byte-diff on chronium's `morse0` + gate-bit; ship the (already re-verified,
  P0) code-map.
- **Blocker to clear first:** power-cycle **chronite** (recovers it + silences its mesh radio).
