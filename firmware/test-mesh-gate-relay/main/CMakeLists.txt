idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "." "../../test-common/include"
    REQUIRES halow nvs_flash esp_wifi esp_netif esp_event lwip
)
# Role MACs are build-time overridable (defaults board0/1/2 in app_main.c). Uses the harness-threaded
# MESH_*_MAC names (Makefile) so the T2 orchestrator's build_mac_var feeds each board's mesh_mac.
foreach(v TEST_MESH_ORIGIN_MAC TEST_MESH_RELAY_MAC TEST_MESH_DEST_MAC)
    if(DEFINED ${v})
        target_compile_definitions(${COMPONENT_LIB} PRIVATE "${v}=\"${${v}}\"")
    endif()
endforeach()
