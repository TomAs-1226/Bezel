idf_component_register(SRCS "src/hal_tab5.c" "src/hal_tab5_net.c" "src/hal_tab5_dev.c" "src/hal_tab5_os.c"
                       INCLUDE_DIRS "include"
                       REQUIRES bezel
                       PRIV_REQUIRES esp_lcd esp_mm esp_driver_ppa esp_driver_jpeg esp_driver_i2c esp_driver_i2s
                                     esp_driver_twai esp_driver_tsens esp_driver_usb_serial_jtag esp_driver_gpio esp_timer esp_psram
                                     nvs_flash fatfs espcoredump esp_wifi esp_netif esp_event vfs lwip esp_http_client
                                     mbedtls)
# esp_lcd's DMA2D frame-buffer copier (esp_async_fbcpy.h) is private to it: the page slide uses it directly
target_include_directories(${COMPONENT_LIB} PRIVATE "${IDF_PATH}/components/esp_lcd/priv_include")
# Core 1 draws: an unpinned task above priority 9 (esp-hosted's SDIO tasks at 23, lwIP at 18) is kept on
# core 0 (__wrap_xTaskCreatePinnedToCore in hal_tab5.c), where it can't stall a frame
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=xTaskCreatePinnedToCore")
