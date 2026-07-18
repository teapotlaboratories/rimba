idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "." "../../test-common/include"
    # halow pulls in morselib, which is where esp_mesh_ccm_selftest() lives (ccmp.c:141).
    # esp_timer supplies the elapsed-time measurement.
    REQUIRES halow esp_timer
)
