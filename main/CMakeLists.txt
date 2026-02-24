idf_component_register(
    SRCS "main.c"  
    SRCS "ap.c"
    SRCS "midi.c"
    SRCS "sta.c"
    SRCS "http_client.c"
    SRCS "store.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        usb esp_wifi esp_event esp_netif esp_http_client
        nvs_flash esp_http_server esp_timer esp_driver_gpio lwip
)