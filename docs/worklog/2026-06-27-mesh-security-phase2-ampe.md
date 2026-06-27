# 2026-06-27 — Mesh security Phase 2 (AMPE): per-peer-stad TX (P2a)

Phase 2 replaces Phase-1's static/shared keys with the real AMPE key hierarchy: a per-pair MTK
derived from a PMK + exchanged nonces, plus a per-node MGTK exchanged in an AES-SIV-protected AMPE
element — following hostap `wpa_supplicant/mesh_rsn.c` + `mesh_mpm.c` line by line. It is split into
four independently on-air-verifiable increments:

- **P2a — per-peer-stad TX** (this section, DONE): drive unicast mesh TX from the *per-peer* stad
  (queue + dequeue per peer, like the AP datapath) instead of the single common stad. Prerequisite
  for per-pair keys; verified with the existing static keys so the TX-architecture change is
  de-risked from the crypto.
- **P2b — derived per-pair MTK + nonce exchange** (next): `MTK = sha256_prf(PMK, "Temporal Key
  Derivation", min/max nonce ‖ LID ‖ AKM ‖ MAC)` on a static PMK; nonces carried in a throwaway IE.
- **P2c — per-node MGTK exchange**: each node generates its own MGTK and ships it to each peer.
- **P2d — real AES-SIV AMPE element**: the standard AMPE IE (139) + MIC IE (140) SIV-encrypted under
  the AEK, replacing the P2b/P2c scaffolding. Gold-standard byte-diff vs a live Linux secured node.

## Pivotal design finding: P2 is integration, not crypto authorship

The ESP build already links all the crypto P2/P3 need — confirmed in the bundled hostap
(`framework/src/hostap`): `sha256_prf` (KDF for MTK/AEK), `aes_siv_encrypt`/`aes_siv_decrypt` (the
AMPE MIC, RFC 5297 — the original Cozybit 802.11s primitive), `aes-omac1`/`aes-ctr` (SIV deps),
`crypto_get_random` (nonce/MGTK), and even `sae.c` (P3). They are exported to morselib through the
`mmint_*` ABI (`hostap_morse_common.h` mangles e.g. `#define sha256_prf mmint_sha256_prf`,
`crypto_get_random → mmint_crypto_get_random`). So P2b/P2c call `mmint_sha256_prf` +
`mmint_crypto_get_random` via a small morselib-local `extern` shim (avoids pulling hostap
`src/crypto/*.h` into morselib and its `utils/common.h` type clashes). The **only** net-new build
change in all of P2 is P2d adding `aes-siv.c` + `aes-ctr.c` to the hostap CMake SRCS (upstream
verbatim) and exporting `mmint_aes_siv_*` via `hostap_morse_common.h`.

## P2a — what changed

Phase-1's decisive gotcha was that mesh TX is **common-stad-driven**: every mesh frame dequeues from
the one common (MBSS) stad, so a single shared MTK on the common stad is what encrypts unicast. That
works only because the P1 keys are static/shared. P2's per-pair MTK means unicast to peer B must be
encrypted under the B-specific key, i.e. queued on and dequeued from **B's per-peer stad**. P2a makes
that switch (keys still static, so the result is observably identical — this isolates the TX change):

`umac/datapath/umac_datapath.c`
- `umac_datapath_lookup_stad_by_tx_dest_addr_mesh` (the TX enqueue lookup) now returns the **per-peer
  stad** for a unicast dest (`umac_mesh_get_peer_stad(addr)`), falling back to the common stad for
  broadcast/multicast/zero or an unknown/not-yet-ESTAB dest. Previously it always returned the common
  stad. Mirrors the existing RX lookup and the AP per-STA model. (Single-hop: dest == next hop; the
  MTK is per *link*, so HWMP multi-hop needs a next-hop resolve here too — a later step.)
- `umac_datapath_tx_dequeue_frame_mesh` rewritten from a common-stad-only pop into an AP-style
  scheduler: a new pure helper `mesh_get_next_tx_stad()` returns the common stad first (broadcast/
  mgmt + group MGTK), then the first established peer stad with queued traffic — mirroring
  `umac_ap_get_next_sta_for_tx` walking `data->stas[]`. The dequeue scans + pops + recomputes
  `has_more` under one critical section; `has_more` is an aggregate re-scan (mesh has no single
  queued-frame counter like the AP's `num_pkts_queued`), so the other peers' queues never stall.

`umac/mesh/umac_mesh.{c,h}`
- `umac_mesh_peer_stad_at(size_t index)` — enumerates peer slot `index`'s stad, but only when that
  slot is `used && state == MESH_PLINK_ESTAB` (else NULL), letting the scheduler iterate established
  peers without exposing `mesh_peers[]`.
- `UMAC_MESH_MAX_PEERS` lifted into the header as the single source of truth; the .c's
  `MESH_MAX_PEERS` now derives from it (the datapath loop and the peer table agree by construction).

## On-air proof (2026-06-27, board0 + board1, ch27 clear, chronium `morse0` monitor)

2-board direct test (board1 → board0, both on the P2a build). Linux mesh stopped (ch27 clear); the
third ESP (board2) was on a PPK2 power-profiling rig with older fw, so a 2-node direct ping was used
— board1's ping target was temporarily pointed at board0 (`10.9.9.136`) for this.

```
board1 console:  54 replies, 0 timeouts  (seq 54..93 unbroken)  -> 0% loss
chronium morse0 pcap (694 frames, tshark):
  QoS-Data Protected tally:
    board0 (e2:72:a1:f8:ef:a4):  28 Protected=1,  0 plaintext
    board1 (e2:72:a1:f8:f9:40):  29 Protected=1   (all 28 unicast->board0 + 1 broadcast)
                                  1 plaintext = a startup broadcast gratuitous ARP (10.9.9.100),
                                  common-stad path, in the window before the group key went active
                                  — benign, identical to P1, NOT on the unicast path P2a changed.
  unicast board1->board0 frame:  Protected=1, RA=board0 / TA=board1,
                                  CCMP Ext-IV present, Key Index = 0  (pairwise MTK, per-peer stad)
```

**0% loss is the decisive functional proof**: if the per-peer enqueue and the new scheduler dequeue
were inconsistent, unicast frames would stick on the per-peer queue and never TX (exactly the
regression hit mid-P1 when only the enqueue was moved to the per-peer stad). They drain cleanly, and
the CCMP **Key Index 0** confirms the encrypting key is the **pairwise MTK selected from the per-peer
stad**, not the group key — the per-peer-stad TX path is correct end to end. This is the foundation
P2b's per-pair derived MTK installs onto.

Multi-peer drain (one relay node with two established peers) is verified by construction — the
scheduler loops over all peer slots — and is deferred to a later 3-node on-air run once board2 is
free of the profiling rig.

## Bench note

While freeing a serial port for flashing, a `fuser -k` on `/dev/ttyACM2` killed `/tmp/ppk2_hold.py`
(the PPK2 power-hold for board2 — the USB ports had re-enumerated, so ACM2 was the PPK2, not board2's
console). That cut board2's 5 V DUT power. Restored by re-running `/tmp/ppk2_hold.py` with the IDF
python (which has `ppk2_api`): board2 re-enumerated and power is held again (ampere-meter, DUT ON).
Lesson: identify each `/dev/ttyACM*` by USB VID:PID (Espressif `303a:1001` vs Nordic PPK2 `1915:c00a`)
before assuming a port is a board console — USB re-enumeration shuffles the ACM numbers.
