idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "." "../../test-common/include"
    REQUIRES halow nvs_flash esp_wifi esp_netif esp_event lwip
)

# HW_RESTART=1 adds the S3 stage-1b arm: force a chip restart on a LIVE gateway (mesh + AP + a real
# AP-side datapath) and score what the mesh-only reproducer could not -- the AP's group-key and
# per-STA restore. Off by default, so the T2 mesh-ap scenario is untouched.
# Tests the VALUE, not merely DEFINED, so HW_RESTART=0 does not silently enable it.
#
# ⚠ TEST_HW_RESTART is a CMake CACHE variable, so the arm is STICKY -- the same trap documented at
# test-mesh-hwrestart's gate. After one `make build ... HW_RESTART=1`, a later plain `make build`
# (no HW_RESTART -- what the T0 matrix runs) passes no -D and inherits it, silently producing a
# fixture that restarts the chip in the middle of a T2 run. Pass HW_RESTART=0 explicitly or remove
# build/<app>/<board>/; omitting the variable is NOT enough.
if(TEST_HW_RESTART)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "TEST_HW_RESTART=1")
endif()
