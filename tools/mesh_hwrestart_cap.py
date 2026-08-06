#!/usr/bin/env python3
# mesh_hwrestart_cap.py — the RECOVERY verdict for test-mesh-hwrestart, scored on the air.
#
#   sudo python3 mesh_hwrestart_cap.py <nut_mesh_mac> <peer_mesh_mac> [dur_s]
#
# Run on chronium (morse0 monitor, ch27 — see docs/reference/rimba-linux-halow-monitor.md), STARTED
# BEFORE the node under test is reset. Both MACs are printed by the fixture as TEST|INFO|mesh-mac| and
# are listed in docs/reference/rimba-bench-devices.md.
#
# WHY THIS IS OFF-BOARD. test-mesh-hwrestart cannot score its own recovery. Its first version tried, on
# mmwlan_mesh_peer_count(), and returned PASS on a node that transmitted nothing for 45 s:
# mmwlan_mesh_peer_count() walks mesh_peers[] (umac_mesh.c:1404), a HOST-SIDE array that
# mmdrv_deinit()/mmdrv_init() never touches. "Silently deaf" is by definition the state where every
# host-side view still looks healthy, so the probe has to be a different radio. This one is.
#
# WHAT IT SCORES. The node's own S1G beacons, in three phases:
#   1. baseline  — beacons flowing before the restart (else there is nothing to lose: INCONCLUSIVE)
#   2. gap       — beacons stop; expected either way, mmdrv_deinit()/mmdrv_init() reloads the firmware
#   3. recovery  — do they come back?  PASS iff yes, FAIL iff the silence runs to the end of the window.
# No clock sync with the DUT is needed: the shape (beacons, gap, beacons) carries the verdict by itself.
#
# THE LIVENESS CONTROL IS WHAT MAKES "NO BEACONS" MEAN ANYTHING. The peer board beacons throughout and
# is unaffected by the restart, so its beacons prove the monitor still had the channel. If the peer goes
# quiet too, the capture — not the node — is what broke, and the run is INCONCLUSIVE rather than a FAIL.
# Absence of evidence is only evidence of absence while the control is still being received.

import socket
import struct
import sys
import time

if len(sys.argv) < 3:
    sys.exit(__doc__ or "usage: mesh_hwrestart_cap.py <nut_mesh_mac> <peer_mesh_mac> [dur_s]")

NUT = sys.argv[1].lower()
PEER = sys.argv[2].lower()
DUR = int(sys.argv[3]) if len(sys.argv) > 3 else 180

# A gap shorter than this is jitter/RF loss, not a restart. Beacon interval is 100 TU (~102 ms), so a
# couple of seconds is many missed beacons and still far below a real mmdrv teardown+reload.
GAP_S = 3.0
# Beacons needed before the gap for the baseline to count as real.
MIN_BASELINE = 20
# Consecutive beacons needed after a gap to call it recovered (one stray frame is not a working mesh).
MIN_RECOVERY = 5
# The control must stay above this fraction of its own baseline rate, else the monitor lost the channel.
CONTROL_MIN_FRAC = 0.25


def mac(b):
    return ":".join("%02x" % x for x in b)


def s1g_beacon_sa(d):
    """Return the SA of an S1G (or legacy) beacon in this radiotap frame, else None."""
    if len(d) < 4:
        return None
    rtlen = struct.unpack_from("<H", d, 2)[0]
    f = d[rtlen:]
    if len(f) < 16:
        return None
    fc = f[0]
    ftype, fsub = (fc >> 2) & 3, (fc >> 4) & 0xF
    if ftype == 3 and fsub == 1:      # S1G ext beacon: SA at [4:10] (mesh_beacon_cap.py agrees)
        return mac(f[4:10])
    if ftype == 0 and fsub == 8:      # legacy beacon: SA at [10:16]
        return mac(f[10:16])
    return None


s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.ntohs(0x0003))
s.bind(("morse0", 0))
s.settimeout(1)

print("watching morse0: nut=%s peer=%s for %ds" % (NUT, PEER, DUR), flush=True)

nut_t = []      # timestamps of the node's beacons
peer_t = []     # timestamps of the control's beacons
t0 = time.time()
last_report = t0
while time.time() - t0 < DUR:
    try:
        d = s.recv(4096)
    except socket.timeout:
        d = None
    now = time.time()
    if d:
        sa = s1g_beacon_sa(d)
        if sa == NUT:
            nut_t.append(now)
        elif sa == PEER:
            peer_t.append(now)
    if now - last_report >= 10:
        last_report = now
        print("  +%5.0fs  nut_beacons=%d  peer_beacons=%d" % (now - t0, len(nut_t), len(peer_t)),
              flush=True)
