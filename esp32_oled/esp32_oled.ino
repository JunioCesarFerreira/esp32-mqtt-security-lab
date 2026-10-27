// ESP32 + MQTT sobre TLS + HMAC-SHA256 + display OLED SSD1306 (I2C)
//
// Mesma lógica de segurança do firmware em esp32/esp32.ino:
//   1. TLS (porta 8883)             -> criptografia do transporte
//   2. usuário/senha MQTT           -> autenticação no broker
//   3. HMAC-SHA256 (chave partilhada com o subscriber, nunca com o broker)
//                                   -> integridade e autenticidade dos dados
//
// Além disso, mostra temperatura e umidade num display OLED SSD1306 128x64.
// Sem sensor DHT, os dois valores são gerados aleatoriamente.
//
// A mensagem MQTT é a mesma do firmware original (só a temperatura é
// publicada e assinada), então subscriber, painel e atacante funcionam sem
// alteração. A umidade aparece apenas no display.
//
// Ligação do display (I2C padrão do ESP32):
//   OLED VCC -> 3V3
//   OLED GND -> GND
//   OLED SDA -> GPIO 21
//   OLED SCL -> GPIO 22
//
// Bibliotecas necessárias (Arduino IDE > Library Manager):
//   - PubSubClient (Nick O'Leary)
//   - Adafruit SSD1306 (Adafruit) - instala junto a Adafruit GFX Library
// mbedtls/md.h já faz parte do core do ESP32 (Espressif), não precisa instalar.

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "mbedtls/md.h"

#include "secrets.h"
#include "ca_cert.h"

#define OLED_WIDTH  128
#define OLED_HEIGHT 64
#define OLED_ADDR   0x3C   // alguns módulos usam 0x3D
#define OLED_SDA    21
#define OLED_SCL    22

WiFiClientSecure secureClient;
PubSubClient mqttClient(secureClient);
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

bool displayOk = false;
uint32_t seqNumber = 0;
volatile uint8_t wifiDisconnectReason = 0;

void showStatus(const char *line1, const char *line2 = "") {
  if (!displayOk) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(DEVICE_ID);
  display.drawFastHLine(0, 10, OLED_WIDTH, SSD1306_WHITE);
  display.setCursor(0, 20);
  display.print(line1);
  display.setCursor(0, 32);
  display.print(line2);
  display.display();
}

void showReadings(const char *tempStr, const char *humStr, bool published) {
  if (!displayOk) return;
  display.clearDisplay();

  // Cabeçalho: dispositivo e número de sequência
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(DEVICE_ID);
  char seqStr[12];
  snprintf(seqStr, sizeof(seqStr), "#%lu", (unsigned long)seqNumber);
  display.setCursor(OLED_WIDTH - strlen(seqStr) * 6, 0);
  display.print(seqStr);
  display.drawFastHLine(0, 10, OLED_WIDTH, SSD1306_WHITE);

  // Temperatura
  display.setCursor(0, 19);
  display.print("Temp");
  display.setTextSize(2);
  display.setCursor(36, 14);
  display.print(tempStr);
  display.write(0xF8);  // símbolo de grau (CP437)
  display.print("C");

  // Umidade
  display.setTextSize(1);
  display.setCursor(0, 39);
  display.print("Umid");
  display.setTextSize(2);
  display.setCursor(36, 34);
  display.print(humStr);
  display.print("%");

  // Rodapé: estado da publicação MQTT
  display.setTextSize(1);
  display.setCursor(0, 56);
  display.print(published ? "MQTT/TLS: publicado" : "MQTT/TLS: falhou");

  display.display();
}

void setupDisplay() {
  Wire.begin(OLED_SDA, OLED_SCL);
  // display.begin() não verifica o barramento, então testamos se há
  // algum dispositivo respondendo no endereço do display
  Wire.beginTransmission(OLED_ADDR);
  if (Wire.endTransmission() != 0) {
    Serial.printf("Display SSD1306 não encontrado em 0x%02X - seguindo sem display\n", OLED_ADDR);
    return;
  }
  // periphBegin = false: o Wire já foi iniciado acima com os pinos explícitos
  displayOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR, true, false);
  if (!displayOk) {
    Serial.println("Falha ao iniciar o display SSD1306 - seguindo sem display");
    return;
  }
  Serial.printf("Display SSD1306 encontrado em 0x%02X\n", OLED_ADDR);
  display.cp437(true);
  display.setTextColor(SSD1306_WHITE);
  showStatus("Iniciando...");
}

