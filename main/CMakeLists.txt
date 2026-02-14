idf_component_register(
    SRCS "app_main.c" "display_ui.c" "lcd_st7789.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_lcd esp_lvgl_port
    PRIV_REQUIRES esp_timer lwip esp_http_server esp_http_client
                  esp_wifi esp_netif esp_event nvs_flash
                  esp-tls mbedtls
)