# test-mesh-linux — ESP ↔ Linux 802.11s mesh interoperability

**Status: automated + hardware-verified** (2026-07-16: board2 peered SAE+AMPE with chronite +
13/15 ICMP replies). The **gold-standard** T2 test: the ESP interoperates with a real Linux
`mac80211` + `morse_driver` stack, not just another ESP running the same morselib.

## Rig

| role | device | notes |
|---|---|---|
| **linux** (support) | `chronite` (real Linux mesh node) | brought up + torn down over ssh by `tools/regtest/linux_peer.py` (no ESP app) |
| **esp** (reporter) | board2 | `test-mesh-linux`: joins the same mesh, peers with + pings the Linux node |

## What it proves / does not prove

- **Proves:** an ESP completes SAE+AMPE mesh peering against a genuine Linux stack **and** exchanges
  ICMP data with it — byte-level interop of the peering handshake + the secured (host SW-CCMP) data
  path.
- **Does NOT prove:** multi-hop *through* a Linux node, or on-air frame byte-equivalence (that's the
  separate chronium-monitor capture the on-air rule points at).

## Structural assertion

An ESTAB peer whose MAC == the Linux node's wlan1 MAC (`3c:22:7f:37:51:38`) appears — peering is
binary (SAE+AMPE either closed against mac80211 or it didn't) — **and** ≥6/15 ICMP replies come back
from the Linux node's mesh IP (`10.9.9.2`). 0 replies despite peering = FAIL; partial = INCONCLUSIVE.

## Matching parameters (why it interoperates)

The ESP app uses mesh ID **`rimba-smesh`** (matching the Linux `docs/reference/captures/wpa-smesh.conf`),
SAE password `rimbamesh2026` (compiled into morselib), S1G ch27 — only the mesh ID differs from the
ESP↔ESP `mesh-peering` test (which uses `rimba-mesh`).

## How to run

```sh
make test-t2 TEST=mesh-linux
```

The orchestrator: `linux_peer.bring_up_mesh(chronite)` pushes the reference config if missing, starts
`wpa_supplicant_s1g` in mesh mode, assigns `10.9.9.2` → flashes `test-mesh-linux` to board2 →
captures the verdict → **tears chronite back down** (wlan1 DOWN) and returns board2 to `rimba-hello`.
chronite must be reachable by ssh (it is, via `~/.ssh/config`).
