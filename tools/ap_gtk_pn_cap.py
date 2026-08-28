#!/usr/bin/env python3
# ap_gtk_pn_cap.py — did the AP's GROUP key survive a hardware restart? Scored on the air.
#
#   sudo python3 ap_gtk_pn_cap.py <ap_bssid> [dur_s] [--control <mac>]
#
# Run on chronium (morse0 monitor, ch27 — see docs/reference/rimba-linux-halow-monitor.md), STARTED
# BEFORE the gate under test is reset. Pair it with `test-mesh-ap-gate HW_RESTART=1`, which emits a
# 5 Hz broadcast train on its AP subnet for exactly this purpose and prints its AP BSSID at boot.
#
# WHAT IT SCORES, AND WHY IT IS THE ONLY PROBE THAT WORKS HERE.
# umac_ap_handle_hw_restarted() re-pushes the AP's keychain after a chip restart. The failure mode it
# has to avoid is the one S2 found on the mesh side: connection_keys_install_key() COPIES tx_seq out of
# the struct it is handed (connection_keys.c:118-121), so a REBUILT key restarts the CCMP packet number
# at zero while every associated station keeps its replay window. Every frame is then silently dropped
# as a replay, every install returns success, and nothing is logged. Host-side it is invisible.
#
# On the air it is not: the PN of the AP's group-addressed frames is right there in the CCMP header. So
#     PN keeps climbing across the outage  =>  the surviving keychain was re-pushed   (PASS)
#     PN rewinds to a low value            =>  the key was rebuilt — the S2 defect    (FAIL)
#
# The unicast half of the same question CANNOT be scored this way, and that is not an oversight: a
# client tears its association down ~1.1 s into the outage and re-associates ~22 s later (measured
# 2026-08-26), so its pairwise key is renegotiated before anything can be observed crossing the
# restored one. The group key has no such problem — the AP owns it alone.
#
# HOW THE RESTART IS IDENTIFIED — by a marker, never by the shape of the trace. The fixture switches
# its broadcast payload to a LONGER one immediately before triggering, so the frames split into two
# on-air lengths with the event at the boundary. Length, because the frames are GTK-encrypted: the
# sniffer can see how big they are and nothing else.
#
# The first version looked for the largest gap instead, and the bench refuted it on the first run: the
# board's own REBOOT leaves a bigger hole (3.33 s measured) than the chip restart does (~1 s), and a
# reboot legitimately installs a fresh GTK at PN 1 — so a textbook PASS was scored FAIL. The lesson is
# the same one the mesh scorer learned twice: a verdict keyed on "the biggest thing in the window" is
# keyed on whatever else happened to be in the window.
#
# THE CAPTURE CARRIES ITS OWN POSITIVE CONTROLS. A GTK is legitimately reinstalled twice in a normal
# run — once when the AP boots, and once when the first client associates — and each shows up here as a
# PN reset to 1. They are reported, not hidden: they are what proves this instrument can SEE a rewind.
# A run with no reset anywhere before the restart is a run whose negative result means less, and the
# tool says so.

import socket
import struct
import sys
import time

argv = sys.argv[1:]
CONTROL = None
if "--control" in argv:
    i = argv.index("--control")
    if i + 1 >= len(argv):
        sys.exit("--control needs a MAC (a node that beacons throughout, to prove the monitor held "
                 "the channel)")
    CONTROL = argv[i + 1].lower()
    del argv[i:i + 2]

if not argv:
    sys.exit(__doc__ or "usage: ap_gtk_pn_cap.py <ap_bssid> [dur_s] [--control <mac>]")

AP = argv[0].lower()
DUR = int(argv[1]) if len(argv) > 1 else 150

# A gap shorter than this is jitter or RF loss, not a chip restart. The train runs at 5 Hz and the
# measured teardown+reload is ~1-3 s at the AP vif, so 0.8 s is comfortably between the two.
GAP_S = 0.8
# Frames needed either side of the outage for the comparison to mean anything.
MIN_EACH_SIDE = 15


def mac(b):
    return ":".join("%02x" % x for x in b)


