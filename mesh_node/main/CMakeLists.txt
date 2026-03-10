idf_component_register(
    SRCS
        "main.c"
        "mesh_protocol.c"
        "dedup_cache.c"
        "neighbor_table.c"
        "routing_table.c"
        "oled.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        esp_timer
        driver
)