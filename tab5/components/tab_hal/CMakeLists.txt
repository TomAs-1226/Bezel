idf_component_register(SRCS "src/hal_tab5.c" INCLUDE_DIRS "include"
                       REQUIRES bezel
                       PRIV_REQUIRES esp_lcd esp_driver_ppa esp_driver_jpeg esp_driver_i2c esp_driver_i2s
                                     esp_driver_twai esp_driver_tsens esp_driver_gpio esp_timer esp_psram
                                     nvs_flash fatfs esp_wifi esp_netif esp_event vfs)
