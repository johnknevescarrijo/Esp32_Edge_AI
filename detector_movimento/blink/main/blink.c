#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "img_converters.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "usb_device_uvc.h"

static const char *TAG = "DETECTOR_UVC";

// ========================== CONFIGURAÇÃO DO LED ==========================
#define LED_PIN             GPIO_NUM_38
#define LED_ACTIVE_LEVEL    1       // 1 = Ativo em nível Alto (3.3V). Mude para 0 se o LED for ativo baixo (GND).
#define LED_ON              (LED_ACTIVE_LEVEL)
#define LED_OFF             (!LED_ACTIVE_LEVEL)

// ========================== PINOS DA CÂMERA (OV3660) ==========================
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     15
#define SIOD_GPIO_NUM     4    // I2C SDA
#define SIOC_GPIO_NUM     5    // I2C SCL

#define Y9_GPIO_NUM       16   // D0
#define Y8_GPIO_NUM       17   // D1
#define Y7_GPIO_NUM       18   // D2
#define Y6_GPIO_NUM       12   // D3
#define Y5_GPIO_NUM       10   // D4
#define Y4_GPIO_NUM       8    // D5
#define Y3_GPIO_NUM       9    // D6
#define Y2_GPIO_NUM       11   // D7

#define VSYNC_GPIO_NUM    6
#define HREF_GPIO_NUM     7
#define PCLK_GPIO_NUM     13

// ========================== PARÂMETROS DE DETECÇÃO ==========================
#define UVC_MAX_FRAMESIZE_SIZE  (40 * 1024)
#define MOTION_PERCENT_THRESH   6.0f    // Porcentagem de pixels alterados para disparar movimento
#define HOLD_TIME_MS            2000    // Tempo (ms) que o LED fica aceso após o movimento cessar
#define WARMUP_FRAMES           25      // Frames ignorados no início para estabilização da câmera

// Buffer reduzido 1/8 de QVGA (320x240 -> 40x30 = 1200 pixels)
#define SAMPLE_W 40
#define SAMPLE_H 30
#define SAMPLE_PIXELS (SAMPLE_W * SAMPLE_H)

static uint16_t s_prev_rgb[SAMPLE_PIXELS];
static uint16_t s_curr_rgb[SAMPLE_PIXELS];
static bool s_has_prev_frame = false;
static int s_warmup_counter = 0;
static int64_t s_last_motion_time = 0;
static bool s_led_is_on = false;

static uvc_fb_t s_uvc_fb;
static camera_fb_t *s_cam_fb = NULL;
static bool s_uvc_streaming = false;

static void set_led_state(bool on)
{
    s_led_is_on = on;
    gpio_set_level(LED_PIN, on ? LED_ON : LED_OFF);
}

static void init_led(void)
{
    // Configura GPIO 38 com leitura e escrita
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_PIN),
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    // Teste visual de inicialização: pisca 3 vezes para confirmar que o circuito funciona
    ESP_LOGI(TAG, "Testando LED no GPIO %d (pisca 3x)...", (int)LED_PIN);
    for (int i = 0; i < 3; i++) {
        set_led_state(true);
        vTaskDelay(pdMS_TO_TICKS(150));
        set_led_state(false);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    ESP_LOGI(TAG, "Teste de LED concluido.");
}

static void process_motion(camera_fb_t *fb)
{
    if (!fb || fb->len == 0) return;

    if (s_warmup_counter < WARMUP_FRAMES) {
        s_warmup_counter++;
        return;
    }

    // Decodifica miniatura 40x30 do JPEG para cálculo ultrarrápido (< 2ms)
    if (!jpg2rgb565(fb->buf, fb->len, (uint8_t *)s_curr_rgb, JPEG_IMAGE_SCALE_1_8)) {
        return;
    }

    if (!s_has_prev_frame) {
        memcpy(s_prev_rgb, s_curr_rgb, sizeof(s_prev_rgb));
        s_has_prev_frame = true;
        return;
    }

    int changed_pixels = 0;
    for (int i = 0; i < SAMPLE_PIXELS; i++) {
        uint16_t c = s_curr_rgb[i];
        uint16_t p = s_prev_rgb[i];

        int r_diff = abs(((c >> 11) & 0x1F) - ((p >> 11) & 0x1F));
        int g_diff = abs(((c >> 5) & 0x3F) - ((p >> 5) & 0x3F));
        int b_diff = abs((c & 0x1F) - (p & 0x1F));

        if ((r_diff + g_diff + b_diff) > 7) {
            changed_pixels++;
        }
    }

    float motion_percent = ((float)changed_pixels / (float)SAMPLE_PIXELS) * 100.0f;
    int64_t now = esp_timer_get_time();

    if (motion_percent >= MOTION_PERCENT_THRESH) {
        s_last_motion_time = now;
        if (!s_led_is_on) {
            set_led_state(true);
            ESP_LOGI(TAG, "[!] MOVIMENTO DETECTADO: %.1f%% alterado -> LED ACESO", motion_percent);
        }
    } else {
        if (s_led_is_on && ((now - s_last_motion_time) > (HOLD_TIME_MS * 1000LL))) {
            set_led_state(false);
            ESP_LOGI(TAG, "[-] Movimento cessou -> LED APAGADO");
        }
    }

    memcpy(s_prev_rgb, s_curr_rgb, sizeof(s_prev_rgb));
}

