idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES halow nvs_flash esp_wifi esp_netif esp_event lwip
)

# AP_OPEN=1 runs the SoftAP with no security. FOR ON-AIR CAPTURE ONLY: the AP link is normally
# SAE/PMF, so data frames are CCMP-encrypted and a monitor capture cannot decode the payload. The
# frames under test (proxy-ARP replies) are unaffected by the link cipher -- the ARP sits in the
# SNAP payload either way -- so dropping encryption exposes the bytes without changing them.
#
# Tests the VALUE, not merely DEFINED: the Makefile forwards -D TEST_AP_OPEN=$(AP_OPEN), and make
# treats "0" as a non-empty string, so `AP_OPEN=0` still arrives defined. Under `if(DEFINED ...)` the
# off switch therefore turned security OFF. Other TEST_* flags here only skip a ping when spelled that
# way; this one drops link encryption, so it does not get to fail in the unsafe direction.
if(TEST_AP_OPEN)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "TEST_AP_OPEN=1")
endif()
