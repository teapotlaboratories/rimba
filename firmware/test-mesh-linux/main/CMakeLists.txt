idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "." "../../test-common/include"
    REQUIRES halow nvs_flash esp_wifi esp_netif esp_event lwip
)

# The Linux peer MAC/IP are BUILD-TIME arguments (not hardcoded in the source). They arrive as CMake
# cache vars TEST_LINUX_MAC / TEST_LINUX_IP (the Makefile threads `LINUX_MAC=`/`LINUX_IP=` into
# them; the harness passes them from the manifest registry or a --linux-mac/--linux-ip override).
# Absent, app_main falls back to its #ifndef defaults (chronite). See app_main.c.
if(DEFINED TEST_LINUX_MAC)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "TEST_LINUX_MAC=\"${TEST_LINUX_MAC}\"")
endif()
if(DEFINED TEST_LINUX_IP)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "TEST_LINUX_IP=\"${TEST_LINUX_IP}\"")
endif()
