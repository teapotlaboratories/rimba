idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES halow nvs_flash esp_wifi esp_netif esp_event lwip
)

# Optional: `make ... GATE=1` makes this node a PROACTIVE_RANN gate (for the S4b send_to_gates test —
# the node under test needs a gate to learn). Absent, it is a plain relay/receiver.
if(DEFINED MESH_GATE_MODE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "MESH_GATE_MODE=${MESH_GATE_MODE}")
endif()
# MESH_ID=rimba-mesh to peer with the gate app (S5c mesh-dest role).
if(DEFINED TEST_MESH_ID)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "TEST_MESH_ID=\"${TEST_MESH_ID}\"")
endif()
