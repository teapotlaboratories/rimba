#!/usr/bin/env python3
# mesh_beacon_cap.py — capture mesh beacons on morse0, dump the Mesh Configuration IE (id 113) per SA.
# Verifies the S2 beacon bits: Formation Info (bit0 = Connected-to-Gate, bits1-6 = peer count) and
# Capability (bit0 ACCEPT_PLINKS, bit3 FORWARDING).  sudo python3 mesh_beacon_cap.py [dur] [want_sa]
import socket, struct, sys, time
DUR = int(sys.argv[1]) if len(sys.argv) > 1 else 20
WANT = sys.argv[2].lower() if len(sys.argv) > 2 else None
MESH_CONFIG_EID = 0x71  # 113
MESH_ID_EID = 0x72      # 114

def mac(b): return ":".join("%02x" % x for x in b)

def scan_ies(ies, want):
    i = 0
    out = {}
    while i + 2 <= len(ies):
        eid, ln = ies[i], ies[i+1]
        if i + 2 + ln > len(ies): break
        out.setdefault(eid, ies[i+2:i+2+ln])
        i += 2 + ln
    return out

s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.ntohs(0x0003))
s.bind(("morse0", 0)); s.settimeout(1)
seen = {}
t0 = time.time()
while time.time() - t0 < DUR:
    try: d = s.recv(4096)
    except socket.timeout: continue
    if len(d) < 4: continue
    rtlen = struct.unpack_from('<H', d, 2)[0]
    f = d[rtlen:]
    if len(f) < 16: continue
    fc = f[0]; ftype = (fc >> 2) & 3; fsub = (fc >> 4) & 0xf
    if ftype == 3 and fsub == 1:      # S1G ext beacon: 15-byte hdr, SA at [4:10]
        sa = mac(f[4:10]); ies = f[15:]
    elif ftype == 0 and fsub == 8:    # legacy beacon: SA at [10:16], fixed 12 then IEs
        sa = mac(f[10:16]); ies = f[24+12:]
    else:
        continue
    if WANT and sa != WANT: continue
    d_ies = scan_ies(ies, None)
    if MESH_CONFIG_EID not in d_ies: continue
    cfg = d_ies[MESH_CONFIG_EID]
    if sa in seen: continue
    seen[sa] = cfg
    mid = d_ies.get(MESH_ID_EID, b'')
    if len(cfg) >= 7:
        psel, pmetric, cong, synch, auth, form, cap = cfg[0], cfg[1], cfg[2], cfg[3], cfg[4], cfg[5], cfg[6]
        print("BEACON sa=%s mesh_id=%r" % (sa, mid.decode('latin1', 'replace')))
        print("  MeshConfig hex=%s" % cfg.hex())
        print("  formation_info=0x%02x -> connected_to_gate(bit0)=%d  peer_count(bits1-6)=%d  as(bit7)=%d"
              % (form, form & 1, (form >> 1) & 0x3f, (form >> 7) & 1))
        print("  capability=0x%02x -> accept_plinks(0x01)=%d  forwarding(0x08)=%d  ps(0x40)=%d"
              % (cap, bool(cap & 0x01), bool(cap & 0x08), bool(cap & 0x40)))
s.close()
if not seen:
    print("no mesh-config beacon captured" + (" for %s" % WANT if WANT else ""))
