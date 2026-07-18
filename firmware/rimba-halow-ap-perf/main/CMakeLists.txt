idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    # console + espressif__iperf-cmd + esp-qa__ping-cmd back the `iperf -s` / `ping` serial console.
    REQUIRES halow nvs_flash esp_wifi esp_netif esp_event lwip console espressif__iperf-cmd esp-qa__ping-cmd
)
