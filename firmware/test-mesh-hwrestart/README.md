# test-mesh-hwrestart — can a mesh node survive a chip restart?

S1 of the mesh `hw_restart` work. It makes the defect **reproducible on demand** instead of something
you wait for: the node peers, forces a chip restart on itself, and then you look at whether it ever
comes back **on air**.

Mechanism and staging: `docs/mesh-ap/rimba-mesh-hw-restart-design.md`.
Session record: `docs/worklog/2026-08-05-mesh-hwrestart-s1-metric-reads-the-air.md`.

## The verdict this app prints is NOT the recovery verdict

Read this before changing anything here.

The first version of this fixture scored recovery on `mmwlan_mesh_peer_count()` and returned **PASS on a
node that was transmitting nothing at all**. That function (`umac_mesh.c:1404`) walks `mesh_peers[]`, a
**host-side array** that `mmdrv_deinit()`/`mmdrv_init()` never touches, so it reads 1 whether the node is
meshing or stone deaf — measured holding at 1 for a full 45 s while the monitor saw zero frames.

The trap is structural, not an oversight:

> **"Silently deaf" is by definition the state in which every host-side view still looks healthy.**

So no on-device probe can score this, and any replacement metric that reads RAM on this board will fall
into it again. A fixture that certifies a broken node as healthy is worse than no fixture.

The work is therefore split — the same shape as `test-raw-rps`, whose `TEST|` gates are preconditions
with the real verdict decoded from a capture:

| | scores | today |
|---|---|---|
| **this app** | did the trigger really restart the chip (`hw_restart_counter`) | **PASS** — the fixture works |
| **`tools/mesh_hwrestart_cap.py`** | do this board's own beacons ever return | **FAIL** — the S2 defect |

`mmwlan_mesh_peer_count()` is still printed, demoted to INFO and labelled unscored, so the stale number
stays visible next to the on-air truth. That contrast is the finding — don't delete it.

## Rig

Two ESP boards plus chronium in monitor mode. **No board2, so no PPK2.**

- **board1** — this app (node under test).
- **board0** — any mesh node on the same `MESH_ID`; `test-mesh-gate-node NO_PING=1` is the cheapest
  responder and needs no special build. It only has to be there to peer and to keep beaconing, which is
  what makes it the liveness control.
- **chronium** — `morse0` monitor on S1G ch27, per `docs/reference/rimba-linux-halow-monitor.md`.

## Running it

Start the scorer **before** resetting the node under test, so the capture contains the pre-restart
baseline:

```sh
# chronium — wlan1 must be UP and TUNED before morse0 will carry anything
sudo ip link set wlan1 up && sudo iw dev wlan1 set freq 5560 && sudo ip link set morse0 up
sudo python3 mesh_hwrestart_cap.py e2:72:a1:f8:f9:40 e2:72:a1:f8:ef:a4 150   # <nut> <peer> [dur]

# dev box
make flash APP=test-mesh-gate-node BOARD=proto1-fgh100m NO_PING=1 PORT=<board0>
make flash APP=test-mesh-hwrestart BOARD=proto1-fgh100m PORT=<board1>
```

The board prints its mesh MAC as `TEST|INFO|mesh-mac|…` — that is the SA the scorer filters on.

## Expected result today

```
TEST|STEP|restart-ran|PASS|hw_restart_counter 0 -> 1
RESULT|FAIL|the node's beacons stopped at +37.9s and NEVER returned (112.1s of silence ...)
```

**The FAIL is the deliverable.** It converts the 2026-07-15 crash observation into a repeatable red test
that S2 (`umac_mesh_handle_hw_restarted()`) has to turn green.

## Footguns

- **The liveness control is what makes the FAIL a measurement.** The peer board is unaffected by the
  restart, so its steady beacon rate proves the monitor still had the channel. If it drops too, the tool
  reports INCONCLUSIVE rather than FAIL — absence of evidence is only evidence of absence while the
  control is still being received.
- **pyserial lives in the IDF venv**, not system `python3`. A serial capture piped through `grep`
  swallowed the traceback once and cost a whole run, because the app parks forever after `TEST|END`.
- **`tcpdump` cannot decode S1G beacons** — they print as "unknown 802.11 frame type (3)". Parse the
  bytes: `fc == 0x1c`, SA at frame offset `[4:10]`.
- **This fixture cannot reach the AP-side assert (S3).** It is mesh-only, so it never instantiates an AP
  vif and never hits `MMOSAL_ASSERT(false)`. S3 needs its own reproducer.
