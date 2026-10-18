/*
 * node2_esp32s3_mic_alarm.ino
 * Node 2 — ESP32-S3: INMP441 mic (I2S input) + MAX98357A alarm (I2S output).
 *
 * The S3 box has BOTH the mic and the amp, so this one sketch:
 *   - samples the mic at 1 Hz and POSTs the real sound level (dBFS) to the
 *     server (same endpoint/contract style as node1)
 *   - plays a 2 kHz alarm tone on the amp when the sound crosses
 *     SOUND_TRIGGER_DBFS, or when the server replies "SHUTDOWN"/"ALARM"
 *
 * WHY NOT THE A2DP/BLUETOOTH SKETCH:
 *   Bluetooth Classic (A2DP) does NOT exist on the ESP32-S3 — it is BLE-only.
 *   The A2DP speaker code only runs on a classic ESP32. Mic + amp on the S3
 *   are plain I2S, which is what this sketch uses.
 *
 * BOARD: "ESP32S3 Dev Module". Libraries: none beyond the ESP32 core
 * (Wire/WiFi/HTTPClient/mbedtls/driver-i2s are all built in).
 * If the sketch fails to compile on Arduino-ESP32 3.x, switch
 * Boards Manager -> "esp32 by Espressif" -> 2.0.x (same as BLACKBOX-Z node).
 *
 * PINS (S3-SAFE — avoid GPIO26-32 which are SPI flash, GPIO33-37 which are
 * octal PSRAM on many S3 modules, GPIO19/20 USB, strapping 0/3/45/46):
 *   INMP441   VIN -> 3.3V, GND -> GND
 *             SCK (BCLK)  -> GPIO 12
 *             WS  (LRCLK) -> GPIO 13
 *             SD  (data)  -> GPIO 14
 *             L/R jumper   -> GND  (left channel, matches ONLY_LEFT)
 *   MAX98357A VIN -> 3.3V, GND -> GND
 *             BCLK -> GPIO 12   (shared clock — both chips only listen)
 *             LRC  -> GPIO 13
 *             DIN  -> GPIO 15   (ESP32 OUTPUT — never share with mic data)
 *             SD   -> module default (gain/jumper per silkscreen)
 *             SPK+/- -> 4..8 ohm speaker ONLY — never a GPIO
 *
 * TIMING NOTE: the alarm tone briefly pauses mic capture (one I2S peripheral,
 * switched RX -> TX -> RX). That is intended and documented behaviour.
 *
 * Server contract (same as node1, extra sound fields):
 *   POST /api/telemetry
 *   {"id":"node2","ms":..,"count":..,"sound_db":<-96..0>,"alarm":<0|1>,
 *    "hash":"<sha256 hex of {\"sound_db\":..,\"alarm\":..}>"}
 *   Receiver just needs to accept id="node2" and read sound_db.
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <mbedtls/md.h>
#include <driver/i2s.h>

// ===================== EDIT ONLY THIS BLOCK =====================
// Wi-Fi (placeholders only in this repo copy — put your real network here
// when flashing; credentials never go into Git)
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASS";

// IBM LinuxONE endpoint — REPLACE 192.168.1.X with the server's real LAN IP
const char* serverUrl = "http://192.168.1.X:5000/api/telemetry";

// I2S pins (S3-safe, see header)
const int PIN_BCLK     = 12;   // shared clock
const int PIN_LRCK     = 13;   // shared word select
const int PIN_MIC_DIN  = 14;   // INMP441 SD  -> ESP32 input
const int PIN_AMP_DIN  = 15;   // MAX98357 DIN -> ESP32 output

// Alarm behaviour
const float SOUND_TRIGGER_DBFS = -10.0f;  // louder than this (0 = full scale) -> alarm
const int   ALARM_BURST_MS     = 500;     // one tone burst length
const int   ALARM_BURSTS       = 3;       // number of bursts
const int   ALARM_FREQ_HZ      = 2000;    // tone
// =================================================================

const uint32_t SAMPLE_INTERVAL_MS = 1000;   // 1 Hz, constant-time
const int I2S_PORT = I2S_NUM_0;
const int SAMPLE_RATE = 16000;

const uint32_t WIFI_WAIT_MS = 20000;
const uint32_t WIFI_RETRY_MS = 10000;
uint32_t last_wifi_retry_ms = 0;
bool wifi_uplink_ok = false;

uint32_t last_sample_ms = 0;
uint32_t sample_count = 0;
bool alarm_active = false;   // sticky flag reported to the server

// ===== SHA-256 fingerprint (same helper as node1) =====
void sha256_of(const uint8_t* data, size_t len, uint8_t* out_hash) {
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
  mbedtls_md_starts(&ctx);
  mbedtls_md_update(&ctx, data, len);
  mbedtls_md_finish(&ctx, out_hash);
  mbedtls_md_free(&ctx);
}

// ===== I2S: mic receiver =====
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
  if (i2s_driver_install(I2S_PORT, &cfg, 0, NULL) != ESP_OK) {
    Serial.println("I2S RX install failed");
    return false;
  }
  i2s_pin_config_t pins = {};
  pins.bck_io_num = PIN_BCLK;
  pins.ws_io_num = PIN_LRCK;
  pins.data_out_num = -1;
  pins.data_in_num = PIN_MIC_DIN;
  if (i2s_set_pin(I2S_PORT, &pins) != ESP_OK) {
    Serial.println("I2S RX pin set failed");
    return false;
  }
  Serial.println("INMP441 I2S RX: ok @16kHz");
  return true;
}

// Read the mic for a short window and return RMS level in dBFS
// (-96 = silence floor). Real sensor value, never invented.
float readSoundDbfs() {
  static int16_t buf[2048];              // 1024 samples = 64 ms @16 kHz
  size_t nread = 0;
  esp_err_t r = i2s_read(I2S_PORT, buf, sizeof(buf), &nread, 100 / portTICK_PERIOD_MS);
  if (r != ESP_OK || nread < 256) return NAN;
  float sum = 0;
  int cnt = 0;
  for (size_t i = 0; i + 1 < nread; i += 2) {   // 16-bit little-endian samples
    int16_t s = (int16_t)(buf[i + 1] << 8 | buf[i]);
    float f = s / 32768.0f;
    sum += f * f;
    cnt++;
  }
  if (cnt == 0) return NAN;
  float rms = sqrtf(sum / cnt);
  return rms > 1e-6f ? 20.0f * log10f(rms) : -96.0f;
}

// ===== I2S: alarm tone on the amp (switches RX -> TX -> RX) =====
void playAlarm() {
  i2s_driver_uninstall(I2S_PORT);

  i2s_config_t cfg = {};
  cfg.mode = I2S_MODE_MASTER | I2S_MODE_TX;
  cfg.sample_rate = SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = 0;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;
  if (i2s_driver_install(I2S_PORT, &cfg, 0, NULL) != ESP_OK) {
    Serial.println("I2S TX install failed — alarm skipped");
    initI2sRx();                            // recover the mic anyway
    return;
  }
  i2s_pin_config_t pins = {};
  pins.bck_io_num = PIN_BCLK;
  pins.ws_io_num = PIN_LRCK;
  pins.data_out_num = PIN_AMP_DIN;
  pins.data_in_num = -1;
  i2s_set_pin(I2S_PORT, &pins);

  // 2 kHz sine, 16-bit mono
  const int N = 256;
  int16_t tone[N];
  for (int i = 0; i < N; i++) {
    tone[i] = (int16_t)(sinf(2.0f * PI * ALARM_FREQ_HZ * i / SAMPLE_RATE) * 12000.0f);
  }
  size_t written;
  for (int burst = 0; burst < ALARM_BURSTS; burst++) {
    uint32_t t_end = millis() + ALARM_BURST_MS;
    while (millis() < t_end) {
      i2s_write(I2S_PORT, tone, sizeof(tone), &written, portMAX_DELAY);
    }
    delay(150);                             // gap between bursts
  }

  i2s_driver_uninstall(I2S_PORT);
  initI2sRx();                              // mic back online
  Serial.println("alarm tone played");
}

// ===== Wi-Fi (non-blocking, same pattern as node1) =====
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
    Serial.println(" not up yet — sampling continues, will retry every 10 s.");
  }
  last_wifi_retry_ms = millis();
}

void serviceWifi() {
  if (WiFi.status() != WL_CONNECTED &&
      millis() - last_wifi_retry_ms >= WIFI_RETRY_MS) {
    last_wifi_retry_ms = millis();
    WiFi.disconnect();
    WiFi.begin(ssid, password);
    Serial.println("Wi-Fi: retrying...");
  }
  wifi_uplink_ok = (WiFi.status() == WL_CONNECTED);
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n=== Node 2 (S3 mic + alarm) Starting ===");
  initI2sRx();
  // drain whatever is in the DMA buffer so the first reading is fresh
  readSoundDbfs();
  setupWifi();
  Serial.println("Node 2 ready — sampling sound at 1 Hz.");
}

void loop() {
  serviceWifi();

  if (millis() - last_sample_ms < SAMPLE_INTERVAL_MS) return;
  last_sample_ms = millis();
  sample_count++;

  // ---- real mic reading ----
  float sound_db = readSoundDbfs();

  // ---- local alarm decision (also server-driven below) ----
  if (!isnan(sound_db) && sound_db > SOUND_TRIGGER_DBFS) {
    alarm_active = true;
  }
  if (alarm_active) {
    playAlarm();                 // pauses mic ~1.8 s, then RX resumes
  }

  // ---- build payload, hash it, full JSON (same style as node1) ----
  int alarm_int = alarm_active ? 1 : 0;
  alarm_active = false;          // reported once, then re-armed

  char payload[96];
  snprintf(payload, sizeof(payload),
    "{\"sound_db\":%.1f,\"alarm\":%d}",
    isnan(sound_db) ? -99.0f : sound_db, alarm_int);

  uint8_t hash[32];
  sha256_of((uint8_t*)payload, strlen(payload), hash);
  char hash_hex[65];
  for (int i = 0; i < 32; i++) snprintf(hash_hex + i * 2, 3, "%02x", hash[i]);
  hash_hex[64] = '\0';

  char json_buf[320];
  snprintf(json_buf, sizeof(json_buf),
    "{\"id\":\"node2\",\"ms\":%lu,\"count\":%lu,\"sound_db\":%.1f,\"alarm\":%d,"
    "\"hash\":\"%s\"}",
    (unsigned long)millis(), (unsigned long)sample_count,
    isnan(sound_db) ? -99.0f : sound_db, alarm_int, hash_hex);

  // ---- local serial mirror (works with Wi-Fi down) ----
  if (isnan(sound_db)) {
    Serial.printf("[%lu] mic read failed\n", (unsigned long)sample_count);
  } else {
    Serial.printf("[%lu] sound=%.1f dBFS alarm=%d\n",
                  (unsigned long)sample_count, sound_db, alarm_int);
  }

  // ---- POST ----
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(serverUrl);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(1500);
    int code = http.POST(json_buf);
    if (code > 0) {
      String resp = http.getString();
      if (resp.indexOf("\"command\":\"SHUTDOWN\"") >= 0 ||
          resp.indexOf("\"command\":\"ALARM\"") >= 0) {
        Serial.println(">> server command — sounding local alarm.");
        alarm_active = true;
      }
    } else {
      Serial.printf("POST failed (%d) — sample kept on serial only.\n", code);
    }
    http.end();
  } else {
    Serial.println("Wi-Fi lost — samples continue to serial.");
  }
}
