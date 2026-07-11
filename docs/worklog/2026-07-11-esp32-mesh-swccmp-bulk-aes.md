# 2026-07-11 — ESP32 mesh SW-CCMP: bulk-DMA AES-CCM (relay crypto ~14-28× cheaper), + 240 MHz + in-place

Root-caused and fixed the host **SW-CCMP** per-frame crypto cost that shows up as a relay bottleneck under
concurrent load (the "host SW-CCMP relay bottleneck" flagged in the mesh-gate Performance section). SW-CCMP is
the mesh's host-side CCMP: the MM6108 firmware holds **no** mesh key, so it delivers protected mesh frames raw
and the host encrypts/decrypts — this is what sidesteps the #20 HW-crypto A4≠TA firmware gate (see
`2026-07-11-mesh-20-linux-also-withholds-fw-limitation.md`), so keeping crypto host-side is a hard requirement.
A relay pays CCMP **twice** per forwarded frame (RX decrypt keyed by TA + TX re-encrypt keyed by next-hop;
802.11s per-link keys make re-encryption unavoidable).

## Bottom line
1. **Root cause (measured on-device, not theory):** the CCM ran AES **one 16-byte ECB block at a time** —
   hostap `aes-ccm.c` `aes_ccm_ae/ad` → `aes_encrypt` = `mbedtls_aes_crypt_ecb` (ESP32-S3 HW AES), **~2·⌈len/16⌉
   ≈ 187 single-block ops per 1442 B frame**. Each ECB op pays the full `esp_aes` fixed wrapper
   (`esp_aes_acquire_hardware` = global `AES_LOCK` mutex + bus-clock enable + peripheral reset → per-call
   DMA-descriptor `heap_caps_aligned_calloc` → poll [len ≤ 2000 never interrupts, so
   `CONFIG_MBEDTLS_AES_USE_INTERRUPT` is inert here] → release). **≈ 384 `AES_LOCK` acquire/release per relayed
   frame.** It is that fixed per-op overhead + lock churn, not the AES math, that dominates.
2. **Fix:** a bulk-DMA CCM — **one** `mbedtls_aes_crypt_cbc` (CBC-MAC) + **one** `mbedtls_aes_crypt_ctr` + **one**
   ECB (S₀), i.e. ~3 HW-AES passes instead of ~187, and ~6 lock acquisitions instead of ~384. Plus CPU
   160→240 MHz and in-place crypto (no scratch copies).
3. **Measured crypto cost dropped ~14-28× (avg) / ~3.6× (min).** Proven correct 3 ways (RFC-3610 KAT +
   `mbedtls_ccm` cross-check + on-device ping).
4. **HONEST SCOPE:** single-flow relay *throughput* is **airtime-bound** (single-MPDU, no A-MPDU — the
   Performance section's open-vs-secured A/B [open ~0.14 vs secured-CCMP ~0.23 Mbit/s] already showed CCMP is
   not the dominant single-flow relay cost). **This change does NOT raise the single-flow throughput ceiling.**
   Its value is (a) the **concurrent-load** relay case, where the relay ESP saturates carrying its own traffic
   *and* relaying — the 14-28× CPU reduction + collapsed per-frame latency frees relay headroom; (b) lower
   per-frame latency under contention (≈11 ms → ≈0.6 ms wall-clock); (c) freeing the shared AES peripheral for
   concurrent SAE/AMPE/beacon crypto; and (d) a **prerequisite** so crypto doesn't become the bottleneck once
   A-MPDU lifts the airtime ceiling.

## 1. Measurement method + baseline
Instrumented `mesh_ccmp_encrypt/decrypt` (`morselib/.../supplicant_shim/ccmp.c`) with the Xtensa cycle counter
(`rsr ccount`) around the CCM call, tracking running min + count, logged per 500 frames. App
`rimba-halow-mesh-perf` (`MESH_IPERF` line topology), secured mesh (SAE+AMPE+CCMP), SW-CCMP default
(`g_mesh_sw_crypto=true`). Driven with 1400 B pings (each round-trip makes the peer do enc+dec of full-size
frames). Per 1442 B frame, wall-clock:

| CCMP | Original per-block (160 MHz) | bulk CCM (160 MHz) | bulk + 240 MHz + in-place |
|---|---|---|---|
| encrypt min / **avg** | 339 µs / **7038 µs** | 154 µs / 254 µs | **94 µs / 197 µs** |
| decrypt min / **avg** | 290 µs / **4401 µs** | 142 µs / 321 µs | **102 µs / 285 µs** |

The **min→avg gap collapsed from ~21× to ~1.6×** — the ~187-op window was where lock contention + task
preemption inflated the wall-clock per frame; ~3 bulk ops removes it. Crypto *ceiling* per forwarded frame
(enc+dec): contended ~11 ms → ~0.6 ms wall-clock (≈ 1 → 24 Mbit/s *if* crypto were the limiter — it is not at
the current airtime-bound throughput, see scope note above). ⚠️ The temp instrumentation hardcodes
`µs = cyc/160`; at 240 MHz its printed µs are 1.5× inflated — the cycle counts are truth, divide by 240.

## 2. The bulk-DMA CCM (byte-exact, RFC-3610)
`esp_mesh_ccm_ae/ad` + `esp_ccm_cbc_mac` (static, file-local in `ccmp.c`, L=2, aad ≤ 30, M ∈ {8,16}):
- **CBC-MAC**: build `[B₀ | len16‖AAD zero-padded | plaintext zero-padded]` in a static scratch, one
  `mbedtls_aes_crypt_cbc(ENCRYPT, IV=0)`; the **last output block is the MAC T** (CBC-MAC = CBC with IV=0).