// -------------------------------------------------------------------------
// Callbacks do USB UVC (Webcam USB)
// -------------------------------------------------------------------------
static void camera_stop_cb(void *cb_ctx)
{
    ESP_LOGI(TAG, "UVC: Streaming encerrado/pausado pelo navegador/PC");
    s_uvc_streaming = false;
}

static esp_err_t camera_start_cb(uvc_format_t format, int width, int height, int rate, void *cb_ctx)
{
    ESP_LOGI(TAG, "UVC: Streaming iniciado pelo Firefox/PC (%dx%d @ %d fps)", width, height, rate);
    s_uvc_streaming = true;
    return ESP_OK;
}

static uvc_fb_t *camera_fb_get_cb(void *cb_ctx)
{
    s_cam_fb = esp_camera_fb_get();
    if (!s_cam_fb) {
        return NULL;
    }

    // Processa detecção de movimento em tempo real
    process_motion(s_cam_fb);

    uint64_t us = (uint64_t)esp_timer_get_time();
    s_uvc_fb.buf = s_cam_fb->buf;
    s_uvc_fb.len = s_cam_fb->len;
    s_uvc_fb.width = s_cam_fb->width;
    s_uvc_fb.height = s_cam_fb->height;
    s_uvc_fb.format = UVC_FORMAT_JPEG;
    s_uvc_fb.timestamp.tv_sec = us / 1000000UL;
    s_uvc_fb.timestamp.tv_usec = us % 1000000UL;

    if (s_uvc_fb.len > UVC_MAX_FRAMESIZE_SIZE) {
        esp_camera_fb_return(s_cam_fb);
        s_cam_fb = NULL;
        return NULL;
    }

    return &s_uvc_fb;
}

static void camera_fb_return_cb(uvc_fb_t *fb, void *cb_ctx)
{
    if (s_cam_fb) {
        esp_camera_fb_return(s_cam_fb);
        s_cam_fb = NULL;
    }
}

// -------------------------------------------------------------------------
// Inicialização da Câmera OV3660
// -------------------------------------------------------------------------
static esp_err_t init_camera(void)
{
    camera_config_t config = {0};
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;   // Formato JPEG para transmissão UVC
    config.frame_size = FRAMESIZE_QVGA;     // 320x240
    config.jpeg_quality = 12;
    config.fb_count = 2;                    // 2 buffers para streaming fluido
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    config.fb_location = CAMERA_FB_IN_DRAM;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao inicializar camera: 0x%x (%s)", err, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Camera OV3660 inicializada com sucesso!");
    return ESP_OK;
}

// Tarefa que mantém o detector de movimento funcionando mesmo se o Firefox não estiver aberto
static void standalone_motion_task(void *pvParameters)
{
    while (1) {
        if (!s_uvc_streaming) {
            camera_fb_t *fb = esp_camera_fb_get();
            if (fb) {
                process_motion(fb);
                esp_camera_fb_return(fb);
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        } else {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    init_led();

    ESP_LOGI(TAG, "Inicializando camera...");
    if (init_camera() != ESP_OK) {
        ESP_LOGE(TAG, "Erro ao inicializar camera. Abortando.");
        return;
    }

    uint8_t *uvc_buffer = (uint8_t *)malloc(UVC_MAX_FRAMESIZE_SIZE);
    if (!uvc_buffer) {
        ESP_LOGE(TAG, "Falha ao alocar buffer UVC");
        return;
    }

    uvc_device_config_t uvc_config = {
        .uvc_buffer = uvc_buffer,
        .uvc_buffer_size = UVC_MAX_FRAMESIZE_SIZE,
        .start_cb = camera_start_cb,
        .fb_get_cb = camera_fb_get_cb,
        .fb_return_cb = camera_fb_return_cb,
        .stop_cb = camera_stop_cb,
        .cb_ctx = NULL,
    };

    ESP_LOGI(TAG, "Iniciando dispositivo USB UVC (Webcam)...");
    ESP_ERROR_CHECK(uvc_device_config(0, &uvc_config));
    ESP_ERROR_CHECK(uvc_device_init());

    xTaskCreate(standalone_motion_task, "motion_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, " Webcam USB + Detector de Movimento Ativos!");
    ESP_LOGI(TAG, " 1. Abra o arquivo 'webcam_viewer.html' no Firefox");
    ESP_LOGI(TAG, " 2. O LED no GPIO 38 acende quando houver movimento");
    ESP_LOGI(TAG, "==================================================");
}
