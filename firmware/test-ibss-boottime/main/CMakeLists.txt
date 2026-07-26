idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES halow nvs_flash esp_wifi esp_netif esp_event lwip esp_timer
)

# IBSS_CREATE=1 builds the peer/creator (board0); default builds the MEASURE DUT (board2).
if(DEFINED TEST_IBSS_CREATE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "TEST_IBSS_CREATE=1")
endif()
# PING_IP= the creator's IP the DUT pings (its first reply is the "first frame"). Same
# make flag every other pinging fixture uses.
if(DEFINED TEST_PING_IP)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "TEST_PING_IP=\"${TEST_PING_IP}\"")
endif()
