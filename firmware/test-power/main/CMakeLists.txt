idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "." "../../test-common/include"
    REQUIRES halow nvs_flash esp_wifi esp_netif esp_event lwip esp_pm
)

# HOST_LIGHT_SLEEP is a BUILD-TIME arg (not a source constant): the Makefile threads
# HOST_LIGHT_SLEEP=1 -> -D TEST_HOST_LIGHT_SLEEP=1, which the tp --light-sleep variant uses to
# build the stronger secondary current gate. Absent -> app_main's #ifndef default (0 = host-awake).
if(DEFINED TEST_HOST_LIGHT_SLEEP)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "HOST_LIGHT_SLEEP=${TEST_HOST_LIGHT_SLEEP}")
endif()
