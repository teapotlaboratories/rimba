# Depends on the published Morse Micro component. `halow` is the component name
# provided by the morsemicro/halow registry package (installs as
# managed_components/morsemicro__halow).
idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES halow nvs_flash esp_wifi
)
