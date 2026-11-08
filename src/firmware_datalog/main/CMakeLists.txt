idf_component_register(SRCS "main.c"
                            "mpu6050.c"
                            "rgb_led.c"
                            "vibration.c"
                            "button.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_i2c esp_driver_ledc esp_driver_gpio
                                esp_timer esp_wifi esp_event nvs_flash esp_netif
                                lwip)