void connectWiFi() {
  // Mostra o motivo de cada falha (ex.: senha errada, rede não encontrada)
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    wifiDisconnectReason = info.wifi_sta_disconnected.reason;
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Conectando ao Wi-Fi");
  showStatus("Conectando Wi-Fi", WIFI_SSID);
  uint8_t shownReason = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
    uint8_t reason = wifiDisconnectReason;
    if (reason != 0 && reason != shownReason) {
      shownReason = reason;
      const char *name = WiFi.disconnectReasonName((wifi_err_reason_t)reason);
      Serial.printf("\nFalha no Wi-Fi: motivo %u (%s)\n", reason, name);
      char reasonStr[24];
      snprintf(reasonStr, sizeof(reasonStr), "falha: motivo %u", reason);
      showStatus(WIFI_SSID, reasonStr);
    }
  }
  Serial.println();
  Serial.print("Wi-Fi conectado, IP: ");
  Serial.println(WiFi.localIP());
  showStatus("Wi-Fi conectado", WiFi.localIP().toString().c_str());
}

void connectMQTT() {
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  while (!mqttClient.connected()) {
    Serial.print("Conectando ao broker MQTT...");
    showStatus("Conectando MQTT", MQTT_HOST);
    String clientId = String(DEVICE_ID) + "-" + String(random(0xffff), HEX);
    if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD)) {
      Serial.println(" conectado.");
      showStatus("MQTT conectado");
    } else {
      Serial.print(" falhou, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" - tentando novamente em 3s");
      char rcStr[24];
      snprintf(rcStr, sizeof(rcStr), "rc=%d, aguarde 3s", mqttClient.state());
      showStatus("MQTT falhou", rcStr);
      delay(3000);
    }
  }
}

static uint8_t hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 0;
}

size_t hexKeyToBytes(const char *hex, uint8_t *out, size_t maxLen) {
  size_t len = strlen(hex) / 2;
  if (len > maxLen) len = maxLen;
  for (size_t i = 0; i < len; i++) {
    out[i] = (hexNibble(hex[i * 2]) << 4) | hexNibble(hex[i * 2 + 1]);
  }
  return len;
}

void hmacSha256Hex(const String &message, const uint8_t *key, size_t keyLen, char *outHex) {
  uint8_t digest[32];
  mbedtls_md_context_t ctx;
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, info, 1);
  mbedtls_md_hmac_starts(&ctx, key, keyLen);
  mbedtls_md_hmac_update(&ctx, (const unsigned char *)message.c_str(), message.length());
  mbedtls_md_hmac_finish(&ctx, digest);
  mbedtls_md_free(&ctx);

  for (int i = 0; i < 32; i++) {
    sprintf(outHex + (i * 2), "%02x", digest[i]);
  }
  outHex[64] = '\0';
}

float readSimulatedTemperature() {
  // Substitua por dht.readTemperature() quando tiver o sensor DHT
  float base = 25.0;
  float noise = (random(-100, 100) / 100.0);
  return base + noise;
}

float readSimulatedHumidity() {
  // Substitua por dht.readHumidity() quando tiver o sensor DHT
  float base = 60.0;
  float noise = (random(-500, 500) / 100.0);
  return base + noise;
}

void setup() {
  Serial.begin(115200);
  randomSeed(esp_random());

  setupDisplay();

  connectWiFi();

  secureClient.setCACert(CA_CERT);

  connectMQTT();
}

void loop() {
  if (!mqttClient.connected()) {
    connectMQTT();
  }
  mqttClient.loop();

  seqNumber++;
  float temperature = readSimulatedTemperature();
  float humidity = readSimulatedHumidity();

  char tempStr[8];
  snprintf(tempStr, sizeof(tempStr), "%.2f", temperature);
  char humStr[8];
  snprintf(humStr, sizeof(humStr), "%.1f", humidity);

  uint8_t hmacKey[32];
  size_t keyLen = hexKeyToBytes(HMAC_KEY_HEX, hmacKey, sizeof(hmacKey));

  String signingMessage = String(DEVICE_ID) + "|" + String(seqNumber) + "|" + String(tempStr);

  char hmacHexStr[65];
  hmacSha256Hex(signingMessage, hmacKey, keyLen, hmacHexStr);

  char payload[256];
  snprintf(payload, sizeof(payload),
           "{\"device\":\"%s\",\"seq\":%lu,\"temp\":\"%s\",\"hmac\":\"%s\"}",
           DEVICE_ID, (unsigned long)seqNumber, tempStr, hmacHexStr);

  Serial.print("Publicando: ");
  Serial.println(payload);
  Serial.print("Umidade (só no display): ");
  Serial.println(humStr);

  bool published = mqttClient.publish(MQTT_TOPIC, payload);
  showReadings(tempStr, humStr, published);

  delay(5000);
}
