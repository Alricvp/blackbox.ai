#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <mbedtls/md.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASS";
const char* serverUrl = "http://148.100.98.160:8000/api/tilt";

const uint32_t SAMPLE_INTERVAL_MS = 1000;
Adafruit_MPU6050 mpu;
sensors_event_t a, g, tempEvent;
const int MOISTURE_PIN = 34;
int MOISTURE_THRESHOLD = 2000;
const uint32_t WIFI_WAIT_MS = 20000;
const uint32_t WIFI_RETRY_MS = 10000;
uint32_t last_wifi_retry_ms = 0;
bool wifi_uplink_ok = false;
uint32_t last_sample_ms = 0;
uint32_t sample_count = 0;

void sha256_of(const uint8_t* data, size_t len, uint8_t* out_hash) {
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
  mbedtls_md_starts(&ctx);
  mbedtls_md_update(&ctx, data, len);
  mbedtls_md_finish(&ctx, out_hash);
  mbedtls_md_free(&ctx);
}

static String wlStatusName(int s) {
  switch (s) {
    case WL_IDLE_STATUS:     return "IDLE";
    case WL_NO_SSID_AVAIL:   return "NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED:  return "SCAN_COMPLETED";
    case WL_CONNECTED:       return "CONNECTED";
    case WL_CONNECT_FAILED:  return "CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED:    return "DISCONNECTED";
    default:                 return "code " + String(s);
  }
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
  if (!mpu.begin()) {
    Serial.println("MPU6050 init failed");
    while (1) delay(1000);
  }
  mpu.setAccelerometerRange(MPU6050_RANGE_2_G);
  mpu.setGyroRange(MPU6050_RANGE_500_DEG);
  setupWifi();
}

void loop() {
  serviceWifi();
  if (millis() - last_sample_ms < SAMPLE_INTERVAL_MS) return;
  last_sample_ms = millis();
  sample_count++;

  mpu.getEvent(&a, &g, &tempEvent);
  float acc_mag = sqrt(a.acceleration.x*a.acceleration.x + a.acceleration.y*a.acceleration.y + a.acceleration.z*a.acceleration.z);
  float tilt = atan2(a.acceleration.x, a.acceleration.z) * 180.0 / PI;
  int raw_adc = analogRead(MOISTURE_PIN);
  int moisture_pct = constrain(map(raw_adc, 0, 4095, 100, 0), 0, 100);
  int leak = (raw_adc < MOISTURE_THRESHOLD) ? 1 : 0;
  String status_str = (leak || acc_mag > 5.0 || abs(tilt) > 60.0) ? "warning" : "safe";

  char json_buf[256];
  snprintf(json_buf, sizeof(json_buf),
    "{\"device_id\":\"node1\",\"tilt\":%.2f,\"moisture\":%.1f,\"status\":\"%s\",\"ip\":\"%s\"}",
    tilt, (float)moisture_pct, status_str.c_str(), WiFi.localIP().toString().c_str());

  Serial.printf("[%lu] tilt=%.1f moist=%d%% leak=%d acc=%.2f status=%s\n",
                (unsigned long)sample_count, tilt, moisture_pct, leak, acc_mag, status_str.c_str());

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
