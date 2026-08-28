#!/usr/bin/env python3
# mesh_hwrestart_cap.py — the RECOVERY verdict for test-mesh-hwrestart, scored on the air.
#
#   sudo python3 mesh_hwrestart_cap.py <nut_mesh_mac> <peer_mesh_mac> [dur_s] [--ap <ap_bssid>]
#
# Run on chronium (morse0 monitor, ch27 — see docs/reference/rimba-linux-halow-monitor.md), STARTED
# BEFORE the node under test is reset. Both MACs are printed by the fixture as TEST|INFO|mesh-mac| and
# are listed in docs/reference/rimba-bench-devices.md.
#
# --ap scores the S3 arm (AP_VIF=1), where the node runs an AP vif alongside the mesh — the shipped
# gateway's shape. The AP beacons under its OWN BSSID (a locally-administered address derived from the
# device MAC, not the mesh MAC), which the fixture prints as TEST|INFO|ap-mac|. Both vifs are then
# scored independently against the same capture and the run is only a PASS if BOTH came back: the
# mesh+AP defect this exists to catch is exactly the one where one slot recovers and the other stays
# dead on the chip, which a single-SA capture reads as a clean PASS.
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
# The gap is REQUIRED for a verdict, and that is deliberate. Without one there is no evidence a restart
# occurred at all, and the fixture has several early-exits that park before triggering one -- so a
# gapless capture is INCONCLUSIVE, never PASS. Cross-check the fixture's own restart-ran step.
#
# THE LIVENESS CONTROL IS WHAT MAKES "NO BEACONS" MEAN ANYTHING. The peer board beacons throughout and
# is unaffected by the restart, so its beacons prove the monitor still had the channel. If the peer goes
# quiet too, the capture — not the node — is what broke, and the run is INCONCLUSIVE rather than a FAIL.
# Absence of evidence is only evidence of absence while the control is still being received.

import socket
import struct
import sys
import time

argv = sys.argv[1:]
AP = None
if "--ap" in argv:
    i = argv.index("--ap")
    if i + 1 >= len(argv):
        sys.exit("--ap needs the AP BSSID (the fixture prints it as TEST|INFO|ap-mac|)")
    AP = argv[i + 1].lower()
    del argv[i:i + 2]

if len(argv) < 2:
    sys.exit(__doc__ or
             "usage: mesh_hwrestart_cap.py <nut_mesh_mac> <peer_mesh_mac> [dur_s] [--ap <ap_bssid>]")

NUT = argv[0].lower()
PEER = argv[1].lower()
DUR = int(argv[2]) if len(argv) > 2 else 180
if AP in (NUT, PEER):
    sys.exit("--ap must differ from the mesh and control MACs; the AP vif cannot share the mesh's "
             "address (umac_interface.c derives a locally-administered one for it)")

# A gap shorter than this is jitter/RF loss, not a restart. Beacon interval is 100 TU (~102 ms), so
# 2 s is ~20 consecutive beacons -- far beyond jitter, and below a real mmdrv teardown+reload.
#
# Measured, not guessed. With the S2 recovery in place the teardown+reload outage is 2.94 s (pcap,
# 2026-08-06, binned against the fixture's own trigger timestamp). The original 3.0 s threshold sat
# just ABOVE that, so the restart's own gap went unseen and the run still PASSed -- on the strength of
# an unrelated gap (the board's reboot) that happened to share the capture. Right answer, wrong
# evidence, which is the failure mode this whole tool exists to avoid.
GAP_S = 2.0
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

print("watching morse0: nut=%s peer=%s%s for %ds"
      % (NUT, PEER, (" ap=%s" % AP) if AP else "", DUR), flush=True)

nut_t = []      # timestamps of the node's mesh beacons
peer_t = []     # timestamps of the control's beacons
ap_t = []       # timestamps of the node's AP beacons (S3 arm only)
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
        elif AP is not None and sa == AP:
            ap_t.append(now)
    if now - last_report >= 10:
        last_report = now
        print("  +%5.0fs  nut_beacons=%d  peer_beacons=%d%s"
              % (now - t0, len(nut_t), len(peer_t),
                 ("  ap_beacons=%d" % len(ap_t)) if AP else ""), flush=True)
