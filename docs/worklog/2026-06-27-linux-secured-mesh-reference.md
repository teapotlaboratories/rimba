# 2026-06-27 — Linux SECURED (SAE+AMPE) 802.11s mesh: working reference (Phase 0)

Brought up two Linux morse nodes as a **secured** 802.11s HaLow mesh — SAE authentication +
AMPE-encrypted peering + CCMP data — and confirmed they peer and pass **encrypted** IP traffic.
This is the gold-standard reference the ESP (morselib) mesh-security port must match. It also
**empirically settles the pivotal question**: encrypted mesh works on the MM6108 firmware, so the
−116 station-handle wall that blocks IBSS/ADHOC CCMP does **not** block the MESH vif — Linux
creates a per-peer station handle (`NEW_STATION`) and installs keys against it.

Bench: **chronium** `192.168.7.187` (MAC `3c:22:7f:37:50:42`) and **chronite** `192.168.7.191`
(MAC `3c:22:7f:37:51:38`), both RPi5 + MM6108, 1.17.8 morse stack, kernel `6.12.21-v8-16k+`.
S1G channel 27 → 5 GHz-model ch112 / `freq 5560` (915.5 MHz, 1 MHz BW, US). `wpa_supplicant_s1g`
on both is built with `CONFIG_SAE=y`.

## Result

```
# chronium: iw dev wlan1 station dump        # chronite log
Station 3c:22:7f:37:51:38 (on wlan1)         SAE: Selecting supported ECC group 19
    signal: -18 dBm                          SAE: State Confirmed -> Accepted (peer ...:50:42)
    mesh plink: ESTAB                         RSN: Cache PMK from SAE (len=32)
                                              wlan1: MESH-PEER-CONNECTED 3c:22:7f:37:50:42
# encrypted ping 10.9.9.2 -> 10.9.9.1: 5/5 received, 0% loss (first pkt slow = HWMP + SAE)
```

## Config — the secured delta

Identical to the open-mesh recipe (`2026-06-26-linux-mesh-reference.md`) except **two lines**:
`key_mgmt=NONE` → `key_mgmt=SAE`, and add `sae_password="…"`. `user_mpm=1` stays (the kernel
forces user-MPM when AMPE/secure is set anyway — `nl80211.c:8655` `if (is_secure) user_mpm=true`).
Full config saved at `docs/reference/captures/wpa-smesh.conf`; bring-up is the same recipe (mesh
leave → `pkill -9 -x wpa_supplicant_s1g` → clear ctrl socket → `set type managed` → up →
`wpa_supplicant_s1g -B -D nl80211 -i wlan1 -c … -dd`). Use `pkill -x` (exact name) — `pkill -f`
matches the launching ssh command line and self-kills.

## The secured handshake, observed on-air (gold standard for the port)

Ordered sequence from the `-dd` logs (sanitized extract at
`docs/reference/captures/secured-mesh-sequence.txt`). Each step is what morselib must reproduce.

### 1. SAE (auth) — produces the PMK
- `SAE: Selecting supported ECC group 19` — **group 19 = NIST P-256** (default). Configurable via
  `sae_groups`; group-19-only lets the port drop `dh_groups.c` (FFC/MODP).
- Commit (`auth_alg=3 auth_transaction=1`, IE len **98**): `own commit-scalar`(32) +
  `commit-element x`(32) + `commit-element y`(32). Confirm (`auth_transaction=2`).
- `State Nothing → Committed → Confirmed → Accepted`; out = **PMK (32 B)** + PMKID (16 B), cached.
- Crypto to port: EC point ops on P-256, dragonfly hash-to-element, SHA-256 / HMAC-KDF,
  const-time. Map onto mbedTLS `mbedtls_ecp_*`. (~3.8k LOC in hostap; the dominant cost.)

### 2. AMPE — protects peering + carries group keys
- `Initializing AMPE, Peering Start`. Plaintext AMPE element **98 B** → encrypted **114 B**:
  AES-SIV adds the 16 B MIC (`mesh_rsn_protect_frame`, `mesh_rsn.c:553`; `aes_siv_encrypt` over the
  AMPE IE keyed by `AEK`). AAD = `{own_addr, peer_addr, cat..}`.
- **GTKdata present in the Open frame only** (98 B): `MGTK`(16) + `Key RSC`(8, zeros) +
  `GTKExpirationTime = 4294967295` (0xffffffff). Confirm AMPE is 70 B (no GTKdata) → encrypted 86 B.
- Keys: **AEK** = `sha256_prf(PMK,"AEK Derivation",AKM‖min(MAC)‖max(MAC))` (32 B); **MTK** =
  `sha256_prf(PMK,"Temporal Key Derivation",min/max nonce‖min/max LID‖AKM‖min/max MAC)` (16 B,
  CCMP). **MGTK/IGTK are generated locally (random) and exchanged in the AMPE IE**, not derived.
  The `min/max` (os_memcmp) ordering is load-bearing — port byte-exact.

### 3. Key install — the firmware boundary (Phase-1 target)
After `mesh plink … established`, the per-peer station + keys are pushed (key-flag decode:
DEFAULT=0x02 RX=0x04 TX=0x08 GROUP=0x10 PAIRWISE=0x20):

| Step | nl80211 call | Meaning |
|---|---|---|
| sta_add | `NL80211_CMD_NEW_STATION` peer MAC, Flags:2 | create per-peer handle, move to ASSOC/AUTHORIZED |
| MTK | `set_key key_idx=0 set_tx=0 len=16 flag=0x2c` | pairwise MTK, RX+TX (alg=3 CCMP) |
| MGTK rx | `set_key key_idx=1 set_tx=0 len=16 flag=0x14` | peer's group key, RX |
| MGTK tx | `set_key key_idx=1 set_tx=1 len=16 flag=0x1a` | own group key, TX default |
| IGTK | `set_key key_idx=4 / 5 flag=0x10` | MFP/BIP protected-mgmt keys |
| defaults | `key_idx=0/1/2/3 flag=0x10/0x20 len=0` | default key selection |

In morselib these map to `SET_STA_STATE (0x14)` + `INSTALL_KEY (0x0A)` driven the **mesh** way
(sta_add → ASSOC → install), the order `key.c:160-161` (sta must be `uploaded`) and `cfg.c:524`
(sta must be ASSOC) require. This is Phase 1 of the port.

## Port implication (Phases 1–4)

The reference is now captured and reproducible. Mapping to the planned phases:
- **Phase 1 — key plumbing**: replicate the §3 table with *static* MTK/MGTK to prove the firmware
  boundary (the −116 question) in isolation.
- **Phase 2 — AMPE**: §2 (AMPE IE + AES-SIV MIC + MTK/AEK derive + MGTK/IGTK exchange) on a static PMK.
- **Phase 3 — SAE**: §1, replacing the static PMK (mbedTLS P-256 + dragonfly).
- **Phase 4 — MFP/IGTK + integration**, full on-air A/B against this reference.

## State at end of session
- chronium + chronite: running `wpa_supplicant_s1g` **secured** mesh `rimba-smesh`, plink **ESTAB**,
  IPs `10.9.9.1` / `10.9.9.2`, encrypted ping 0% loss. Runtime only — re-run the recipe after reboot.
- Artifacts: `docs/reference/captures/wpa-smesh.conf`, `…/secured-mesh-sequence.txt`.
- chronosalt (the would-be 3rd node / on-air monitor) is powered off pending an MM6108 power fix —
  the byte-level on-air monitor capture of the secured peering is deferred until it (or an ESP
  monitor) is available; the `-dd` logs cover protocol structure in the meantime.
