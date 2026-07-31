#!/usr/bin/env python3
# raw_s1g_beacon_decode.py — decode S1G beacons out of a morse0 radiotap pcap and dump their IE chain.
#
# Written for the RAW (802.11ah Restricted Access Window) AP-side port: the S0b stages need to answer
# "did the RPS element (EID 208) reach the air, byte-identical to the Linux reference?" — which needs a
# radiotap-aware pcap decoder. Every other capture script in this tree reads a live AF_PACKET socket and
# SKIPS the radiotap header (mesh_beacon_cap.py:31-32, mesh_rann_cap.py:47-48, mesh_grab_fwd.py:18), so
# none of them can carry radiotap.mactime or produce a durable artifact.
#
#   sudo tcpdump -i morse0 -w cap.pcap        # on chronium (the bench's only sniffer)
#   python3 tools/raw_s1g_beacon_decode.py cap.pcap [--sa <mac>] [--beacon-int-tu 100]
#
# Two decode hazards this handles explicitly, both of which have manufactured a false result before:
#
#  1. THE TRAILING FCS. morse0 frames carry a 4-byte FCS. A brute-forced IE start offset that requires
#     the chain to land on the frame end matched only 3 of 242 frames and reported VARYING offsets
#     (10/11/12/13) that looked exactly like the conditional S1G presence bits — i.e. like the firmware
#     having rewritten the beacon. Both were artefacts of the unaccounted FCS.
#  2. THE CONDITIONAL S1G HEADER LENGTH. The S1G beacon header is 15 bytes
#     (frame_control 2 + duration 2 + SA 6 + timestamp 4 + change_seq 1), but Frame Control can add
#     three optional fixed fields ahead of the IEs — Next TBTT +3 (0x0100), Compressed SSID +4 (0x0200),
#     ANO +1 (0x0400): morse_driver/dot11ah/ie.c:442-457 `morse_dot11_find_s1g_beacon_ies()` and the
#     sizes at dot11ah/dot11ah.h:16-18,24-26. A wrong offset fabricates a false "the FW stripped our IE".
#
# So the offset is COMPUTED from Frame Control, never searched, and the observed FC is reported as
# evidence with every capture. The IE walk must then consume the frame exactly; anything else is
# reported as an undecodable frame rather than silently skipped.
import argparse
import struct
import sys
from collections import Counter

RADIOTAP_LINKTYPE = 127

# 802.11ah S1G Beacon = Extension frame, type 3 / subtype 1.
S1G_BEACON_FC0 = 0x1C

# Conditional fixed fields ahead of the IEs (morse_driver/dot11ah/dot11ah.h:16-18, 24-26).
FC_NEXT_TBTT, FC_COMPRESS_SSID, FC_ANO = 0x0100, 0x0200, 0x0400
SZ_NEXT_TBTT, SZ_COMPRESS_SSID, SZ_ANO = 3, 4, 1
S1G_BEACON_HDR_LEN = 15

FCS_LEN = 4

# morselib dot11/dot11.h:2334-2349 plus the RAW element the port is about.
EID_NAMES = {
    0: "SSID", 5: "TIM", 56: "TIE", 60: "ECSA", 76: "MMIC",
    208: "RPS", 210: "AID_REQUEST", 211: "AID_RESPONSE",
    213: "S1G_BEACON_COMPATIBILITY", 214: "SHORT_BCN_INT", 216: "TWT",
    217: "S1G_CAPABILITIES", 221: "VENDOR_SPECIFIC", 225: "REACHABLE_ADDRESS",
    232: "S1G_OPERATION",
}
EID_RPS = 208
EID_TIM = 5
EID_S1G_CAPABILITIES = 217

# The golden constant (design doc §7.1), validated on air in S0b-2 against the Linux reference AP.
GOLDEN_RPS = bytes((0xD0, 0x06, 0x20, 0x40, 0x09, 0x04, 0xE0, 0x1F))


def mac(b):
    return ":".join("%02x" % x for x in b)


# ---------------------------------------------------------------- pcap + radiotap

