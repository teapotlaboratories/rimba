/* test-swccmp — host SW-CCMP correctness, on-target, no radio.
 *
 * Mesh forwarding on this project ships with HOST software CCMP (g_mesh_sw_crypto=true):
 * HW-crypto mesh forwarding is a firmware limitation — the MM6108 HW-crypto path emits a
 * forwarded frame whose CCMP MIC does not verify under the relay's own group key, so any
 * canonical receiver rejects it (root-caused 2026-07-15 by offline MIC-verify; see
 * docs/mesh-ap/rimba-mesh-20-hwcrypto-forward-investigation.md). Every multi-hop mesh
 * milestone therefore rests on the host CCM implementation in
 * components/halow/.../umac/supplicant_shim/ccmp.c being byte-exact.
 *
 * That implementation is not stock hostap: it is a bulk-DMA AES-CCM that replaces
 * hostap aes-ccm.c's per-block HW-AES-ECB calls with three bulk passes (one CBC-MAC, one
 * CTR, one ECB) — ~14-28x faster crypto under load (merged 2026-07-11). Bulk-vs-per-block
 * is exactly the kind of optimisation a toolchain/mbedtls bump can silently break, and it
 * would break as "the mesh drops ~99% of forwards", days of bench time away from the
 * cause. This test makes that failure show up in 2 seconds with no bench at all.
 *
 * What it does: calls esp_mesh_ccm_selftest() (ccmp.c:141), the RFC-3610
 * Packet-Vector-#1 known-answer test + decrypt round-trip. Its own comment says
 * "available for a boot check" — this is that boot check, wired to the harness.
 *
 * WHAT THIS PROVES: the bulk-CCM encrypt path produces the exact RFC-3610 ciphertext and
 * MIC, and decrypt round-trips, on real ESP32-S3 silicon with the real mbedtls/HW-AES
 * backend. That is what makes our MICs interoperate with Linux/mac80211.
 *
 * WHAT THIS DOES NOT PROVE: anything about framing. The KAT covers the CCM primitive
 * only — not the 802.11 AAD/nonce construction (mesh_ccmp_aad_nonce), not the
 * defrag-before-decrypt ordering, not replay. Those need frames, i.e. test-mesh-relay.
 *
 * NO RADIO. Runs on any board, needs no peer, and is deterministic — so it is the one
 * mesh-crypto regression test that can never be blamed on the RF link.
 */

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "test_report.h"

#define NAME "swccmp"
#define RIG  "any single board; no radio, no peer"

/* Defined in components/halow/components/mm-iot-sdk/framework/morselib/src/umac/
 * supplicant_shim/ccmp.c:141. Non-static, but morselib exposes no prototype for it
 * (umac_supp_shim.h declares only mesh_ccmp_key_id), so declare it here. Returns 0 on
 * PASS. If a future morselib renames or removes it, THIS APP FAILS TO LINK — which is a
 * correct and loud outcome for a port-forward: the KAT vanishing must not be silent. */
extern int esp_mesh_ccm_selftest(void);

void app_main(void)
{
    /* Give the console a moment so the BEGIN line is not swallowed by boot spam. */
    vTaskDelay(pdMS_TO_TICKS(500));

    TEST_BEGIN(NAME, RIG);
    TEST_INFO("target: esp_mesh_ccm_selftest() -- RFC-3610 Packet-Vector-1 KAT "
                 "+ decrypt round-trip (ccmp.c:141)");
    TEST_INFO("covers: bulk-DMA AES-CCM (CBC-MAC + CTR + ECB) used by the mesh "
                 "host SW-CCMP datapath (g_mesh_sw_crypto=true is the shipping default)");

    int64_t t0 = esp_timer_get_time();
    int rc = esp_mesh_ccm_selftest();
    int64_t us = esp_timer_get_time() - t0;

    TEST_STEP("rfc3610-kat", rc == 0, "rc=%d elapsed_us=%lld", rc, us);

    if (rc == 0)
    {
        TEST_PASS("rfc3610-pv1 ciphertext+MIC exact, decrypt round-trip OK "
                     "(rc=0, %lld us)", us);
    }
    else
    {
        /* A non-zero rc means one of: wrong ciphertext, wrong MIC, or a failed decrypt
         * round-trip. Any of those means mesh forwarding MICs will not match Linux — the
         * whole multi-hop stack is broken, silently, at ~99% frame loss. */
        TEST_FAIL("rc=%d -- bulk-CCM no longer matches RFC-3610. Mesh MICs will NOT "
                     "interop with Linux/mac80211 and multi-hop forwarding will drop "
                     "~all frames. Suspect the mbedtls/HW-AES backend or a change to "
                     "esp_ccm_cbc_mac/esp_mesh_ccm_ae in ccmp.c.", rc);
    }

    TEST_END(NAME);

    /* Park host-awake. Never auto-run into sleep: a sleeping app powers the native USB
     * down and re-enumerates it constantly, so esptool can never land the board in
     * download mode and every later flash fails "No serial data received"
     * (docs/reference/rimba-bench-devices.md:221-236). Staying awake keeps the board
     * reflashable without a PPK2 power-cycle. */
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
