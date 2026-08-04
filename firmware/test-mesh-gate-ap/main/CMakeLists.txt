idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES halow nvs_flash esp_wifi esp_netif esp_event lwip
)

# AP_OPEN=1 runs the SoftAP with no security. FOR ON-AIR CAPTURE ONLY: the AP link is normally
# SAE/PMF, so data frames are CCMP-encrypted and a monitor capture cannot decode the payload. The
# frames under test (proxy-ARP replies) are unaffected by the link cipher -- the ARP sits in the
# SNAP payload either way -- so dropping encryption exposes the bytes without changing them.
if(DEFINED TEST_AP_OPEN)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "TEST_AP_OPEN=1")
endif()
