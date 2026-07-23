idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES halow nvs_flash esp_wifi esp_netif esp_event lwip
)
# S5c test: L2_DST_MAC=<mesh node MAC> makes the STA also ping a static-ARP'd fake IP whose MAC is a
# mesh node, so the frame's L2 dst is the mesh node (the gate proxies it into the mesh, not L3-forward).
if(DEFINED TEST_L2_DST_MAC)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "TEST_L2_DST_MAC=\"${TEST_L2_DST_MAC}\"")
endif()
# B2 test: NO_PING=1 makes the STA a responder-only (associate + static IP, no traffic of its own) so a
# mesh node must INITIATE to it, forcing the gate's mesh->AP broadcast bridging.
if(DEFINED TEST_NO_PING)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "TEST_NO_PING=1")
endif()
