idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "." "../../test-common/include"
    REQUIRES halow nvs_flash esp_wifi esp_netif esp_event lwip
)

# AP_VIF=1 brings an AP vif up alongside the mesh before the restart, reproducing the shipped gate's
# mesh+AP shape. EXPECTED TO PANIC on the S3 assert until S3 lands -- that is the point of the arm.
# Tests the VALUE, not merely DEFINED, so AP_VIF=0 does not silently enable it.
if(TEST_AP_VIF)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "TEST_AP_VIF=1")
endif()
