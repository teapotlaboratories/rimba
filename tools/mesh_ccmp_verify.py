# Offline mesh-CCMP MIC verify. Replicates morselib ccmp.c:mesh_ccmp_aad_nonce exactly.
# usage: verify_ccmp.py <key_hex_16B> <frame_hex>   (frame = full 802.11 MPDU after radiotap)
import sys
from cryptography.hazmat.primitives.ciphers.aead import AESCCM

key = bytes.fromhex(sys.argv[1].replace(' ', ''))
f = bytes.fromhex(sys.argv[2].replace(' ', ''))
assert len(key) == 16, "key must be 16 bytes (MGTK)"

fc = f[0] | (f[1] << 8)
ftype = (fc >> 2) & 3
stype = (fc >> 4) & 0xf
addr4 = 1 if (fc & 0x0300) == 0x0300 else 0
qos = 0
nonce = bytearray(13)
if ftype == 2:                      # DATA
    fc &= ~0x0070
    if stype & 0x08:
        qos = 1
        fc &= ~0x8000
        nonce[0] = f[24 + (6 if addr4 else 0)] & 0x0f
elif ftype == 0:                    # MGMT
    nonce[0] |= 0x10
fc &= ~(0x0800 | 0x1000 | 0x2000)   # retry, pwrmgmt, moredata
fc |= 0x4000                        # protected
aad = bytearray([fc & 0xff, (fc >> 8) & 0xff])
aad += f[4:22]                      # A1 A2 A3
seq = (f[22] | (f[23] << 8)) & ~0xfff0
aad += bytes([seq & 0xff, (seq >> 8) & 0xff])
base = len(aad)
aad += f[24:24 + addr4*6 + qos*2]   # A4 (if 4addr) + QoS (if qos)
if qos:
    qb0 = base + addr4*6
    aad[qb0] &= (~0x70) & 0xff
    aad[qb0] &= (~0x80) & 0xff
    aad[qb0+1] = 0x00
nonce[1:7] = f[10:16]               # A2
hdr_len = 24 + addr4*6 + qos*2
ccmp = f[hdr_len:hdr_len+8]
key_id = (ccmp[3] >> 6) & 3
nonce[7], nonce[8], nonce[9] = ccmp[7], ccmp[6], ccmp[5]
nonce[10], nonce[11], nonce[12] = ccmp[4], ccmp[1], ccmp[0]
pn = bytes([ccmp[7], ccmp[6], ccmp[5], ccmp[4], ccmp[1], ccmp[0]]).hex()
body_start = hdr_len + 8
print("addr4=%d qos=%d hdr_len=%d key_id=%d PN=%s aad_len=%d" % (addr4, qos, hdr_len, key_id, pn, len(aad)))
print("nonce=%s" % bytes(nonce).hex())
print("aad=%s" % bytes(aad).hex())
ok = False
for fcs in (0, 4):
    end = len(f) - fcs
    ct_mic = f[body_start:end]
    if len(ct_mic) < 9:
        continue
    try:
        pt = AESCCM(key, tag_length=8).decrypt(bytes(nonce), bytes(ct_mic), bytes(aad))
        print("*** MIC VERIFIED (fcs_stripped=%d)  ct_len=%d plaintext[:24]=%s" % (fcs, len(ct_mic)-8, pt[:24].hex()))
        ok = True
    except Exception as e:
        print("    MIC FAIL (fcs_stripped=%d, ct_len=%d): %s" % (fcs, len(ct_mic)-8, type(e).__name__))
sys.exit(0 if ok else 2)
