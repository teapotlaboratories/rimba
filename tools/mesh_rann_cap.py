#!/usr/bin/env python3
# mesh_rann_cap.py — capture 802.11s RANN (Root Announcement) HWMP frames off chronium's morse0.
#
# Runs ON chronium (which is in monitor mode on ch27). Reads morse0 raw radiotap frames, filters to
# mesh HWMP path-selection action frames carrying a RANN element (WLAN_EID_RANN=126), and dumps the
# full hex + a decoded field breakdown, grouped by source MAC (A2). Used to byte-diff an ESP gate's
# RANN against a live Linux gate's RANN (S1 mesh-gate verification).
#
#   sudo python3 mesh_rann_cap.py [duration_s] [out_dir]
#
# RANN element on the wire (after the 24-byte 802.11 mgmt header):
#   [24] category=0x0d (WLAN_CATEGORY_MESH_ACTION)
#   [25] action  =0x01 (WLAN_MESH_ACTION_HWMP_PATH_SELECTION)
#   [26] eid     =0x7e (126, WLAN_EID_RANN)
#   [27] len     =0x15 (21)
#   [28] flags | [29] hopcount | [30] ttl | [31..36] rann_addr(6)
#   [37..40] rann_seq(le32) | [41..44] rann_interval(le32) | [45..48] rann_metric(le32)
import socket, struct, sys, time, os

DUR = int(sys.argv[1]) if len(sys.argv) > 1 else 40
OUT = sys.argv[2] if len(sys.argv) > 2 else "/tmp/rann_cap"
os.makedirs(OUT, exist_ok=True)

def mac(b):
    return ":".join("%02x" % x for x in b)

def le32(b):
    return struct.unpack_from("<I", b, 0)[0]

s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.ntohs(0x0003))
s.bind(("morse0", 0))
s.settimeout(1)

seen = {}   # sa -> count
first = {}  # sa -> (full_hex, action_hex, decoded)
t0 = time.time()
total = 0
print("cap start dur=%ds out=%s" % (DUR, OUT), flush=True)
while time.time() - t0 < DUR:
    try:
        d = s.recv(4096)
    except socket.timeout:
        continue
    total += 1
    if len(d) < 4:
        continue
    rtlen = struct.unpack_from("<H", d, 2)[0]      # radiotap length
    f = d[rtlen:]
    if len(f) < 49:
        continue
    fc = f[0]
    ftype = (fc >> 2) & 3
    fsub = (fc >> 4) & 0xf
    if ftype != 0 or fsub != 13:                    # mgmt / action only
        continue
    if f[24] != 0x0d or f[25] != 0x01:              # mesh action / HWMP path selection
        continue
    if f[26] != 0x7e:                               # RANN element id
        continue
    sa = mac(f[10:16])
    seen[sa] = seen.get(sa, 0) + 1
    flags, hop, ttl = f[28], f[29], f[30]
    raddr = mac(f[31:37])
    seq = le32(f[37:41])
    interval = le32(f[41:45])
    metric = le32(f[45:49])
    action_hex = f[24:49].hex()                     # category..metric (the comparable RANN body)
    full_hex = f.hex()
    dec = ("flags=0x%02x hop=%d ttl=%d rann_addr=%s seq=%d interval=%d metric=%d"
           % (flags, hop, ttl, raddr, seq, interval, metric))
    print("RANN sa=%s %s" % (sa, dec), flush=True)
    print("     action_hex=%s" % action_hex, flush=True)
    if sa not in first:
        first[sa] = (full_hex, action_hex, dec)
        # persist the first frame per SA for offline byte-diff
        with open(os.path.join(OUT, "rann_%s.txt" % sa.replace(":", "")), "w") as fp:
            fp.write("sa=%s\n%s\naction_hex=%s\nfull_hex=%s\n" % (sa, dec, action_hex, full_hex))
s.close()
print("---- summary ----", flush=True)
print("total frames seen on morse0: %d" % total, flush=True)
for sa, n in sorted(seen.items()):
    print("RANN emitter %s: %d frames  first: %s" % (sa, n, first[sa][2]), flush=True)
if not seen:
    print("NO RANN captured (check: emitter up? monitor tuned to ch27/5560? morse0 up?)", flush=True)