def group_ccmp_pn(d, ap):
    """(pn, key_id, frame_len) for a protected, group-addressed data frame sent by `ap`, else None.

    Layout confirmed against a real capture (2026-08-26): the MM6108 puts QoS data on air in the
    ordinary 802.11 form — FC, duration, A1, A2, A3, seq-ctl, then 2 bytes of QoS control — so the
    CCMP header starts at 26 for subtype 8. PN is split either side of the key-id octet, little-endian
    in each half: PN0,PN1 then PN2..PN5.
    """
    if len(d) < 4:
        return None
    rtlen = struct.unpack_from("<H", d, 2)[0]
    f = d[rtlen:]
    if len(f) < 34:
        return None
    if ((f[0] >> 2) & 3) != 2:          # data frames only
        return None
    if not (f[1] & 0x40):               # Protected
        return None
    if not (f[4] & 1):                  # group-addressed (A1 has the I/G bit set)
        return None
    if mac(f[10:16]) != ap:             # transmitted by the AP under test
        return None
    hdr = 24 + (2 if ((f[0] >> 4) & 0x8) else 0)
    c = f[hdr:hdr + 8]
    if len(c) < 8 or not (c[3] & 0x20):  # ExtIV — no ExtIV means no CCMP PN to read
        return None
    pn = c[0] | c[1] << 8 | c[4] << 16 | c[5] << 24 | c[6] << 32 | c[7] << 40
    return pn, c[3] >> 6, len(f)


def s1g_beacon_sa(d):
    """SA of an S1G (or legacy) beacon, for the liveness control. Same parse as mesh_hwrestart_cap.py."""
    if len(d) < 4:
        return None
    rtlen = struct.unpack_from("<H", d, 2)[0]
    f = d[rtlen:]
    if len(f) < 16:
        return None
    ftype, fsub = (f[0] >> 2) & 3, (f[0] >> 4) & 0xF
    if ftype == 3 and fsub == 1:
        return mac(f[4:10])
    if ftype == 0 and fsub == 8:
        return mac(f[10:16])
    return None


s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.ntohs(0x0003))
s.bind(("morse0", 0))
s.settimeout(1)

print("watching morse0: ap=%s%s for %ds" % (AP, (" control=%s" % CONTROL) if CONTROL else "", DUR),
      flush=True)

pts = []        # (t, pn, key_id, frame_len) for the AP's group-addressed frames
ctrl_t = []
t0 = time.time()
last_report = t0
while time.time() - t0 < DUR:
    try:
        d = s.recv(4096)
    except socket.timeout:
        d = None
    now = time.time()
    if d:
        got = group_ccmp_pn(d, AP)
        if got is not None:
            pts.append((now, got[0], got[1], got[2]))
        elif CONTROL is not None and s1g_beacon_sa(d) == CONTROL:
            ctrl_t.append(now)
    if now - last_report >= 10:
        last_report = now
        print("  +%5.0fs  group_frames=%d  last_pn=%s%s"
              % (now - t0, len(pts), pts[-1][1] if pts else "-",
                 ("  control_beacons=%d" % len(ctrl_t)) if CONTROL else ""), flush=True)
s.close()

span = time.time() - t0
print("\ncaptured %.0fs: %d group-addressed frames from %s%s"
      % (span, len(pts), AP, (", %d control beacons" % len(ctrl_t)) if CONTROL else ""))

if CONTROL is not None and not ctrl_t:
    print("RESULT|INCONCLUSIVE|the control node emitted no beacons, so the monitor never had the "
          "channel (wrong freq, wlan1 not in monitor type, or the control board is down). Nothing "
          "about the AP can be concluded from this capture.")
    sys.exit(2)

if len(pts) < 2 * MIN_EACH_SIDE:
    print("RESULT|INCONCLUSIVE|only %d group-addressed frames from the AP (need >=%d). Either the "
          "fixture's broadcast train is not running (check for TEST|INFO|bcast on its console), the "
          "BSSID is wrong, or the AP never came up. Note the AP BSSID is NOT the device MAC: it is the "
          "device MAC with bit 1 of octet 0 flipped, and the fixture prints it."
          % (len(pts), 2 * MIN_EACH_SIDE))
    sys.exit(2)

# --- every PN reset in the window, labelled ---------------------------------------------------------
resets = [(pts[i + 1][0] - t0, pts[i][1], pts[i + 1][1])
          for i in range(len(pts) - 1) if pts[i + 1][1] <= pts[i][1]]
print("PN ran %d -> %d over the window; %d reset(s) seen:" % (pts[0][1], pts[-1][1], len(resets)))
for at, a, b in resets:
    print("  +%6.2fs  PN %d -> %d" % (at, a, b))
if not resets:
    print("  (none — see the note below on what that costs this run)")

# --- find the marker: two dominant frame lengths, one strictly after the other ----------------------
buckets = {}
for t, pn, k, ln in pts:
    buckets.setdefault(ln, []).append((t, pn, k))
