idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES halow nvs_flash esp_wifi esp_netif esp_event lwip
)
# Role MACs are build-time overridable (defaults board0/1/2 in app_main.c).
foreach(v TEST_NODE_MAC TEST_RELAY_MAC TEST_GATE_MAC)
    if(DEFINED ${v})
        target_compile_definitions(${COMPONENT_LIB} PRIVATE "${v}=\"${${v}}\"")
    endif()
endforeach()
