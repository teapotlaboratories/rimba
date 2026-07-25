idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "." "../../test-common/include"
    REQUIRES halow nvs_flash esp_wifi esp_netif esp_event lwip
)

# The Linux gate's MAC is a BUILD-TIME argument (not hardcoded in the source). It arrives as the CMake
# cache var TEST_LINUX_MAC (the Makefile threads `LINUX_MAC=` into it; the harness passes it from the
# manifest LINUX_NODES registry or a --linux-mac override). Absent, app_main falls back to its #ifndef
# default (chronite). See app_main.c.
if(DEFINED TEST_LINUX_MAC)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "TEST_LINUX_MAC=\"${TEST_LINUX_MAC}\"")
endif()
