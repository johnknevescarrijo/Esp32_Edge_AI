#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "FAROL";

// Definição dos pinos GPIO para o semáforo (farol)
// Ajuste a ordem das cores conforme a sua ligação física:
#define PIN_VERDE     GPIO_NUM_38
#define PIN_AMARELO   GPIO_NUM_40
#define PIN_VERMELHO  GPIO_NUM_42

// Tempos de cada fase em milissegundos
#define TEMPO_VERDE_MS     5000  // 5 segundos
#define TEMPO_AMARELO_MS   2000  // 2 segundos
#define TEMPO_VERMELHO_MS  5000  // 5 segundos

// Função auxiliar para definir o estado dos 3 LEDs de uma vez
static void set_farol(int verde, int amarelo, int vermelho)
{
    gpio_set_level(PIN_VERDE, verde);
    gpio_set_level(PIN_AMARELO, amarelo);
    gpio_set_level(PIN_VERMELHO, vermelho);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Inicializando controle do farol...");

    // Configuração dos pinos como saída
    // ATENÇÃO: É fundamental usar '1ULL' (64 bits) pois GPIO 38, 40 e 42 são maiores que 31
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_VERDE) | (1ULL << PIN_AMARELO) | (1ULL << PIN_VERMELHO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    // Garante que todos começam apagados
    set_farol(0, 0, 0);

    ESP_LOGI(TAG, "Ciclo do semáforo iniciado!");

    while (1) {
        // 1. Fase VERDE: Siga em frente
        ESP_LOGI(TAG, "[ VERDE ] - Sinal aberto");
        set_farol(1, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(TEMPO_VERDE_MS));

        // 2. Fase AMARELO: Atenção
        ESP_LOGI(TAG, "[ AMARELO ] - Atenção");
        set_farol(0, 1, 0);
        vTaskDelay(pdMS_TO_TICKS(TEMPO_AMARELO_MS));

        // 3. Fase VERMELHO: Pare
        ESP_LOGI(TAG, "[ VERMELHO ] - Pare");
        set_farol(0, 0, 1);
        vTaskDelay(pdMS_TO_TICKS(TEMPO_VERMELHO_MS));
    }
}
