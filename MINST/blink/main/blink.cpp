#include <stdio.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "model_data.h"

static const char *TAG = "MNIST_INFERENCE";

// Tensor arena. Para este modelo (784 -> 128 -> 10) 50 KB sao suficientes.
constexpr int kTensorArenaSize = 50 * 1024;
uint8_t tensor_arena[kTensorArenaSize];

// Parametros de quantizacao extraidos do model_quantized.tflite.
// Para obter: rode no PC interpreter.get_input_details()[0]['quantization']
constexpr float  kInputScale       = 0.012728233821690083f;
constexpr int32_t kInputZeroPoint  = -95;
constexpr float  kOutputScale      = 0.18673887848854065f;
constexpr int32_t kOutputZeroPoint = -19;

extern "C" void app_main(void)
{
    // Mapeia o modelo a partir do array em model_data.h
    const tflite::Model *model = tflite::GetModel(model_quantized_tflite);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "Versao do modelo incompativel. Esperada=%d, atual=%d",
                 TFLITE_SCHEMA_VERSION, model->version());
        return;
    }

    // Registra APENAS os operadores que o modelo usa:
    // SHAPE, STRIDED_SLICE, PACK, RESHAPE, FULLY_CONNECTED (com ReLU fundido)
    static tflite::MicroMutableOpResolver<5> resolver;
    resolver.AddShape();
    resolver.AddStridedSlice();
    resolver.AddPack();
    resolver.AddReshape();
    resolver.AddFullyConnected();

    // Cria o interpretador (a API nova nao usa mais ErrorReporter)
    tflite::MicroInterpreter interpreter(model, resolver, tensor_arena,
                                         kTensorArenaSize);

    if (interpreter.AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "Falha ao alocar tensores (arena pequena?)");
        return;
    }

    TfLiteTensor *input = interpreter.input(0);
    if (input == nullptr) {
        ESP_LOGE(TAG, "Tensor de entrada nulo");
        return;
    }

    // Valida o tipo e shape da entrada
    if (input->type != kTfLiteInt8) {
        ESP_LOGE(TAG, "Tipo de entrada inesperado: %d (esperado int8)",
                 input->type);
        return;
    }
    ESP_LOGI(TAG, "Input shape: [%d, %d, %d, %d]",
             input->dims->data[0], input->dims->data[1],
             input->dims->data[2], input->dims->data[3]);

    // --- Preenchimento do tensor de entrada (INT8) ---
    // Os pixels normalizados valem aproximadamente de -0.4241 a 2.8210
    // (transform Normalize(0.1307, 0.3081)). Para converter para int8:
    //     q = round(x / scale) + zero_point
    //
    // Aqui preenchemos com zeros (imagem "preta") apenas como placeholder.
    // Substitua por seus pixels reais 28x28 normalizados.
    int8_t *input_data = input->data.int8;
    const float pixel_normalizado = 0.0f; // TODO: pixels reais
    for (int i = 0; i < 784; i++) {
        float q = (pixel_normalizado / kInputScale) + (float)kInputZeroPoint;
        int32_t qi = (int32_t)llroundf(q);
        if (qi < -128) qi = -128;        // clamp int8
        else if (qi > 127) qi = 127;
        input_data[i] = (int8_t)qi;
    }

    // Executa a inferencia
    if (interpreter.Invoke() != kTfLiteOk) {
        ESP_LOGE(TAG, "Falha na inferencia");
        return;
    }

    TfLiteTensor *output = interpreter.output(0);
    if (output == nullptr) {
        ESP_LOGE(TAG, "Tensor de saida nulo");
        return;
    }
    if (output->type != kTfLiteInt8) {
        ESP_LOGE(TAG, "Tipo de saida inesperado: %d (esperado int8)", output->type);
        return;
    }

    // Desquantiza a saida e encontra a classe com maior score
    int8_t *output_data = output->data.int8;
    int predicted_class = 0;
    float max_score = -1e30f;
    for (int i = 0; i < 10; i++) {
        float score = ((float)output_data[i] - (float)kOutputZeroPoint) * kOutputScale;
        ESP_LOGI(TAG, "classe %d: score=%.4f", i, score);
        if (i == 0 || score > max_score) {
            max_score = score;
            predicted_class = i;
        }
    }

    ESP_LOGI(TAG, "Digito previsto: %d (score: %.4f)", predicted_class, max_score);
}