s.close()

span = time.time() - t0
t_end = time.time()
print("\ncaptured %.0fs: nut=%d beacons, peer(control)=%d beacons%s"
      % (span, len(nut_t), len(peer_t), (", ap=%d beacons" % len(ap_t)) if AP else ""))

# --- the control decides whether any of this is readable at all ------------------------------------
if not peer_t:
    print("RESULT|INCONCLUSIVE|the control node emitted no beacons either -- the monitor never had the "
          "channel (wrong freq, wlan1 not in monitor type, or the peer board is down). Nothing about "
          "the node under test can be concluded from this capture.")
    sys.exit(2)


def rate(ts, lo, hi):
    n = sum(1 for t in ts if lo <= t < hi)
    return n / max(hi - lo, 1e-6)


def find_gaps(ts):
    return [(ts[i], ts[i + 1]) for i in range(len(ts) - 1) if ts[i + 1] - ts[i] >= GAP_S]


# Whether a restart happened at all is evidence about the RADIO, not about one vif: the firmware
# reload takes both slots down together. So it is decided once, across every scored SA, and then used
# by each of them. Scoring it per-SA would poison the verdict on the S3 arm the moment one slot
# recovered inside GAP_S while the other showed the outage -- the fast slot would read as "no evidence
# of a restart" even though the capture plainly contains one.
restart_observed = bool(find_gaps(nut_t)) or bool(find_gaps(ap_t))


