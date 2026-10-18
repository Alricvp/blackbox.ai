#include <WiFi.h>
#include <HTTPClient.h>
#include <mbedtls/md.h>
#include <driver/i2s.h>

const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASS";
const char* serverUrl = "http://148.100.98.160:8000/api/tilt";

const int PIN_BCLK     = 12;
const int PIN_LRCK     = 13;
const int PIN_MIC_DIN  = 14;
const int PIN_AMP_DIN  = 15;

const float SOUND_TRIGGER_DBFS = -10.0f;
const int   ALARM_BURST_MS     = 500;
const int   ALARM_BURSTS       = 3;
const int   ALARM_FREQ_HZ      = 2000;

const uint32_t SAMPLE_INTERVAL_MS = 1000;
const int I2S_PORT = I2S_NUM_0;
const int SAMPLE_RATE = 16000;

const uint32_t WIFI_WAIT_MS = 20000;
const uint32_t WIFI_RETRY_MS = 10000;
uint32_t last_wifi_retry_ms = 0;
bool wifi_uplink_ok = false;
uint32_t last_sample_ms = 0;
uint32_t sample_count = 0;
bool alarm_active = false;

bool initI2sRx() {
  i2s_config_t cfg = {};
  cfg.mode = I2S_MODE_MASTER | I2S_MODE_RX;
  cfg.sample_rate = SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = 0;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;
  if (i2s_driver_install(I2S_PORT, &cfg, 0, NULL) != ESP_OK) return false;
  i2s_pin_config_t pins = {};
  pins.bck_io_num = PIN_BCLK;
  pins.ws_io_num = PIN_LRCK;
  pins.data_out_num = -1;
  pins.data_in_num = PIN_MIC_DIN;
  if (i2s_set_pin(I2S_PORT, &pins) != ESP_OK) return false;
  return true;
}

float readSoundDbfs() {
  static int16_t buf[2048];
  size_t nread = 0;
  esp_err_t r = i2s_read(I2S_PORT, buf, sizeof(buf), &nread, 100 / portTICK_PERIOD_MS);
  if (r != ESP_OK || nread < 256) return NAN;
  float sum = 0;
  int cnt = 0;
  for (size_t i = 0; i + 1 < nread; i += 2) {
    int16_t s = (int16_t)(buf[i + 1] << 8 | buf[i]);
    float f = s / 32768.0f;
    sum += f * f;
    cnt++;
  }
  if (cnt == 0) return NAN;
  float rms = sqrtf(sum / cnt);
  return rms > 1e-6f ? 20.0f * log10f(rms) : -96.0f;
}

void setupWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Wi-Fi connecting");
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_WAIT_MS) {
    Serial.print(".");
    delay(500);
  }
  if (WiFi.status() == WL_CONNECTED) {
    wifi_uplink_ok = true;
    Serial.println(" connected. IP: " + WiFi.localIP().toString());
  } else {
    Serial.println(" not up yet");
  }
  last_wifi_retry_ms = millis();
}

void serviceWifi() {
  if (WiFi.status() != WL_CONNECTED && millis() - last_wifi_retry_ms >= WIFI_RETRY_MS) {
    last_wifi_retry_ms = millis();
    WiFi.disconnect();
    WiFi.begin(ssid, password);
    Serial.println("Wi-Fi: retrying...");
  }
  wifi_uplink_ok = (WiFi.status() == WL_CONNECTED);
}

void setup() {
  Serial.begin(115200);
  initI2sRx();
  readSoundDbfs();
  setupWifi();
}

void loop() {
  serviceWifi();
  if (millis() - last_sample_ms < SAMPLE_INTERVAL_MS) return;
  last_sample_ms = millis();
  sample_count++;

  float sound_db = readSoundDbfs();
  String status_str = (!isnan(sound_db) && sound_db > SOUND_TRIGGER_DBFS) ? "warning" : "safe";

  char json_buf[256];
  snprintf(json_buf, sizeof(json_buf),
    "{\"device_id\":\"node2\",\"tilt\":0.0,\"moisture\":0.0,\"status\":\"%s\",\"ip\":\"%s\"}",
    status_str.c_str(), WiFi.localIP().toString().c_str());

  Serial.printf("[%lu] sound=%.1f dBFS status=%s\n", (unsigned long)sample_count, sound_db, status_str.c_str());

  if (WiFi.status() == WL_CONNECTED) {
    WiFiClient client;
    HTTPClient http;
    if (http.begin(client, serverUrl)) {
      http.addHeader("Content-Type", "application/json");
      http.setTimeout(1500);
      int code = http.POST(json_buf);
      if (code <= 0) {
        Serial.printf("POST failed (%d)\n", code);
      }
      http.end();
    }
  }
}
