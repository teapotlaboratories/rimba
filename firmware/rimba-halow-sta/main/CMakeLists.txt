idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES halow nvs_flash esp_wifi esp_netif esp_event lwip
)
# STA_IP=<a.b.c.d> pins a STATIC IP (for an AP with no DHCP server, e.g. the plain rimba-halow-ap demo, or
# a fixed-address test). Omit it to be a DHCP client — the default, against the mesh-gate's flat 10.9.9.x
# SoftAP (zero-config: lease an address and reach any mesh node directly).
if(DEFINED TEST_STATIC_IP)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "TEST_STATIC_IP=\"${TEST_STATIC_IP}\"")
endif()
# PING_IP=<a.b.c.d> overrides the ping target (default: mesh node D at 10.9.9.100).
if(DEFINED TEST_PING_IP)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "TEST_PING_IP=\"${TEST_PING_IP}\"")
endif()
# NO_PING=1 makes the STA a responder-only (associate + acquire an IP, no traffic of its own) so a mesh
# node must INITIATE to it, forcing the gate's mesh->AP broadcast bridging (B2). Pair it with STA_IP= for a
# FIXED responder address — the DHCP default gives a dynamic IP a mesh node can't target ahead of time.
if(DEFINED TEST_NO_PING)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "TEST_NO_PING=1")
endif()