def read_pcap(path):
    """Yield (ts_us, packet_bytes) from a classic pcap. Returns the link type first."""
    with open(path, "rb") as fh:
        blob = fh.read()
    if len(blob) < 24:
        sys.exit("%s: too short to be a pcap" % path)
    magic = blob[:4]
    if magic == b"\xd4\xc3\xb2\xa1":
        endian, ts_div = "<", 1_000
    elif magic == b"\xa1\xb2\xc3\xd4":
        endian, ts_div = ">", 1_000
    elif magic == b"\x4d\x3c\xb2\xa1":      # nanosecond-resolution variant
        endian, ts_div = "<", 1_000_000
    elif magic == b"\xa1\xb2\x3c\x4d":
        endian, ts_div = ">", 1_000_000
    else:
        sys.exit("%s: not a classic pcap (magic %s) — pcapng is not supported" % (path, magic.hex()))
    linktype = struct.unpack_from(endian + "I", blob, 20)[0]
    pkts = []
    off = 24
    while off + 16 <= len(blob):
        ts_sec, ts_frac, caplen, origlen = struct.unpack_from(endian + "IIII", blob, off)
        off += 16
        if off + caplen > len(blob):
            break
        pkts.append((ts_sec * 1_000_000 + ts_frac // ts_div, origlen, blob[off:off + caplen]))
        off += caplen
    return linktype, pkts


# Radiotap field sizes/alignments, in bit order (only the prefix we can hit on morse0 is needed;
# the walk stops at the first bit it does not know, which is safe because fields are ordered).
RT_FIELDS = [
    (0, 8, 8),    # TSFT              u64
    (1, 1, 1),    # Flags             u8
    (2, 1, 1),    # Rate              u8
    (3, 2, 4),    # Channel           u16 freq + u16 flags
    (4, 1, 2),    # FHSS              u8 + u8
    (5, 1, 1),    # dBm antsignal     s8
    (6, 1, 1),    # dBm antnoise      s8
    (7, 2, 2),    # Lock quality      u16
    (8, 2, 2),    # TX attenuation    u16
    (9, 2, 2),    # dB TX attenuation u16
    (10, 1, 1),   # dBm TX power      s8
    (11, 1, 1),   # Antenna           u8
    (12, 1, 1),   # dB antsignal      u8
    (13, 1, 1),   # dB antnoise       u8
    (14, 2, 2),   # RX flags          u16
]

RT_FLAG_FCS_AT_END = 0x10


def parse_radiotap(pkt):
    """-> (header_len, {'tsft':.., 'flags':.., 'rssi':..}) or (None, {}) if malformed."""
    if len(pkt) < 8:
        return None, {}
    version, _pad, rtlen = struct.unpack_from("<BBH", pkt, 0)
    if version != 0 or rtlen < 8 or rtlen > len(pkt):
        return None, {}
    # The present bitmap is a chain: bit 31 set means another u32 follows.
    presents, off = [], 4
    while off + 4 <= rtlen:
        word = struct.unpack_from("<I", pkt, off)[0]
        presents.append(word)
        off += 4
        if not (word & 0x80000000):
            break
    out = {}
    p0 = presents[0] if presents else 0
    for bit, align, size in RT_FIELDS:
        if not (p0 & (1 << bit)):
            continue
        off = (off + align - 1) // align * align
        if off + size > rtlen:
            break
        if bit == 0:
            out["tsft"] = struct.unpack_from("<Q", pkt, off)[0]
        elif bit == 1:
            out["flags"] = pkt[off]
        elif bit == 5:
            out["rssi"] = struct.unpack_from("<b", pkt, off)[0]
        off += size
    return rtlen, out


# ---------------------------------------------------------------- 802.11 / S1G

def s1g_ie_offset(fc):
    """Header length ahead of the IEs, computed from Frame Control — never searched."""
    off = S1G_BEACON_HDR_LEN
    if fc & FC_NEXT_TBTT:
        off += SZ_NEXT_TBTT
    if fc & FC_COMPRESS_SSID:
        off += SZ_COMPRESS_SSID
    if fc & FC_ANO:
        off += SZ_ANO
    return off


def walk_ies(ies):
    """-> (list of (eid, payload), ok) where ok means the walk consumed `ies` exactly."""
    out, i = [], 0
    while i + 2 <= len(ies):
        eid, ln = ies[i], ies[i + 1]
        if i + 2 + ln > len(ies):
            return out, False
        out.append((eid, ies[i + 2:i + 2 + ln]))
        i += 2 + ln
    return out, i == len(ies)


def main():
    ap = argparse.ArgumentParser(description="Decode S1G beacon IE chains from a morse0 radiotap pcap")
    ap.add_argument("pcap")
    ap.add_argument("--sa", help="only decode beacons from this source MAC")
    ap.add_argument("--beacon-int-tu", type=int, default=100,
                    help="beacon interval in TU, for frame-count reconciliation (default 100)")
    ap.add_argument("--show", type=int, default=2, help="dump this many full frames as hex (default 2)")
    args = ap.parse_args()

    linktype, pkts = read_pcap(args.pcap)
    print("pcap            : %s" % args.pcap)
    print("linktype        : %d%s" % (linktype, "" if linktype == RADIOTAP_LINKTYPE else " (EXPECTED 127 radiotap)"))
    print("packets         : %d" % len(pkts))
    if not pkts:
        sys.exit("no packets")

    want_sa = args.sa.lower() if args.sa else None
    chains = Counter()
    rps_vals = Counter()
    caps6 = Counter()
    fc_vals = Counter()
    offsets = Counter()
    per_sa = Counter()
    dtim_split = Counter()
    rssi = []
    tsfts = []
    undecodable = []
    shown = 0
    n_s1g = 0

    for ts_us, origlen, pkt in pkts:
        rtlen, rt = parse_radiotap(pkt)
        if rtlen is None:
            undecodable.append((ts_us, "radiotap malformed"))
            continue
        f = pkt[rtlen:]
        if len(f) < S1G_BEACON_HDR_LEN or f[0] != S1G_BEACON_FC0:
            continue
        n_s1g += 1
        sa = mac(f[4:10])
        per_sa[sa] += 1
        if want_sa and sa != want_sa:
            continue

        # The FCS is present unless radiotap Flags explicitly says otherwise; morse0 carries it.
        body = f[:-FCS_LEN] if (rt.get("flags", RT_FLAG_FCS_AT_END) & RT_FLAG_FCS_AT_END) else f

        fc = struct.unpack_from("<H", f, 0)[0]
        off = s1g_ie_offset(fc)
        fc_vals[fc] += 1
        offsets[off] += 1
        ies, ok = walk_ies(body[off:])
        if not ok:
            undecodable.append((ts_us, "IE walk did not land on frame end (fc=0x%04x off=%d len=%d)"
                                % (fc, off, len(body))))
            continue

        if "rssi" in rt:
            rssi.append(rt["rssi"])
        if "tsft" in rt:
            tsfts.append(rt["tsft"])

        chains[tuple(e for e, _ in ies)] += 1
        for eid, payload in ies:
            if eid == EID_RPS:
                rps_vals[bytes((EID_RPS, len(payload))) + payload] += 1
            elif eid == EID_S1G_CAPABILITIES and len(payload) > 6:
                caps6[payload[6]] += 1
            elif eid == EID_TIM and len(payload) >= 2:
                # TIM: DTIM Count, DTIM Period, ... — count==0 marks a DTIM beacon.
                dtim_split["DTIM" if payload[0] == 0 else "non-DTIM"] += 1

        if shown < args.show:
            shown += 1
            print("\n--- frame @ %d us, sa=%s, rtlen=%d, fc=0x%04x, ie_offset=%d ---" % (ts_us, sa, rtlen, fc, off))
            print("  radiotap : %s" % {k: (hex(v) if k == "flags" else v) for k, v in rt.items()})
            print("  frame    : %s" % f.hex())
            for eid, payload in ies:
                print("    %3d %-26s len=%-3d %s"
                      % (eid, EID_NAMES.get(eid, "?"), len(payload), payload.hex()))

    print("\n================ summary ================")
    print("S1G beacons seen        : %d" % n_s1g)
    print("per source MAC          : %s" % dict(per_sa))
    if want_sa:
        print("filtered to sa          : %s" % want_sa)
    print("undecodable             : %d" % len(undecodable))
    for ts, why in undecodable[:5]:
        print("   @%d %s" % (ts, why))

    print("frame control observed  : %s" % {("0x%04x" % k): v for k, v in fc_vals.items()})
    print("  presence bits         : %s" % ", ".join(
        "0x%04x -> next_tbtt=%d compress_ssid=%d ano=%d" % (
            k, bool(k & FC_NEXT_TBTT), bool(k & FC_COMPRESS_SSID), bool(k & FC_ANO))
        for k in fc_vals))
    print("IE offset used          : %s" % dict(offsets))
    if rssi:
        rssi.sort()
        print("RSSI dBm  min/med/max   : %d / %d / %d" % (rssi[0], rssi[len(rssi) // 2], rssi[-1]))

    print("\nIE chains (%d distinct):" % len(chains))
    for chain, n in chains.most_common():
        print("  x%-6d %s" % (n, list(chain)))
        print("          %s" % " ".join(EID_NAMES.get(e, str(e)) for e in chain))

    print("\nDTIM split              : %s" % dict(dtim_split))
    print("S1G Capabilities oct[6] : %s" % {("0x%02x" % k): v for k, v in caps6.items()})
    print("  RAW_OPERATION_SUPPORT (bit 3) set in: %d of %d"
          % (sum(v for k, v in caps6.items() if k & 0x08), sum(caps6.values())))

    print("\nEID-208 RPS elements    : %d distinct, %d beacons carry one"
          % (len(rps_vals), sum(rps_vals.values())))
    if not rps_vals:
        print("  *** NO RPS ELEMENT ON AIR ***")
    for val, n in rps_vals.most_common():
        pretty = " ".join("%02X" % b for b in val)
        print("  x%-6d %s" % (n, pretty))
        print("          golden (§7.1) %s" % " ".join("%02X" % b for b in GOLDEN_RPS))
        print("          BYTE-IDENTICAL: %s" % (val == GOLDEN_RPS))

    # Frame-count reconciliation (S0b-1 replaced the morse0 drop counter with this: 2.53 % of beacons
    # were genuinely missing while `dropped` moved by 1). ~2.5 % loss is the healthy baseline, not zero.
    if len(tsfts) >= 2:
        span_us = max(tsfts) - min(tsfts)
        bi_us = args.beacon_int_tu * 1024
        expected = span_us / bi_us + 1
        got = sum(chains.values())
        print("\nreconciliation (mactime):")
        print("  span                  : %.3f s over %d decoded beacons" % (span_us / 1e6, got))
        print("  TBTT slots spanned    : %.1f  (beacon_int %d TU = %d us)" % (expected, args.beacon_int_tu, bi_us))
        print("  capture completeness  : %.2f %% (loss %.2f %%; ~2.5 %% is the healthy bench baseline)"
              % (100.0 * got / expected, 100.0 * (1 - got / expected)))
    else:
        print("\nreconciliation: no radiotap TSFT in this capture — cannot reconcile")


if __name__ == "__main__":
    main()