def score(label, ts, what):
    """Return (exit_code, message) for one scored SA. 0=PASS, 1=FAIL, 2=INCONCLUSIVE."""
    if len(ts) < MIN_BASELINE:
        return (2, "INCONCLUSIVE|%s: the node only ever emitted %d beacon(s) under its %s (need >=%d "
                   "for a baseline). It never established a presence to lose, so a later silence "
                   "proves nothing -- check that the fixture actually brought that vif up."
                   % (label, len(ts), what, MIN_BASELINE))

    gaps = find_gaps(ts)
    tail_silence = t_end - ts[-1]

    print("%s beacon window: first=+%.1fs last=+%.1fs  (%d gaps >=%.0fs, trailing silence %.1fs)"
          % (label, ts[0] - t0, ts[-1] - t0, len(gaps), GAP_S, tail_silence))
    for a, b in gaps:
        print("  gap %.1fs  (+%.1fs -> +%.1fs)" % (b - a, a - t0, b - t0))

    # Control liveness across the node's silence: if the node went quiet AND the control's rate held
    # up, the silence is real.
    if tail_silence >= GAP_S:
        quiet_from = ts[-1]
        ctrl_before = rate(peer_t, t0, quiet_from)
        ctrl_during = rate(peer_t, quiet_from, t_end)
        print("control rate: %.1f/s before %s went quiet, %.1f/s during its silence"
              % (ctrl_before, label, ctrl_during))
        if ctrl_before > 0 and ctrl_during < CONTROL_MIN_FRAC * ctrl_before:
            return (2, "INCONCLUSIVE|%s: the control's beacons fell off too (%.1f/s -> %.1f/s) during "
                       "the silence, so the capture lost the channel rather than the node losing the "
                       "link. Re-run; do not read this as a recovery failure."
                       % (label, ctrl_before, ctrl_during))

        return (1, "FAIL|%s: the node's beacons under its %s stopped at +%.1fs and NEVER returned "
                   "(%.1fs of silence to the end of the capture) while the control kept beaconing at "
                   "%.1f/s. The chip restarted and that vif was not brought back: nothing re-issued "
                   "its chip configuration (BSS, BSSID, beaconing), so the vif can be alive with "
                   "nothing on air. Look for a 'hw-restart restore ABORTED' line on the console."
                   % (label, what, quiet_from - t0, tail_silence, ctrl_during))

    # Beacons are still flowing at the end of the capture, and the baseline was real -- that is the
    # recovery, and it is what PASS means here.
    #
    # A gap is NOT required of THIS SA. An earlier version demanded one and called a gapless capture
    # INCONCLUSIVE, reasoning that the trigger must have fallen outside the window. That is backwards:
    # the better the recovery, the shorter the outage, and once it drops below GAP_S a working node was
    # being reported as unmeasurable. What is required is evidence that a restart happened at all --
    # see restart_observed, which reads every scored SA rather than just this one.
    if gaps:
        # The LARGEST gap, not the last one. The restart's teardown (~3 s measured) dominates anything
        # RF loss produces, whereas the last gap may be a late blip: with GAP_S at 2.0 s a 2.1 s
        # dropout near the end of the window would leave only a handful of beacons after it and score a
        # fully recovered node as FAIL. Picking the largest keys the verdict to the event under test.
        biggest = max(gaps, key=lambda g: g[1] - g[0])
        outage = biggest[1] - biggest[0]
        n_after = sum(1 for t in ts if t >= biggest[1])
        if n_after < MIN_RECOVERY:
            return (1, "FAIL|%s: only %d beacon(s) after its longest interruption (%.1fs; need >=%d). "
                       "It is not back on air in any usable sense." % (label, n_after, outage,
                                                                       MIN_RECOVERY))
        detail = ("its longest interruption was %.1fs, after which it emitted %d more beacons"
                  % (outage, n_after))
    elif not restart_observed:
        # No gap on ANY scored SA: there is no evidence a restart happened, so this cannot be a
        # recovery PASS.
        #
        # An earlier revision did award PASS here, reasoning that a recovery too fast to drop GAP_S
        # worth of beacons should not be penalised. That was wrong, and reachable by design rather than
        # only by operator error: every INCONCLUSIVE early-exit in the fixture park_forever()s BEFORE
        # mmwlan_force_hw_restart() -- so a run that bailed because the rig was wrong produces a
        # capture of a node that was simply never restarted, and it would have scored PASS. The
        # measured teardown is ~3 s against a 2.0 s threshold, so a real restart always leaves a gap;
        # its absence everywhere means the capture does not contain the trigger.
        return (2, "INCONCLUSIVE|%s: the node beaconed for the whole capture with no interruption of "
                   "%.1fs or more on any scored address (%d beacons over %.0fs). A real teardown "
                   "measures ~3s, so this capture almost certainly does not contain the restart -- "
                   "either it started too late, or the fixture bailed before triggering one (check its "
                   "data-before/peer-before steps). NOT a recovery PASS: there is no evidence here "
                   "that anything was restarted." % (label, GAP_S, len(ts), span))
    else:
        # Another SA saw the outage, this one did not drop GAP_S worth of beacons. The restart is
        # proven; this vif simply never went measurably quiet. Reported, not penalised.
        detail = ("it never dropped %.1fs of beacons, though the restart is evident on the other "
                  "scored address" % GAP_S)

    return (0, "PASS|%s: beaconing under its %s at the end of the capture (%d beacons over %.0fs, last "
               "one %.1fs before the end) and %s." % (label, what, len(ts), span, t_end - ts[-1],
                                                      detail))


results = [score("mesh", nut_t, "mesh SA")]
if AP is not None:
    results.append(score("ap", ap_t, "AP BSSID"))

for code, msg in results:
    print("RESULT|" + msg)

# INCONCLUSIVE dominates FAIL: the commonest way to get one is "no evidence a restart happened", and a
# capture that does not contain the trigger cannot condemn anything in it. A FAIL is only meaningful
# once every scored address agrees the run was measurable.
if any(c == 2 for c, _ in results):
    print("RESULT|INCONCLUSIVE|at least one scored address was unmeasurable (above); the run does not "
          "support a verdict either way.")
    sys.exit(2)
if any(c == 1 for c, _ in results):
    print("RESULT|FAIL|at least one vif did not come back after the restart (above).")
    sys.exit(1)
print("RESULT|PASS|every scored vif came back after the restart. Cross-check the fixture's own "
      "data-before/data-after steps: beaconing alone does not prove the datapath came back.")
sys.exit(0)
