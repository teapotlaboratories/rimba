/*
 * rimba-halow-mesh — 802.11s mesh P0: firmware-feasibility probe.
 *
 * The gate before the multi-PR 802.11s mesh port (P1-P5, see
 * docs/worklog/2026-06-24-mesh-80211s-port-recon.md). On Linux the morse driver
 * drives mesh with firmware commands (MESH_CONFIG, ADD_INTERFACE(type=5), ...),
 * and the ESP32 .mbin is byte-identical to the Linux blob for the paths we've
 * checked so far — but morselib never sends a mesh command, so it is unknown
 * whether *this* firmware actually recognizes a mesh interface.
 *
 * This app asks the blob directly: it sends ADD_INTERFACE for a matrix of
 * interface types and reports the firmware's verdict per type. The key row is
 * MESH(5). Controls bracket it:
 *   - STA(1)/AP(2)/ADHOC(4)  — types known to be accepted (ADHOC per the IBSS recon)
 *   - MESH(5)                — the question
 *   - BOGUS(99)              — an undefined type; must be rejected
 * If MESH behaves like the known-good types (fw status 0) and unlike BOGUS, the
 * firmware recognizes mesh → P1 is unblocked. If MESH behaves like BOGUS, mesh
 * is firmware-gated and the whole effort changes.
 *
 * Each probe adds then immediately removes the interface
 * (mmprobe_iface_type_supported), so the matrix doesn't exhaust interface slots.
 */

#include <inttypes.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"

#include "mmhalow.h"

/* Probe hooks live in morselib's internal header (not the public API), defined in
 * morselib/src/driver/driver.c. morselib's build (librarymangler.py) prefixes every
 * non-public symbol with 'mmint_'; that 'mmint*' prefix is itself kept link-visible
 * (framework/tools/metadata/protected_syms.txt), so the mangled names ARE callable
 * from an app. Reference the mangled names, alias them back for readability. */
extern int  mmint_mmprobe_iface_type_supported(uint32_t iface_type, uint32_t *fw_status_out);
extern int  mmint_mmprobe_rm_iface_raw(uint16_t vif_id);
extern void mmint_mmprobe_dump_fw_caps(void);
#define mmprobe_iface_type_supported mmint_mmprobe_iface_type_supported
#define mmprobe_rm_iface_raw         mmint_mmprobe_rm_iface_raw
#define mmprobe_dump_fw_caps         mmint_mmprobe_dump_fw_caps

/* enum morse_cmd_interface_type (morse_commands.h): STA=1 AP=2 MON=3 ADHOC=4 MESH=5 */
#define IF_STA    1u
#define IF_AP     2u
#define IF_ADHOC  4u
#define IF_MESH   5u
#define IF_BOGUS  99u

static const char *TAG = "rimba-mesh";

static void probe_type(const char *label, uint32_t type)
{
    uint32_t fw = 0;
    int ret = mmprobe_iface_type_supported(type, &fw);
    const char *verdict;
    if (ret != 0)
    {
        verdict = "TRANSPORT FAIL";
    }
    else if (fw == 0)
    {
        verdict = "ACCEPTED (fw status 0)";
    }
    else
    {
        verdict = "REJECTED by firmware";
    }
    ESP_LOGW(TAG, "  ADD_INTERFACE(%-9s type=%2u): transport ret=%d fw_status=%" PRIu32
                  "  -> %s", label, (unsigned)type, ret, fw, verdict);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== 802.11s mesh P0: firmware interface-type probe ===");

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    mmhalow_init(NULL);
    mmhalow_print_version_info();

    /* What does the loaded .mbin advertise? */
    ESP_LOGI(TAG, "--- firmware capability words ---");
    mmprobe_dump_fw_caps();

    /* Clear any boot-time interface so the matrix adds are sole interfaces and a
     * single-interface firmware doesn't reject by slot rather than by type.
     * Harmless (logs the result) if there is nothing at vif 0/1. */
    ESP_LOGI(TAG, "--- clearing boot interface(s) ---");
    ESP_LOGI(TAG, "  rm vif 0 -> %d", mmprobe_rm_iface_raw(0));
    ESP_LOGI(TAG, "  rm vif 1 -> %d", mmprobe_rm_iface_raw(1));

    /* The probe matrix. Run each twice to confirm the verdict is stable
     * (add/remove leaves no residue). */
    for (int pass = 1; pass <= 2; pass++)
    {
        ESP_LOGW(TAG, "--- interface-type probe matrix (pass %d) ---", pass);
        probe_type("STA",   IF_STA);
        probe_type("AP",    IF_AP);
        probe_type("ADHOC", IF_ADHOC);
        probe_type("MESH",  IF_MESH);
        probe_type("BOGUS", IF_BOGUS);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    ESP_LOGW(TAG, "=== P0 done. Read the MESH(5) row vs the STA/AP/ADHOC rows "
                  "(known-good) and the BOGUS(99) row (known-bad). ===");
    ESP_LOGW(TAG, "=== MESH accepted -> P1 unblocked;  MESH == BOGUS -> mesh is "
                  "firmware-gated. ===");
}
