idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "." "../../test-common/include"
    REQUIRES halow nvs_flash esp_wifi esp_netif esp_event lwip
)
# NO_PING=1 makes it a plain static mesh RESPONDER (support role, Case C) instead of the pinger reporter.
if(DEFINED TEST_NO_PING)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "TEST_NO_PING=1")
endif()
