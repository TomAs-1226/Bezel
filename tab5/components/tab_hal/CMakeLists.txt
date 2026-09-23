idf_component_register(SRCS "src/hal_tab5.c" "src/hal_tab5_net.c" INCLUDE_DIRS "include"
                       REQUIRES bezel
                       PRIV_REQUIRES esp_lcd esp_driver_ppa esp_driver_jpeg esp_driver_i2c esp_driver_i2s
                                     esp_driver_twai esp_driver_tsens esp_driver_gpio esp_timer esp_psram
                                     nvs_flash fatfs esp_wifi esp_netif esp_event vfs lwip esp_http_client
                                     mbedtls)
# The BSP builds each panel's DPI config inside bsp_display_new_with_handles(); hal_tab5.c retimes it to
# ~60 Hz in __wrap_esp_lcd_new_panel_dpi(), which every call to esp_lcd_new_panel_dpi() reaches through this
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_lcd_new_panel_dpi")
