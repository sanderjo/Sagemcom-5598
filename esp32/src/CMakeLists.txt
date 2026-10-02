idf_component_register(SRCS "main.c" "activity.c" "matrix.c" "mesh.c" "tools.c" "mcp_proto.c" "mcp_http.c"
                            "wifi.c" "router.c" "sha512crypt.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_http_client esp_http_server
                                     esp_timer esp_psram mbedtls spi_flash)
