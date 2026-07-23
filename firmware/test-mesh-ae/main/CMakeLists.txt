idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES halow nvs_flash esp_wifi esp_netif esp_event lwip
)

# The AE-frame target (mesh DA) is a BUILD-TIME argument: the Makefile threads `LINUX_MAC=` into the
# CMake cache var TEST_LINUX_MAC. Absent, app_main falls back to its #ifndef default (chronite). This
# lets the same fixture target a Linux node OR another ESP (e.g. LINUX_MAC=<board1 mesh MAC>).
if(DEFINED TEST_LINUX_MAC)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "TEST_LINUX_MAC=\"${TEST_LINUX_MAC}\"")
endif()
# S5b: MESH_ID=rimba-mesh EADDR1=<gate AP client MAC> point this node at the gate app as a proxied-frame source.
if(DEFINED TEST_MESH_ID)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "TEST_MESH_ID=\"${TEST_MESH_ID}\"")
endif()
if(DEFINED TEST_EADDR1)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "TEST_EADDR1=\"${TEST_EADDR1}\"")
endif()
