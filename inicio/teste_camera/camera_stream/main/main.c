#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "usb_device_uvc.h"

// ========================== DEFINIÇÃO DOS PINOS DA CÂMERA ==========================
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

static const char *TAG = "UVC_WEBCAM";

#define UVC_MAX_FRAMESIZE_SIZE (35 * 1024)

static uvc_fb_t s_uvc_fb;
static camera_fb_t *s_cam_fb = NULL;

// -------------------------------------------------------------------------
// Callbacks do dispositivo USB UVC
// -------------------------------------------------------------------------
static void camera_stop_cb(void *cb_ctx) {
    ESP_LOGI(TAG, "UVC: Streaming pausado/encerrado pelo computador");
}

static esp_err_t camera_start_cb(uvc_format_t format, int width, int height, int rate, void *cb_ctx) {
    ESP_LOGI(TAG, "UVC: Streaming iniciado pelo computador (%dx%d @ %d fps)", width, height, rate);
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        if (width <= 320 && height <= 240) {
            s->set_framesize(s, FRAMESIZE_QVGA);
        } else if (width <= 640 && height <= 480) {
            s->set_framesize(s, FRAMESIZE_VGA);
        }
    }
    return ESP_OK;
}

static uvc_fb_t *camera_fb_get_cb(void *cb_ctx) {
    s_cam_fb = esp_camera_fb_get();
    if (!s_cam_fb) {
        ESP_LOGE(TAG, "Falha ao capturar frame para UVC");
        return NULL;
    }

    uint64_t us = (uint64_t)esp_timer_get_time();
    s_uvc_fb.buf = s_cam_fb->buf;
    s_uvc_fb.len = s_cam_fb->len;
    s_uvc_fb.width = s_cam_fb->width;
    s_uvc_fb.height = s_cam_fb->height;
    s_uvc_fb.format = UVC_FORMAT_JPEG;
    s_uvc_fb.timestamp.tv_sec = us / 1000000UL;
    s_uvc_fb.timestamp.tv_usec = us % 1000000UL;

    if (s_uvc_fb.len > UVC_MAX_FRAMESIZE_SIZE) {
        ESP_LOGW(TAG, "Tamanho do frame (%zu) excedeu buffer (%d)", s_uvc_fb.len, UVC_MAX_FRAMESIZE_SIZE);
        esp_camera_fb_return(s_cam_fb);
        s_cam_fb = NULL;
        return NULL;
    }

    return &s_uvc_fb;
}

static void camera_fb_return_cb(uvc_fb_t *fb, void *cb_ctx) {
    if (s_cam_fb) {
        esp_camera_fb_return(s_cam_fb);
        s_cam_fb = NULL;
    }
}

// -------------------------------------------------------------------------
// Função para inicializar a câmera
// -------------------------------------------------------------------------
static esp_err_t init_camera(void) {
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
    config.xclk_freq_hz = 20000000;        // 20 MHz
    config.pixel_format = PIXFORMAT_JPEG;   // JPEG para UVC
    config.frame_size = FRAMESIZE_QVGA;     // 320x240
    config.jpeg_quality = 12;               // 0 (melhor) a 63 (pior)
    config.fb_count = 1;                    // 1 buffer na RAM interna (DRAM)
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

#if CONFIG_SPIRAM
    config.fb_location = CAMERA_FB_IN_PSRAM;
#else
    config.fb_location = CAMERA_FB_IN_DRAM;
#endif

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao inicializar a câmera: 0x%x (%s)", err, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Câmera OV3660 inicializada com sucesso!");
    return ESP_OK;
}

// -------------------------------------------------------------------------
// Função principal
// -------------------------------------------------------------------------
void app_main(void) {
    esp_err_t ret;

    // Inicializa NVS
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Inicializando câmera do ESP32-S3...");
    if (init_camera() != ESP_OK) {
        ESP_LOGE(TAG, "Falha crítica na câmera. Abortando.");
        return;
    }

    uint8_t *uvc_buffer = (uint8_t *)malloc(UVC_MAX_FRAMESIZE_SIZE);
    if (!uvc_buffer) {
        ESP_LOGE(TAG, "Falha ao alocar buffer UVC (%d bytes)", UVC_MAX_FRAMESIZE_SIZE);
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

    ESP_LOGI(TAG, "Configurando e iniciando dispositivo USB UVC (Webcam)...");
    ESP_ERROR_CHECK(uvc_device_config(0, &uvc_config));
    ESP_ERROR_CHECK(uvc_device_init());

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, " WebCam USB iniciada com sucesso!");
    ESP_LOGI(TAG, " O Linux vai reconhecer como /dev/videoX");
    ESP_LOGI(TAG, " Teste no terminal do seu PC com: ffplay /dev/video0");
    ESP_LOGI(TAG, "==================================================");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
