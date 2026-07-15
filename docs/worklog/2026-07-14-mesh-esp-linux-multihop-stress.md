# 2026-07-14 — ESP↔Linux 3-hop mesh stress (board0→board1→board2→chronite) + a #20 counter-example

**Status: PASS** (both directions), plus two notable findings: (1) a **clean counter-example to the `#20`
"universal FW limitation"** — a HW-crypto Linux node received thousands of multi-hop A4≠TA forwards; (2) an
observed board0 app-pinger stall that was **TX saturation, not a mesh fault**. Encryption verified on all
four nodes. Radio-silent afterward. No firmware change (the defrag-before-decrypt fix was untouched).

## Setup

Forced 3-hop line, ESP allowlists only (chronite unrestricted):
`board0(10.9.9.136) → board1 → board2 → chronite(Linux, 10.9.9.2)`.
- board0 allowlist `{board1}`; board1 `{board0, board2}`; board2 `{board1, chronite 3c:22:7f:37:51:38}`.
- The morselib allowlist gates **both** peering (`umac_mesh.c:1659/2820`) **and** HWMP
  (`:2362`), so chronite peers ONLY board2 (a leaf off board2); board0/board1 show `BLOCKED` in chronite's
  `iw station dump`. Return path chronite→board0 is forced via board2→board1→board0 (mpath `HOP_COUNT 3`),
  single-route, no flap. Allowlist set **before** `mmwlan_mesh_start` (start-window race, app_main.c:271).
- Mesh = secured SAE+AMPE+CCMP, password `rimbamesh2026` (ESP `umac_mesh.c:81`, chronite
  `/tmp/wpa-interop.conf key_mgmt=SAE`). chronite fw 1.17.8, **`no_hwcrypt=N` (HW crypto) — verified**.
- Frames 200 B (< single-PPDU limit → never fragmented; isolates the multi-hop/interop question from the
  fragmentation path, which anyway can't cross to Linux — mac80211 uses the opposite fragment/decrypt order).

## Encryption — verified on all four nodes

1. **chronite (control plane):** `wpa_state=COMPLETED`, `key_mgmt=SAE`, `pairwise_cipher=CCMP`,
   `group_cipher=CCMP`; board2 = ESTAB / authenticated=yes / authorized=yes. Since chronite *requires* SAE,
   board2 reaching authorized proves board2 did SAE+AMPE+CCMP.
2. **ESP firmware:** peers set to `MMWLAN_SAE` (not `MMWLAN_OPEN`) at ESTAB (`umac_mesh.c:621`);
   `g_mesh_sw_crypto = true` (`:146`); full SAE(Dragonfly)+AMPE(AES-SIV)+CCMP compiled in.
3. **Proof-by-flow (covers board0+board1):** a secured mesh **drops** unprotected data —
   `umac_datapath.c:702` ("Received NON EAPOL frame in plain text") on the ESPs, and CCMP-required mac80211
   on chronite. ~15 000 frames flowed bidirectionally across all 3 hops → every frame was CCMP-protected end
   to end (a single plaintext node would black-hole the path).

## ⭐ #20 counter-example (HW-crypto Linux DID receive multi-hop A4≠TA forwards)

The `#20` memory (`mesh-20-hwcrypto-forward-host-stack`, worklog `2026-07-11-mesh-20-linux-also-withholds-...`)
states, bench-proven, that a HW-crypto morse node **withholds** any A4≠TA 4-address mesh forward at the FW —
"HW-crypto mesh multi-hop is impossible on this firmware, on Linux too" (its proof: board1→board0→chronite,
board0 TX'd 10 forwards, chronite's raw socket saw 0; chronite↔board0 direct 40/40).

**This run refutes that for this config.** board0↔chronite here is *only* reachable as an A4≠TA forward at
the board2→chronite hop (A4=SA=board0 ≠ TA=board2), and it delivered at the **application layer** (ICMP
round-trips), which is stronger than a raw-socket count:
- board0→chronite: **3000/3000** application replies (chronite received board0's requests = A4≠TA forwards
  and replied).
- chronite→board0: **8987/9003 (uni) + 2998/3017 (bidir)** — chronite received board0's replies (also A4≠TA
  forwards) ~99%+.

chronite is confirmed **HW crypto** (`no_hwcrypt=N`), so this is NOT the "SW-crypto Linux delivers" case the
#20 memory already allows. **Needs reconciliation** — likely the original #20 result was confounded (tiny
10-forward sample + a raw-socket wlan1 capture that may not reflect kernel delivery), or the 2-hop-vs-3-hop
/ current-tree differs. Either way, **multi-hop encrypted ESP→Linux mesh demonstrably works here**, so #20's
"universal / impossible" framing is too strong. Recommend re-running the exact original #20 rig
(board1→board0→chronite with an application-layer probe, not just a raw socket) to settle it.

## Stress results

| Run | Direction(s) | Delivery | Crashes | Heap (leak) |
|-----|--------------|----------|---------|-------------|
| **Unidirectional, 30 min** | chronite→board0 | **8987 / 9003 = 99.82 %** | 0 | flat (±20 B) |
| **Bidirectional, 20 min** | board0→chronite | **3000 / 3000 = 100 %** | 0 | flat |
| | chronite→board0 | **2998 / 3017 = 99.37 %** | | |

All 3 ESPs ran continuously (unbroken 5 s-heartbeat chains for the full duration), avg RTT ~48 ms over 3
hops. chronite's harmless churn (it keeps failing SAE to the allowlist-refusing board0/board1) does not
touch the board2 data path.

## board0 app-pinger stall — TX saturation, not a fault

A first bidirectional attempt at **200 ms both ways** (~10 pkt/s through board0: 5/s replies to chronite's
flood + 5/s own pings) **froze board0's `esp_ping` at ~292** (ok frozen, **to=0** → blocked on send, not
timing out) while board0 stayed alive (heartbeat advancing, chronite→board0 still flowing). Dropping to
**400 ms each way** (~5/s through board0) fixed it completely — board0→chronite ran **3000/3000, 100 %**.
So the stall was board0's TX queue saturating under the reply+ping load, an app/rate artifact; the mesh,
crypto, and forwarding were never at fault. (Also confirms: run heavy inbound floods against an ESP as
*responder only*, or keep the aggregate per-node rate sane.)

## Disposition

No firmware change — the halow working tree is still exactly the defrag-before-decrypt fix (72 ins / 235
del). Temp app scaffolding (`MESH_CHRONITE_LINE`) git-restored. Radio-silent: 3 ESPs → `rimba-hello`;
chronite `wlan1` down + s1g stopped; chronium `morse0`/`wlan1` down.

## Gotchas (reusable)

- Forcing a Linux node into a multi-hop line: give the **ESP** allowlists (they gate peering + HWMP, so the
  Linux node becomes a leaf off its single allowed ESP); the Linux node needs no restriction.
- The morselib allowlist is bidirectional-in-effect and must be set **before** `mmwlan_mesh_start`.
- chronite bring-up: `cp ~/wpa-interop.conf /tmp/` then `wpa_supplicant_s1g -B -Dnl80211 -i wlan1 -c
  /tmp/wpa-interop.conf`; `ip addr add 10.9.9.2/24 dev wlan1` (IP not persistent). Teardown safely: find the
  literal `-i wlan1` s1g PID and `kill` it — **never** `killall`/`pkill -f` (self-matches your ssh cmdline;
  SSH rides wlan0).
- A sustained inbound ICMP flood can wedge an ESP's `esp_ping` (blocks on send, no timeout) without any
  crash — keep the aggregate per-node rate ≲ ~5/s or make the flooded node a responder.
