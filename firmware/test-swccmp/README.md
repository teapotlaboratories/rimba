# test-swccmp — host SW-CCMP correctness (RFC-3610 KAT)

**Status: implemented + hardware-verified** (board0, 2026-07-16: `rc=0`, 749 µs).

The one mesh-crypto regression test that needs **no radio, no peer, and is fully
deterministic** — so a failure is unambiguously the code, never the RF link.

## What it proves

Multi-hop mesh forwarding on this project ships with **host software CCMP**
(`g_mesh_sw_crypto=true`). HW-crypto mesh forwarding is a firmware limitation — the MM6108
HW-crypto path emits a forwarded frame whose CCMP MIC does not verify under the relay's own
group key, so a canonical receiver rejects it (root-caused 2026-07-15, offline MIC-verify;
see `docs/mesh-ap/rimba-mesh-20-hwcrypto-forward-investigation.md`). Every multi-hop mesh
milestone therefore rests on the host CCM implementation being **byte-exact**.

That implementation is not stock hostap: it is a **bulk-DMA AES-CCM**
(`components/halow/.../umac/supplicant_shim/ccmp.c`) that replaces hostap `aes-ccm.c`'s
per-block HW-AES-ECB calls with three bulk passes (one CBC-MAC, one CTR, one ECB) — roughly
14–28× faster crypto under load (merged 2026-07-11). Bulk-vs-per-block is exactly the kind
of thing a toolchain / mbedtls bump can silently break, and it would surface as *"the mesh
drops ~99% of forwarded frames"* — days of bench time away from the cause.

This app calls `esp_mesh_ccm_selftest()` (`ccmp.c:141`), the RFC-3610 **Packet-Vector-#1**
known-answer test + decrypt round-trip. Its own comment says *"available for a boot
check"* — this is that boot check, wired to the harness. **PASS ⇒ the bulk-CCM encrypt path
produces the exact RFC-3610 ciphertext and MIC, and decrypt round-trips, on real ESP32-S3
silicon with the real mbedtls / HW-AES backend** — which is what makes our mesh MICs
interoperate with Linux / mac80211.

## What it does NOT prove

Any **framing**. The KAT exercises the CCM primitive only — not the 802.11 AAD/nonce
construction (`mesh_ccmp_aad_nonce`), not the defrag-before-decrypt ordering, not replay
protection. Those need real frames on the air — see `test-mesh-relay`.

## How to run

### With the harness (recommended)

```sh
source vendor/esp-idf/export.sh
python tools/regtest/run.py t2 --test swccmp
```

The harness picks any present board (radio-free, so wiring doesn't matter), flashes,
scrapes the verdict, and returns the board to `rimba-hello`. Exit 0 ⇔ PASS.

### By hand

```sh
# resolve the port by efuse MAC, never a cached ttyACM
PORT=/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_E0:72:A1:F8:EF:A4-if00  # board0
make flash APP=test-swccmp BOARD=proto1-fgh100m PORT=$PORT
make monitor APP=test-swccmp BOARD=proto1-fgh100m PORT=$PORT   # Ctrl-] to exit
# then, to leave the bench radio-silent:
make flash APP=rimba-hello BOARD=proto1-fgh100m PORT=$PORT
```

## Expected output

```
TEST|BEGIN|name=swccmp|rig=any single board; no radio, no peer
TEST|INFO|target: esp_mesh_ccm_selftest() -- RFC-3610 Packet-Vector-1 KAT ...
TEST|STEP|rfc3610-kat|PASS|rc=0 elapsed_us=749
TEST|RESULT|PASS|rfc3610-pv1 ciphertext+MIC exact, decrypt round-trip OK (rc=0, 749 us)
TEST|END|name=swccmp
```

A `TEST|RESULT|FAIL|...` line means the bulk-CCM no longer matches RFC-3610: mesh MICs
will not interop with Linux and multi-hop forwarding will drop ~all frames. Suspect the
mbedtls / HW-AES backend or a change to `esp_ccm_cbc_mac` / `esp_mesh_ccm_ae` in `ccmp.c`.

**No `TEST|RESULT` line at all** = the app hung or crashed. The harness treats that as
FAIL — silence is not a pass.

## Note on the morselib symbol

`esp_mesh_ccm_selftest` is exported for this test by adding it to
`components/mm-iot-sdk/framework/tools/metadata/protected_syms.txt`. morselib's build runs
`librarymangler.py`, which renames every symbol to `mmint_*` **except** those in that list;
without the entry the selftest ships as `mmint_esp_mesh_ccm_selftest` and only
morselib-internal callers can reach it. This is the upstream-sanctioned way to widen the
public surface (glob entries like `mmwlan*` / `mmhal*` are how the rest of the API is
exposed) — not a local hack. See the code-map in the worklog.
