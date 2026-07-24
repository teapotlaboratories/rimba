idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "." "../../test-common/include"
    REQUIRES halow nvs_flash esp_wifi esp_netif esp_event lwip
)
# STA_IP=<a.b.c.d> pins a static IP; NO_PING=1 makes it a SILENT static responder (Case D support). Omit
# both for the default DHCP pinger reporter (Case C).
if(DEFINED TEST_STATIC_IP)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "TEST_STATIC_IP=\"${TEST_STATIC_IP}\"")
endif()
if(DEFINED TEST_NO_PING)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "TEST_NO_PING=1")
endif()
