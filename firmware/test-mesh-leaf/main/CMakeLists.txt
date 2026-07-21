idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "." "../../test-common/include"
    REQUIRES halow nvs_flash esp_wifi esp_netif esp_event lwip
)

# The line-topology MACs (origin/dest/relay) are BUILD-TIME arguments, not source constants. They
# arrive as CMake cache vars TEST_MESH_*_MAC (the Makefile threads MESH_ORIGIN_MAC=/MESH_DEST_MAC=/
# MESH_RELAY_MAC= into them; the T2 harness passes all three from the manifest BENCH registry to every
# flash of this symmetric app). Absent, app_main falls back to its #ifndef defaults (board0/1/2).
foreach(v TEST_MESH_ORIGIN_MAC TEST_MESH_DEST_MAC TEST_MESH_RELAY_MAC)
    if(DEFINED ${v})
        target_compile_definitions(${COMPONENT_LIB} PRIVATE "${v}=\"${${v}}\"")
    endif()
endforeach()
