#!/usr/bin/env python3
"""
Offline validation of the ESP mesh AMPE AES-SIV model against a LIVE Linux (hostap) peer.

P3d gold-standard check WITHOUT board instrumentation: take chronite's own ground truth from its
`wpa_supplicant_s1g -dd -K` log (it dumps `SAE: PMK`, the self-protected action frame, and the
plaintext + encrypted AMPE element) and replay it through a known-correct AES-SIV. If the oracle
reproduces chronite's encrypt, then the AEK derivation + AAD construction + SIV algorithm are exactly
what the ESP must implement (and the ESP source matches them line-for-line).

Reference AES-SIV = pycryptodome `AES.MODE_SIV` (RFC 5297), first checked against the RFC 5297 §A.1 KAT.
hostap's own `src/crypto/aes-siv.c` (== the ESP's `mmint_aes_siv_*`) implements the same RFC.

    python3 -m venv /tmp/sivenv && /tmp/sivenv/bin/pip install pycryptodome
    /tmp/sivenv/bin/python docs/mesh-ap/ampe-siv-validate.py

The vector below is chronite (3c:22:7f:37:51:38) <-> board0 (e2:72:a1:f8:ef:a4), PMK from the peering
at log line 4009, its Confirm-bearing frame (5557/5559). VERIFIED PASS 2026-06-28.
"""
import struct, hmac, hashlib
from Crypto.Cipher import AES

def h(s): return bytes.fromhex(s.replace(' ', '').replace('\n', ''))

def sha256_prf(key, label, data, outlen):
    """IEEE 802.11 PRF: HMAC-SHA256(key, LE16(counter) || label || data || LE16(bitlen))."""
    out, counter = b'', 1
    while len(out) < outlen:
        blk = struct.pack('<H', counter) + label + data + struct.pack('<H', outlen * 8)
        out += hmac.new(key, blk, hashlib.sha256).digest()
        counter += 1
    return out[:outlen]

# --- 1. RFC 5297 A.1 KAT: confirm the reference SIV matches the standard hostap follows ---
_k  = h('fffefdfcfbfaf9f8f7f6f5f4f3f2f1f0 f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff')
_ad = h('101112131415161718191a1b1c1d1e1f 2021222324252627')
_pt = h('112233445566778899aabbccddee')
_c = AES.new(_k, AES.MODE_SIV); _c.update(_ad)
_ct, _tag = _c.encrypt_and_digest(_pt)
assert _tag + _ct == h('85632d07c6e8f37f950acd320a2ecc93 40c02b9690c4dc04daef7f6afe5c'), 'KAT FAIL'
print('RFC5297 KAT: PASS')

# --- 2. chronite's coherent vector (from /tmp/wpa-interop.log on chronite) ---
chronite = h('3c227f375138')                # TA / own (the encryptor)
board0   = h('e272a1f8efa4')                # RA / peer
akm_sae  = h('000fac08')                    # RSN_AUTH_KEY_MGMT_SAE
cat6     = h('0f03720a7269')                # first 6 action-body bytes (category, action, MeshID EID/len/'r''i')
PMK      = h('8c8110bccf90f1e0794371c15cb605a7275a115df9aad4d9b19c2585172243 2c'.replace('172243 2c', '1722432c'))
crypt    = h('b4507be17220671b7b0d35f82ada5696c7dc50bff15d945e0539a264df553edee9765e1f02253c39804c7e77965e2040'
             '73a4f219f8493502b4be3df927383d16a44b49f8746be4c5d1ca40bb6fe124cb67d77b4f31d0')
expect_pt= h('8b44000fac0450b9985e8e144736757bab7c4afe3ec1f03e297176a8449a29043c8b4e2f7555' + '00' * 32)

# AEK = sha256_prf(PMK || 32 zeros, "AEK Derivation", AKM || min(MAC) || max(MAC))
pmk64 = PMK + b'\x00' * 32
lo, hi = (chronite, board0) if chronite < board0 else (board0, chronite)
aek = sha256_prf(pmk64, b'AEK Derivation', akm_sae + lo + hi, 32)
print('derived AEK:', aek.hex())

# full SIV verify with AAD = {own, peer, cat6} (hostap encrypt order)
siv, C = crypt[:16], crypt[16:]
dec = AES.new(aek, AES.MODE_SIV)
for comp in (chronite, board0, cat6):
    dec.update(comp)
pt = dec.decrypt_and_verify(C, siv)
print('SIV verify: PASS' if pt == expect_pt else 'SIV verify: PASS but plaintext mismatch')
print('recovered == logged plaintext:', pt == expect_pt)
