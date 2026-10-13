#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "CONTADOR_BIN";

// ==============================================================================
// CONFIGURAÇÕES
// ==============================================================================
// Tempo de espera entre cada contagem em milissegundos (ex: 2500 = 2.5 segundos)
#define TEMPO_DELAY_MS   2500

// Definição dos pinos GPIO (Bit 0 = LSB, Bit 3 = MSB)
#define PIN_A0     GPIO_NUM_5    // Bit 0 (Peso 1 - LSB)
#define PIN_A1     GPIO_NUM_6    // Bit 1 (Peso 2)
#define PIN_A2     GPIO_NUM_42   // Bit 2 (Peso 4) - 3º LED físico
#define PIN_A3     GPIO_NUM_40   // Bit 3 (Peso 8 - MSB) - 4º LED físico

// Vetor com os pinos na ordem dos bits (do bit 0 ao bit 3)
static const gpio_num_t leds[] = {PIN_A0, PIN_A1, PIN_A2, PIN_A3};
#define NUM_LEDS (sizeof(leds) / sizeof(leds[0]))

// ==============================================================================
// FUNÇÕES AUXILIARES
// ==============================================================================

// Inicializa os pinos usando gpio_config (método mais robusto do ESP-IDF)
static void init_gpios(void)
{
    uint64_t pin_mask = 0;
    for (int i = 0; i < NUM_LEDS; i++) {
        pin_mask |= (1ULL << leds[i]);
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao configurar GPIOs: %s", esp_err_to_name(err));
    }

    // Inicia com todos desligados
    for (int i = 0; i < NUM_LEDS; i++) {
        gpio_set_level(leds[i], 0);
    }
}

// Teste inicial: liga todos juntos e depois um por um para diagnóstico de hardware
static void teste_inicial_leds(void)
{
    ESP_LOGI(TAG, "==========================================================");
    ESP_LOGI(TAG, "[DIAGNOSTICO] 1. Ligando TODOS os 4 LEDs por 3 segundos...");
    ESP_LOGI(TAG, "Se nenhum acender agora, verifique o fio GND ou os pinos!");
    ESP_LOGI(TAG, "==========================================================");

    for (int i = 0; i < NUM_LEDS; i++) {
        gpio_set_level(leds[i], 1);
    }
    vTaskDelay(pdMS_TO_TICKS(3000));

    // Desliga todos
    for (int i = 0; i < NUM_LEDS; i++) {
        gpio_set_level(leds[i], 0);
    }
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_LOGI(TAG, "==========================================================");
    ESP_LOGI(TAG, "[DIAGNOSTICO] 2. Testando LED por LED (do Bit 0 ao Bit 3)...");
    ESP_LOGI(TAG, "==========================================================");

    for (int i = 0; i < NUM_LEDS; i++) {
        ESP_LOGI(TAG, ">> Ligando LED %d | Pino: GPIO_%d | Bit %d (Peso %d)",
                 i, leds[i], i, (1 << i));
        gpio_set_level(leds[i], 1);
        vTaskDelay(pdMS_TO_TICKS(1000));
        gpio_set_level(leds[i], 0);
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    ESP_LOGI(TAG, "==========================================================");
    ESP_LOGI(TAG, "Diagnostico finalizado! Iniciando contagem normal...");
    ESP_LOGI(TAG, "==========================================================\n");
    vTaskDelay(pdMS_TO_TICKS(1000));
}


// Atualiza o nível lógico de cada pino conforme o bit correspondente
static void atualizar_leds(uint8_t valor)
{
    for (int bit = 0; bit < NUM_LEDS; bit++) {
        int nivel = (valor >> bit) & 0x01;
        gpio_set_level(leds[bit], nivel);
    }
}

// Exibe um painel visual formatado no terminal para facilitar a leitura
static void exibir_painel_visual(uint8_t valor)
{
    int b3 = (valor >> 3) & 1;
    int b2 = (valor >> 2) & 1;
    int b1 = (valor >> 1) & 1;
    int b0 = (valor >> 0) & 1;

    printf("\n+-------------------------------------------------------------+\n");
    printf("| CONTADOR BINARIO: %2d / 15  (Hex: 0x%X)                      |\n", valor, valor);
    printf("+-------------------------------------------------------------+\n");
    printf("| Pesos dos Bits    : [ 8 ]   [ 4 ]   [ 2 ]   [ 1 ]           |\n");
    printf("| Bits (MSB -> LSB) : [ %d ]   [ %d ]   [ %d ]   [ %d ]           |\n", b3, b2, b1, b0);
    printf("| Calculo da Soma   : (%d*8) + (%d*4) + (%d*2) + (%d*1) = %-2d      |\n",
           b3, b2, b1, b0, valor);
    printf("| Estado dos LEDs   : [%s]  [%s]  [%s]  [%s]        |\n",
           b3 ? "● ON " : "○ OFF",
           b2 ? "● ON " : "○ OFF",
           b1 ? "● ON " : "○ OFF",
           b0 ? "● ON " : "○ OFF");
    printf("| Pinos GPIO        : [G%02d]  [G%02d]  [ G%d]  [ G%d]           |\n",
           leds[3], leds[2], leds[1], leds[0]);
    printf("+-------------------------------------------------------------+\n");
}

// ==============================================================================
// PONTO DE ENTRADA PRINCIPAL
// ==============================================================================
void app_main(void)
{
    ESP_LOGI(TAG, "=== Sistema Iniciado ===");
    ESP_LOGI(TAG, "Delay configurado para %d ms (%.1f s)", TEMPO_DELAY_MS, TEMPO_DELAY_MS / 1000.0);

    // 1. Configura os pinos de saída
    init_gpios();

    // 2. Faz o teste visual de cada pino (um a um)
    teste_inicial_leds();

    uint8_t contador = 0;
    const uint8_t max_valor = (1 << NUM_LEDS); // 16 (0 a 15)

    // 3. Loop principal
    while (1) {
        // Atualiza os LEDs físicos na placa
        atualizar_leds(contador);

        // Imprime a visualização detalhada no terminal
        exibir_painel_visual(contador);

        // Próximo valor (ao passar de 15, volta para 0)
        contador = (contador + 1) % max_valor;

        // Pausa configurável liberando a CPU para o FreeRTOS
        vTaskDelay(pdMS_TO_TICKS(TEMPO_DELAY_MS));
    }
}


