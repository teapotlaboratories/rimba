idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "." "../../test-common/include"
    REQUIRES halow nvs_flash esp_wifi esp_netif esp_event lwip
)

# AP_VIF=1 brings an AP vif up alongside the mesh before the restart, reproducing the shipped gate's
# mesh+AP shape. EXPECTED TO PANIC on the S3 assert until S3 lands -- that is the point of the arm.
# Tests the VALUE, not merely DEFINED, so AP_VIF=0 does not silently enable it.
#
# ⚠ TEST_AP_VIF is a CMake CACHE variable, so the arm is STICKY. After one `make build ... AP_VIF=1`,
# the cache under build/<app>/<board>/ keeps TEST_AP_VIF=1, and a later plain `make build APP=...`
# (no AP_VIF -- exactly what the T0 matrix runs) passes no -D and inherits it, silently producing the
# deliberately-asserting binary. To go back, pass AP_VIF=0 explicitly or remove the build directory;
# omitting the variable is NOT enough. The T2 orchestrator already drops CMakeCache.txt when a
# fixture's build vars change (t2_onair.py _flash), but a bare `make build` does not.
if(TEST_AP_VIF)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "TEST_AP_VIF=1")
endif()
