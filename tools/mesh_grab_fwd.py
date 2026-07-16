#!/usr/bin/env python3
# Capture the full 802.11 MPDU (after radiotap) of a mesh RELAY's forwarded frames, for offline
# CCMP analysis. Run on the sniffer (chronium) with wlan1 in monitor + morse0 up on ch27.
#   sudo python3 mesh_grab_fwd.py <seconds>
# Edit RELAY_TA / ORIGIN_SA below to your relay (TA=A2) + origin (mesh SA=A3 for a 3-addr group
# forward, or A4 for a 4-addr unicast forward). Prints one hex line per matching MPDU (incl. FCS).
import socket, sys, time, struct
dur = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0
RELAY_TA = 'e272a1f8f008'   # relay's mesh MAC (A2 / TA)
ORIGIN_SA = 'e272a1f8f940'  # origin's mesh MAC (A3 for group-forward; A4 for 4-addr unicast)
s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x0003))
s.bind(('morse0', 0)); s.settimeout(0.5)
end = time.time() + dur; n = 0
while time.time() < end and n < 5:
    try: pkt = s.recv(4096)
    except socket.timeout: continue
    if len(pkt) < 4: continue
    rtlen = struct.unpack_from('<H', pkt, 2)[0]; f = pkt[rtlen:]
    if len(f) < 34: continue
    if ((f[0] >> 2) & 3) != 2: continue          # data frames only
    a3 = f[16:22].hex(); a4 = f[24:30].hex() if len(f) >= 30 else ''
    if f[10:16].hex() == RELAY_TA and (a3 == ORIGIN_SA or a4 == ORIGIN_SA):
        prot = (f[1] >> 6) & 1
        print('FRAME len=%d prot=%d fc=%02x%02x A1=%s' % (len(f), prot, f[0], f[1], f[4:10].hex()))
        print(f.hex()); n += 1
print('captured', n)
