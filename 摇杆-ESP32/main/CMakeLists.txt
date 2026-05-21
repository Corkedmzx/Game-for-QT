idf_component_register(SRCS "main.c" "led_strip_encoder.c" "host_transport.c" "ble_nus_host.c"
                    PRIV_REQUIRES esp_driver_gpio esp_driver_rmt esp_adc esp_timer
                                  bt nvs_flash esp_driver_usb_serial_jtag
                    INCLUDE_DIRS ".")
