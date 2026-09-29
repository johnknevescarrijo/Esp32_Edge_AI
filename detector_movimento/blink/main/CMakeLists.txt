idf_component_register(SRCS "blink.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp32-camera esp_timer nvs_flash esp_driver_gpio usb_device_uvc)
