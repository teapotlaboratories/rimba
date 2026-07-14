idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    # console + espressif__iperf-cmd (+ espressif__iperf for the temp auto-blast iperf_start API) are
    # only used by the temporary MESH_IPERF throughput build.
    REQUIRES halow nvs_flash esp_wifi esp_netif esp_event lwip console espressif__iperf-cmd espressif__iperf esp-qa__ping-cmd
)
