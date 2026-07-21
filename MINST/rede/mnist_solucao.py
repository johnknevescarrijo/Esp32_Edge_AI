import torch
import torch.nn as nn
import torch.optim as optim
from torchvision import datasets, transforms
import numpy as np
import tensorflow as tf
import os

# ============================================================================
# 1. Definir a arquitetura da rede
# ============================================================================
class NeuralNet(nn.Module):
    def __init__(self):
        super().__init__()
        self.fc1 = nn.Linear(28*28, 128)
        self.fc2 = nn.Linear(128, 10)
        self.relu = nn.ReLU()

    def forward(self, x):
        x = x.view(-1, 28*28)
        x = self.relu(self.fc1(x))
        return self.fc2(x)

# ============================================================================
# 2. Dados e treinamento
# ============================================================================
device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
print(f"Usando dispositivo: {device}")

transform = transforms.Compose([
    transforms.ToTensor(),
    transforms.Normalize((0.1307,), (0.3081,))
])

train_dataset = datasets.MNIST('./data', train=True, download=True, transform=transform)
test_dataset = datasets.MNIST('./data', train=False, download=True, transform=transform)

train_loader = torch.utils.data.DataLoader(train_dataset, batch_size=64, shuffle=True)
test_loader = torch.utils.data.DataLoader(test_dataset, batch_size=64, shuffle=False)

model = NeuralNet().to(device)

weights_file = 'mnist_model.pth'
if os.path.exists(weights_file):
    print("Carregando pesos existentes...")
    model.load_state_dict(torch.load(weights_file, map_location=device, weights_only=True))
else:
    print("Treinando o modelo...")
    criterion = nn.CrossEntropyLoss()
    optimizer = optim.SGD(model.parameters(), lr=0.1)

    epochs = 5
    for epoch in range(epochs):
        model.train()
        total_loss = 0
        for images, labels in train_loader:
            images, labels = images.to(device), labels.to(device)
            optimizer.zero_grad()
            outputs = model(images)
            loss = criterion(outputs, labels)
            loss.backward()
            optimizer.step()
            total_loss += loss.item()

        model.eval()
        correct = 0
        total = 0
        with torch.no_grad():
            for images, labels in test_loader:
                images, labels = images.to(device), labels.to(device)
                outputs = model(images)
                _, predicted = torch.max(outputs, 1)
                total += labels.size(0)
                correct += (predicted == labels).sum().item()
        acc = 100 * correct / total
        print(f"Época {epoch+1}/{epochs} | Perda: {total_loss/len(train_loader):.4f} | Acurácia: {acc:.2f}%")

    torch.save(model.state_dict(), weights_file)
    print(f"Pesos salvos em '{weights_file}'")

model.eval()

# ============================================================================
# 3. Conversão para TFLite
# ============================================================================
print("Convertendo para TFLite...")

# Extrair pesos
w1 = model.fc1.weight.detach().cpu().numpy().T  # (784, 128)
b1 = model.fc1.bias.detach().cpu().numpy()      # (128,)
w2 = model.fc2.weight.detach().cpu().numpy().T  # (128, 10)
b2 = model.fc2.bias.detach().cpu().numpy()      # (10,)

# Criar modelo Keras funcional
inputs = tf.keras.Input(shape=(28, 28, 1))
x = tf.keras.layers.Flatten()(inputs)
x = tf.keras.layers.Dense(128, activation='relu', use_bias=True, name='fc1')(x)
outputs = tf.keras.layers.Dense(10, activation='linear', use_bias=True, name='fc2')(x)

tf_model = tf.keras.Model(inputs=inputs, outputs=outputs)

# Aplicar pesos (agora usando nomes)
tf_model.get_layer('fc1').set_weights([w1, b1])
tf_model.get_layer('fc2').set_weights([w2, b2])

# Conversão FP32
converter = tf.lite.TFLiteConverter.from_keras_model(tf_model)
tflite_model = converter.convert()
with open('model.tflite', 'wb') as f:
    f.write(tflite_model)
print("Modelo TFLite (FP32) salvo como 'model.tflite'")

# Conversão com quantização INT8 (opcional)
def representative_dataset():
    from torchvision import datasets, transforms
    transform = transforms.Compose([transforms.ToTensor(), transforms.Normalize((0.1307,), (0.3081,))])
    dataset = datasets.MNIST('./data', train=True, download=False, transform=transform)
    for i in range(100):
        img, _ = dataset[i]
        img = img.numpy().reshape(1, 28, 28, 1).astype(np.float32)
        yield [img]

converter = tf.lite.TFLiteConverter.from_keras_model(tf_model)
converter.optimizations = [tf.lite.Optimize.DEFAULT]
converter.representative_dataset = representative_dataset
converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
converter.inference_input_type = tf.int8
converter.inference_output_type = tf.int8
tflite_quant = converter.convert()
with open('model_quantized.tflite', 'wb') as f:
    f.write(tflite_quant)
print("Modelo TFLite quantizado (INT8) salvo como 'model_quantized.tflite'")