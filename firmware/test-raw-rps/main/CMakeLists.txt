idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "." "../../test-common/include"
    REQUIRES halow nvs_flash esp_wifi esp_netif esp_event lwip
)
