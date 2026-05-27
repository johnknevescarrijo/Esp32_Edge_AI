#include <iostream>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#tentiva de acender o led

#define LED_PIN GPIO_NUM_15

extern "C" void app_main(void){

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    gpio_config(&io_conf);

    while(1){
        gpio_set_level(LED_PIN,1);
        vTaskDelay(2000/portTICK_PERIOD_MS);
        gpio_set_level(LED_PIN,0);
        vTaskDelay(2000/portTICK_PERIOD_MS);
    }
}