- **CTR**: one `mbedtls_aes_crypt_ctr` from counter block A₁ = `[L-1 | nonce13 | 0x0001]` (mbedtls increments
  the full 16-byte counter; the low 2 bytes never overflow for < 65536 blocks, so it matches CCM's 2-byte
  counter). CTR is in==out safe.
- **MIC** = T ⊕ E(A₀), A₀ = `[L-1 | nonce13 | 0x0000]` (one ECB). Decrypt = CTR (symmetric) → CBC-MAC over the
  recovered plaintext → constant-time compare against T ⊕ E(A₀).

**Correctness — proven 3 independent ways** (boot self-test during dev, all PASS):
- **RFC-3610 Packet-Vector-#1 KAT** — exact (ct/MIC match the published vector).
- **byte-identical to `mbedtls_ccm_encrypt_and_tag`** at sizes 63…1600 (authoritative independent impl).
- on-device: mesh peers + pings work with the datapath on the bulk CCM (36-501 replies across runs).
(A dev-only scare — "A/B fail at exactly len=1500" — was a **test-harness buffer edge** [test buffers were
`[1500]`]; bumping to `[1600]` cleared it. Not a crypto bug.)

Only the **mesh datapath** uses the bulk CCM; the SAE/AMPE/TLS handshake keeps the per-block hostap path. The
datapath is single-task, so the static CBC scratch is race-free.

## 3. CPU 240 MHz + in-place
- `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y` in `rimba-halow-mesh-perf/sdkconfig.defaults` (was 160). ~1.3-1.5× on
  the CPU/DMA-orchestration portion. **Mains-powered relay/perf only — NOT for sleep/STA builds (power).**
- In-place crypto: dropped `ct_scratch`/`pt_scratch` (−3.2 KB static RAM) + the two full-payload memcpy-backs in
  `umac_datapath_sw_ccmp_encrypt/decrypt`; the datapath now passes `body_out == body_in`. Safe because the
  bulk CCM takes the CBC-MAC over the scratch copy before the in==out-safe CTR pass. On encrypt the prepend +
  memmove shifts only the MAC header, leaving the in-place ciphertext where the CCMP slot opens; on decrypt the
  plaintext is already at data-start after the CCMP-header strip.

## 4. Gotchas learned
- **Duplicate-symbol landmine.** `crypto_mbedtls_mm.c` exists **twice** — `components/shims/` and a vendored
  copy in `mm-iot-sdk/.../hostap/` compiled into morselib. Adding a new exported symbol to the shims copy and
  referencing it from morselib drags **both** into the link → `multiple definition of mmint_crypto_ec_*`. (An
  incidental app-level reference had been masking it via archive link order.) **Fix: keep the CCM self-contained
  in `ccmp.c`** (morselib, includes `<mbedtls/aes.h>`) — no cross-component symbol, no conflict.
- The `esp_aes` DMA driver **polls** for len ≤ `AES_DMA_INTR_TRIG_LEN` (2000), so `CONFIG_MBEDTLS_AES_USE_INTERRUPT`
  is inert for every ≤1500 B mesh frame — toggling it does nothing.
- A raw `printf` inside a morselib driver task stack-overflows → boot loop; use `esp_rom_printf` (or read
  `ccount` via inline asm) for datapath-context tracing.

## 5. Non-levers ruled out
`mbedtls_ccm_*` (loops per-block ECB internally; no HW CCM on S3, only HW GCM) — reproduces the overhead;
pure SW AES (`CONFIG_MBEDTLS_HARDWARE_AES=n`) ~3× *slower* + steals HW offload from SAE; dual-core parallel AES —
impossible (single AES peripheral behind one global lock); GCMP — breaks CCMP interop with Linux; per-STA
key-schedule caching — ~1-3 µs (HW re-uploads the key per op regardless).

## 6. Files changed (clean diff, uncommitted)
- `morselib/.../supplicant_shim/ccmp.c` — bulk CCM (self-contained statics) + datapath calls it + a quiet
  RFC-3610-KAT `esp_mesh_ccm_selftest` (not auto-called).
- `morselib/.../umac/datapath/umac_datapath.c` — in-place encrypt/decrypt.
- `firmware/rimba-halow-mesh-perf/sdkconfig.defaults` — 240 MHz.

Dropped (inert here): the latent `aes_encrypt_init` `setkey_dec→enc` fix in `crypto_mbedtls_mm.c:675` — a real
one-line correctness bug for the per-block path, but inert on HW AES (re-derives the schedule from the mode
flag) and unused by the bulk path (which calls `mbedtls_aes_setkey_enc` directly). Worth a separate hygiene
commit.

## 7. Disposition + next
The host SW-CCMP relay crypto is no longer a meaningful CPU/latency cost. The **remaining, dominant** relay
throughput limiter is airtime: **mesh sends single MPDUs, no A-MPDU aggregation** (memory
`mesh-no-ampdu-aggregation`) — per-frame preamble/IFS/backoff/ACK caps relay goodput independent of CPU. The
crypto fix is the prerequisite so crypto doesn't re-emerge as the bottleneck once A-MPDU lifts that ceiling.
Optional follow-on: in-place forward (rewrite headers on the RX mmpkt vs `build_mgmt_frame` alloc+copy in
`umac_mesh_forward_data`).

Bench radio-silent after (3 ESPs on `rimba-hello`). Nothing committed.
