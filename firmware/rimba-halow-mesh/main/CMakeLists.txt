idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES halow nvs_flash esp_wifi esp_netif esp_event lwip
)
# Round-trip test: TEST_PEER_MAC=<off-mesh peer MAC> pins a static ARP (10.9.9.50 -> that MAC) so
# this node can reply to an AP client behind a mesh gate without a broadcast ARP into the mesh.
if(DEFINED TEST_PEER_MAC)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "TEST_PEER_MAC=\"${TEST_PEER_MAC}\"")
endif()
# B2 test: TEST_PING_IP=<off-mesh AP client IP> makes this mesh node originate a ping to that client,
# so its broadcast ARP request must be bridged to the AP by the gate (mesh->AP broadcast bridging).
if(DEFINED TEST_PING_IP)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "TEST_PING_IP=\"${TEST_PING_IP}\"")
endif()