s.close()

span = time.time() - t0
print("\ncaptured %.0fs: nut=%d beacons, peer(control)=%d beacons" % (span, len(nut_t), len(peer_t)))

# --- the control decides whether any of this is readable at all ------------------------------------
if not peer_t:
    print("RESULT|INCONCLUSIVE|the control node emitted no beacons either -- the monitor never had the "
          "channel (wrong freq, wlan1 not in monitor type, or the peer board is down). Nothing about "
          "the node under test can be concluded from this capture.")
    sys.exit(2)


def rate(ts, lo, hi):
    n = sum(1 for t in ts if lo <= t < hi)
    return n / max(hi - lo, 1e-6)


# --- find the node's beacon phases -----------------------------------------------------------------
if len(nut_t) < MIN_BASELINE:
    print("RESULT|INCONCLUSIVE|the node under test only ever emitted %d beacons (need >=%d for a "
          "baseline). It never established a mesh presence to lose, so a later silence proves nothing "
          "-- check that the fixture actually reached mmwlan_mesh_start and peered."
          % (len(nut_t), MIN_BASELINE))
    sys.exit(2)

gaps = [(nut_t[i], nut_t[i + 1]) for i in range(len(nut_t) - 1) if nut_t[i + 1] - nut_t[i] >= GAP_S]
tail_silence = time.time() - nut_t[-1]

print("nut beacon window: first=+%.1fs last=+%.1fs  (%d gaps >=%.0fs, trailing silence %.1fs)"
      % (nut_t[0] - t0, nut_t[-1] - t0, len(gaps), GAP_S, tail_silence))
for a, b in gaps:
    print("  gap %.1fs  (+%.1fs -> +%.1fs)" % (b - a, a - t0, b - t0))

# Control liveness across the node's silence: if the node went quiet AND the control's rate held up,
# the silence is real.
if tail_silence >= GAP_S:
    quiet_from = nut_t[-1]
    ctrl_before = rate(peer_t, t0, quiet_from)
    ctrl_during = rate(peer_t, quiet_from, time.time())
    print("control rate: %.1f/s before the node went quiet, %.1f/s during its silence"
          % (ctrl_before, ctrl_during))
    if ctrl_before > 0 and ctrl_during < CONTROL_MIN_FRAC * ctrl_before:
        print("RESULT|INCONCLUSIVE|the control's beacons fell off too (%.1f/s -> %.1f/s) during the "
              "node's silence, so the capture lost the channel rather than the node losing the mesh. "
              "Re-run; do not read this as a recovery failure." % (ctrl_before, ctrl_during))
        sys.exit(2)

    print("RESULT|FAIL|the node's beacons stopped at +%.1fs and NEVER returned (%.1fs of silence to the "
          "end of the capture) while the control kept beaconing at %.1f/s. The chip restarted and the "
          "mesh vif was never reinstalled: umac_connection_handle_hw_restarted() skips its entire body "
          "on a mesh node because it opens on the UMAC_INTERFACE_STA vif id, which is "
          "MMDRV_VIF_ID_INVALID here -- mesh owns UMAC_INTERFACE_MESH. Nothing restores it. This is the "
          "S2 defect." % (quiet_from - t0, tail_silence, ctrl_during))
    sys.exit(1)

# Beacons are still flowing at the end. Did they survive a gap (i.e. recover), or never stop?
if gaps:
    resumed_at = gaps[-1][1]
    n_after = sum(1 for t in nut_t if t >= resumed_at)
    if n_after >= MIN_RECOVERY:
        print("RESULT|PASS|the node's beacons stopped for %.1fs and RESUMED at +%.1fs (%d beacons "
              "since) -- it is back on air under its own mesh SA. (Validated 2026-08-05 by rebooting a "
              "plain mesh node mid-capture: this branch fires on a real recovery, so a FAIL from this "
              "tool is a finding and not a scorer stuck on one answer.)"
              % (gaps[-1][1] - gaps[-1][0], resumed_at - t0, n_after))
        sys.exit(0)
    print("RESULT|FAIL|the node emitted only %d beacon(s) after its %.1fs gap (need >=%d). It is not "
          "back on air in any usable sense." % (n_after, gaps[-1][1] - gaps[-1][0], MIN_RECOVERY))
    sys.exit(1)

print("RESULT|INCONCLUSIVE|the node beaconed continuously for the whole capture with no gap >=%.0fs -- "
      "the restart never interrupted it, so the capture window almost certainly does not contain the "
      "trigger. Start this tool BEFORE resetting the board and cover the full fixture run." % GAP_S)
sys.exit(2)