big = {ln: v for ln, v in buckets.items() if len(v) >= MIN_EACH_SIDE}
print("\nframe lengths seen (>=%d frames): %s"
      % (MIN_EACH_SIDE, ", ".join("%dB x%d" % (ln, len(v)) for ln, v in sorted(big.items()))))

if len(big) != 2:
    print("RESULT|INCONCLUSIVE|expected exactly two payload lengths in the group train (the fixture "
          "switches to a longer one at the trigger, and that boundary IS the restart); found %d with "
          ">=%d frames. Either the capture does not straddle the trigger, or the fixture predates the "
          "marker — check its console for 'bcast|switching the group train'." % (len(big), MIN_EACH_SIDE))
    sys.exit(2)

(ln_a, va), (ln_b, vb) = sorted(big.items(), key=lambda kv: kv[1][0][0])
before, after = va, vb
if before[-1][0] >= after[0][0]:
    print("RESULT|INCONCLUSIVE|the two payload lengths interleave in time (%dB runs to +%.2fs, %dB "
          "starts at +%.2fs), so there is no clean marker boundary to measure across. Some other "
          "broadcast traffic is being counted as the train; re-run."
          % (ln_a, before[-1][0] - t0, ln_b, after[0][0] - t0))
    sys.exit(2)

outage = after[0][0] - before[-1][0]
pn_before, key_before = before[-1][1], before[-1][2]
pn_after, key_after = after[0][1], after[0][2]

print("marker boundary: last %dB frame at +%.2fs, first %dB frame at +%.2fs  ->  outage %.2fs"
      % (ln_a, before[-1][0] - t0, ln_b, after[0][0] - t0, outage))
print("PN across it: %d -> %d  (delta %+d)   key id %d -> %d"
      % (pn_before, pn_after, pn_after - pn_before, key_before, key_after))

if outage < GAP_S:
    print("RESULT|INCONCLUSIVE|the train barely paused at the marker (%.2fs, need >=%.1fs), so the "
          "capture does not appear to contain the teardown at all — the trigger may have been refused. "
          "Cross-check the fixture's restart-ran step. NOT a pass: there is no outage here to have "
          "survived." % (outage, GAP_S))
    sys.exit(2)

if CONTROL is not None:
    dur_before = max(before[-1][0] - t0, 1e-6)
    dur_after = max(time.time() - after[0][0], 1e-6)
    r_before = sum(1 for t in ctrl_t if t <= before[-1][0]) / dur_before
    r_after = sum(1 for t in ctrl_t if t >= after[0][0]) / dur_after
    print("control beacon rate: %.1f/s before the outage, %.1f/s after" % (r_before, r_after))
    if r_before > 0 and r_after < 0.25 * r_before:
        print("RESULT|INCONCLUSIVE|the control's beacons fell off across the outage (%.1f/s -> %.1f/s), "
              "so the capture lost the channel rather than the AP losing the air. Re-run."
              % (r_before, r_after))
        sys.exit(2)

if pn_after <= pn_before:
    print("RESULT|FAIL|the AP's group key was REBUILT across the restart, not re-pushed: the CCMP "
          "packet number rewound from %d to %d. Every associated station keeps its replay window over "
          "a chip restart, so it will discard every group-addressed frame the AP sends from here on — "
          "silently, with every key install returning success. This is the defect documented at "
          "connection_keys.c:118-121; the fix is umac_keys_reinstall_keys() on the AP's common station, "
          "never a freshly built key." % (pn_before, pn_after))
    sys.exit(1)

if key_after != key_before:
    print("RESULT|INCONCLUSIVE|the PN advanced (%d -> %d) but the key ID changed (%d -> %d), so this is "
          "a different key and the comparison does not test what it is meant to. A rekey landed inside "
          "the outage window; re-run." % (pn_before, pn_after, key_before, key_after))
    sys.exit(2)

control_note = ("The same capture contains %d legitimate PN reset(s) (an AP boot and the first client's "
                "GTK install both reinstall the key), which is the positive control: this measurement "
                "demonstrably CAN see a rewind." % len(resets)) if resets else \
               ("⚠ No PN reset appears anywhere in this window, so the capture carries no positive "
                "control — nothing here demonstrates the measurement could have seen a rewind. Prefer a "
                "run that starts before the gate boots, which always contains one.")

print("RESULT|PASS|the AP's group key SURVIVED the restart: across a %.2fs outage the CCMP packet "
      "number advanced %d -> %d on key id %d, rather than rewinding. The keychain was re-pushed, not "
      "rebuilt, so associated stations' replay windows stay valid. %s"
      % (outage, pn_before, pn_after, key_after, control_note))
sys.exit(0)
