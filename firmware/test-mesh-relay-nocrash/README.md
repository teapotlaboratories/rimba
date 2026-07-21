# test-mesh-relay-nocrash — Relay stability under load (no hw-restart)

**Status: new (2026-07-20) — developed on the pre-forward-port morselib 2.10.4 baseline; not yet
hardware-verified.** All role apps are `test-*`.

## Rig — the same forced line as `test-mesh-relay`, but the roles are RE-CAST

The relay is the **reporter** here (`hw_restart_counter` is the relay's own stat, and only a
reporter's console is scraped), so the origin and dest become support roles:

| role | device | app |
|---|---|---|
| origin (support) | board0 | `test-mesh-relay-nocrash` — drives a long ping burst to load the relay |
| dest (support) | board1 | `test-mesh-relay-nocrash` — responder |
| relay **[REPORTER]** | board2 — MANDATORY fully wired | `test-mesh-relay-nocrash` — forwards, then checks `hw_restart_counter` |

Support roles boot first; the relay (reporter) is flashed **last**. The origin waits for the relay's
plink, then pings for ~90 s so the relay is under sustained forwarding load across its measurement
window.

## What it proves / does not prove

- **Proves:** an ESP relay forwards a sustained mesh load **without a silent hw-restart** — its `hw_restart_counter` does not increment across the forwarding window. This catches the root-caused **interrupt-WDT SPI-host-teardown** crash-and-recover that a delivery check alone can miss (a brief crash-recover inside a ping window still delivers most pings).
- **Does NOT prove:** throughput, nor the SW-CCMP correctness `test-mesh-relay` covers. It also needs **real** forwarding load: if the endpoints never peer the relay, it lands **INCONCLUSIVE** (a relay under no load cannot crash under load — never a false PASS).

## Structural assertion

- **forwarding-path:** BOTH endpoints peered the relay (there was real load).
- **no-hw-restart:** `hw_restart_counter` unchanged across the ~40 s window.

PASS iff both. FAIL if the counter climbed (silent hw-restart under load). INCONCLUSIVE if the relay never got both peers.

## How to run

```sh
make test-t2 TEST=mesh-relay-nocrash
```

Needs board2 powered (`tools/ppk2_hold.py`) as the relay — **doubly** so here: the interrupt-WDT crash this guards was only ever seen on a *wired* relay (board2); board0/board1 as relay produce a different (missing-solder) crash. The orchestrator refuses the relay role on board0/board1.

## Timing

Sized to `REPORTER_TIMEOUT_S` (130 s from the relay's flash to `TEST|END`): relay boot (~25 s) + peer-wait (45 s) + forwarding window (40 s). If a cold bench needs longer for the relay to peer both endpoints, widen the window budget (`RELAY_PEER_WAIT_S` / `FORWARD_WINDOW_S` in `app_main.c`) — keeping the total under 130 s.

## Line topology (the three MACs) — build-time arguments, not hardcoded

Identical scheme to `test-mesh-relay`: the symmetric app self-selects its role by MAC
(`MESH_ORIGIN_MAC` / `MESH_DEST_MAC` / `MESH_RELAY_MAC`, passed by the harness from the manifest
`BENCH` registry to every flash). No MAC is hardcoded.

## Firmware

`firmware/test-mesh-relay-nocrash/` (symmetric 3-node; role by MAC; RELAY is the reporter, origin
drives traffic). Provenance: `tools/regtest/t2_tests.py` (`MESH_RELAY_NOCRASH`), `make test-t2 TEST=mesh-relay-nocrash`.
