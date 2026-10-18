/*
 * node1_esp32_telemetry_relay.ino
 * Node 1 — Telemetry Node (simplified)
 * MPU6050 (vibration + tilt) + analog moisture/leak sensor + Wi-Fi POST.
 * No DHT22, no relay: this node only observes and reports.
 *
 * Fixed-time sampling: one record per SAMPLE_INTERVAL_MS regardless of
 * network speed, so the server-side timeline never has jittery gaps.
 *
 * Flashes unchanged on BOTH boards (classic ESP32 and ESP32-S3):
 * GPIO 21/22 (I2C) and GPIO 34 (ADC1) exist on both.
 * Board: "ESP32 Dev Module" or "ESP32S3 Dev Module".
 *
 * Libraries (Arduino Library Manager):
 *   - Adafruit MPU6050
 *   - Adafruit Unified Sensor
 *   (mbedtls, WiFi, HTTPClient, Wire are in the ESP32 core)
 *
 * Server contract (UNCHANGED — must match the LinuxONE receiver):
 *   POST /api/telemetry  body:
 *   {"id":"node1","ms":<millis>,"count":<n>,"rms_vib":<f>,
 *    "tilt":<f>,"moisture_pct":<int>,"leak":<0|1>,"hash":"<sha256 hex>"}
 *   hash = SHA-256 of the payload string
 *     {"rms_vib":..,"tilt":..,"moisture_pct":..,"leak":..}
 *   server may reply {"command":"SHUTDOWN"}.
 */

#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <mbedtls/md.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>   // for https:// (IBM Cloud) endpoints

// ===================== EDIT ONLY THIS BLOCK =====================
// Wi-Fi (placeholders only in this repo copy — put your real network here
// when flashing; credentials never go into Git)
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASS";

// Receiver = wherever you run receiver/server.py.
// NOW (demo): your LAPTOP on the same Wi-Fi — use its LAN IPv4 (ipconfig
// on Windows / ip a on Linux), e.g. 192.168.1.23:
//   const char* serverUrl = "http://192.168.1.23:5000/api/telemetry";
// LATER (LinuxONE): swap in whatever URL they give you — both schemes work:
//   http://<public-ip>:5000/api/telemetry       (open port)
//   https://<host>/api/telemetry                (443, TLS)
const char* serverUrl = "http://REPLACE-RECEIVER-HOST:5000/api/telemetry";
// =================================================================

// ===== Sampling =====
const uint32_t SAMPLE_INTERVAL_MS = 1000;  // 1 Hz, constant-time

// ===== MPU6050 (I2C on GPIO 21/22) =====
Adafruit_MPU6050 mpu;
sensors_event_t a, g, tempEvent;

// ===== Moisture / leak sensor (analog) =====
const int MOISTURE_PIN = 34;               // ADC1, input only
int MOISTURE_THRESHOLD = 2000;             // raw ADC: below this = leak (calibrate!)
// Calibration: flash once, type nothing, read dry + wet analogRead(34)
// values in Serial, then set MOISTURE_THRESHOLD between them.

// ===== Wi-Fi reconnect state (non-blocking) =====
const uint32_t WIFI_WAIT_MS = 20000;       // initial wait at boot (then run anyway)
const uint32_t WIFI_RETRY_MS = 10000;      // retry cadence while down
uint32_t last_wifi_retry_ms = 0;
bool wifi_uplink_ok = false;

// ===== SHA-256 fingerprint =====
void sha256_of(const uint8_t* data, size_t len, uint8_t* out_hash) {
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
  mbedtls_md_starts(&ctx);
  mbedtls_md_update(&ctx, data, len);
  mbedtls_md_finish(&ctx, out_hash);
  mbedtls_md_free(&ctx);
}

// ===== Wi-Fi diagnostics (names the exact connect failure on serial) =====
static String wlStatusName(int s) {
  switch (s) {
    case WL_IDLE_STATUS:     return "IDLE";
    case WL_NO_SSID_AVAIL:   return "NO_SSID_AVAIL (SSID not visible on 2.4 GHz)";
    case WL_SCAN_COMPLETED:  return "SCAN_COMPLETED";
    case WL_CONNECTED:       return "CONNECTED";
    case WL_CONNECT_FAILED:  return "CONNECT_FAILED (wrong password / WPA3-only router?)";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED:    return "DISCONNECTED (still associating or out of range)";
    default:                 return "code " + String(s);
  }
}

// Boot-time 2.4 GHz scan: proves whether the SSID exists where ESP32 can see it.
static void diagnoseWifi() {
  Serial.println("Scanning 2.4 GHz for \"" + String(ssid) + "\" ...");
  int n = WiFi.scanNetworks();
  if (n <= 0) {
    Serial.println("  no networks visible at all — RF problem (antenna/board)");
    return;
  }
  bool found = false;
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == ssid) {
      found = true;
      Serial.printf("  FOUND \"%s\"  ch=%d  rssi=%d dBm  %s\n",
                    WiFi.SSID(i).c_str(), WiFi.channel(i), WiFi.RSSI(i),
                    (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "open" : "secure");
    }
  }
  if (!found) {
    Serial.println("  SSID NOT in the 2.4 GHz scan. Check:");
    Serial.println("   1) Router: 2.4 GHz radio ENABLED (ESP32 has NO 5 GHz radio)");
    Serial.println("   2) SSID matches EXACTLY (case + spaces), not hidden");
    Serial.println("   3) If router merges both bands, give 2.4 GHz its own SSID");
  }
}

// ===== State =====
uint32_t last_sample_ms = 0;
uint32_t sample_count = 0;

void setup() {
  Serial.begin(115200);
  Serial.println("\n=== Node 1 (Telemetry) Starting ===");

  // MPU6050 on I2C pins 21/22
  Wire.begin(21, 22);
  if (!mpu.begin()) {
    Serial.println("MPU6050 not found! Check SDA=21 SCL=22 and 3.3V.");
    while (true) delay(1000);
  }
  mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
  mpu.setGyroRange(MPU6050_RANGE_500_DEG);

  // Wi-Fi: wait up to WIFI_WAIT_MS at boot, then continue regardless
  // (a dead router must not stop local sampling / serial mirror).
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);   // ESP32 power-save silently drops AP association — off
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
    Serial.println(" not up after " + String(WIFI_WAIT_MS / 1000) + " s. boot status=" +
                   wlStatusName(WiFi.status()));
    diagnoseWifi();       // is the SSID even visible on 2.4 GHz?
    Serial.println(" Sampling continues; will retry every 10 s.");
  }
  last_wifi_retry_ms = millis();

  // ---- connection diagnostics (finds the POST -1 cause) ----
  Serial.println("POST target: " + String(serverUrl));
  Serial.println("Local IP: " + WiFi.localIP().toString() +
                 "  Gateway: " + WiFi.gatewayIP().toString() +
                 "  Subnet: " + WiFi.subnetMask().toString());
  if (String(serverUrl).indexOf(".X:") >= 0 ||
      String(serverUrl).indexOf("REPLACE-RECEIVER-HOST") >= 0) {
    Serial.println("!! FIX: serverUrl is still a placeholder. Put the receiver's "
                   "URL — laptop LAN IP now (e.g. http://192.168.1.23:5000/"
                   "api/telemetry), the LinuxONE URL later.");
  }

  Serial.println("Node 1 ready — sampling at 1 Hz.");
}

void loop() {
  // ---- non-blocking Wi-Fi recovery ----
  if (WiFi.status() != WL_CONNECTED &&
      millis() - last_wifi_retry_ms >= WIFI_RETRY_MS) {
    last_wifi_retry_ms = millis();
    Serial.println("Wi-Fi retry, status=" + wlStatusName(WiFi.status()) +
                   " — re-begin (no disconnect, it can wedge association)");
    WiFi.begin(ssid, password);
  }
  if (WiFi.status() == WL_CONNECTED && !wifi_uplink_ok) {
    wifi_uplink_ok = true;
    Serial.println("Wi-Fi: up. IP: " + WiFi.localIP().toString());
  } else if (WiFi.status() != WL_CONNECTED) {
    wifi_uplink_ok = false;
  }

  // ---- fixed 1 Hz sampling ----
  if (millis() - last_sample_ms < SAMPLE_INTERVAL_MS) return;
  last_sample_ms = millis();
  sample_count++;

  // ---- Read sensors ----
  mpu.getEvent(&a, &g, &tempEvent);
  // Adafruit lib already returns m/s^2 — do NOT multiply by 9.81 again.
  float acc_mag = sqrt(a.acceleration.x * a.acceleration.x +
                       a.acceleration.y * a.acceleration.y +
                       a.acceleration.z * a.acceleration.z);
  float rms_vib = acc_mag;                            // m/s^2 magnitude
  float tilt = atan2(a.acceleration.x, a.acceleration.z) * 180.0 / PI;  // degrees

  int raw_adc = analogRead(MOISTURE_PIN);             // 0..4095
  int moisture_pct = constrain(map(raw_adc, 0, 4095, 100, 0), 0, 100);
  int leak = (raw_adc < MOISTURE_THRESHOLD) ? 1 : 0;

  // ---- Build payload, hash it, then the full JSON packet ----
  char payload[160];
  snprintf(payload, sizeof(payload),
    "{\"rms_vib\":%.2f,\"tilt\":%.2f,\"moisture_pct\":%d,\"leak\":%d}",
    rms_vib, tilt, moisture_pct, leak);

  uint8_t hash[32];
  sha256_of((uint8_t*)payload, strlen(payload), hash);

  char hash_hex[65];
  for (int i = 0; i < 32; i++) {
    snprintf(hash_hex + i * 2, 3, "%02x", hash[i]);
  }
  hash_hex[64] = '\0';

  char json_buf[400];
  snprintf(json_buf, sizeof(json_buf),
    "{\"id\":\"node1\",\"ms\":%lu,\"count\":%lu,\"rms_vib\":%.2f,\"tilt\":%.2f,"
    "\"moisture_pct\":%d,\"leak\":%d,\"hash\":\"%s\"}",
    (unsigned long)millis(), (unsigned long)sample_count, rms_vib, tilt,
    moisture_pct, leak, hash_hex);

  // ---- Local serial mirror (works even with Wi-Fi down) ----
  Serial.printf("[%lu] vib=%.2f m/s2 tilt=%.1f moist=%d%% leak=%d raw=%d\n",
                (unsigned long)sample_count, rms_vib, tilt, moisture_pct,
                leak, raw_adc);

  // ---- POST to receiver ----
  if (WiFi.status() == WL_CONNECTED) {
    // http:// = plain TCP; https:// = TLS (setInsecure skips cert pinning —
    // fine for a demo endpoint; pin a CA fingerprint for production).
    WiFiClient plainClient;
    WiFiClientSecure secureClient;
    HTTPClient http;
    bool isHttps = String(serverUrl).startsWith("https");
    bool okBegin;
    if (isHttps) {
      secureClient.setInsecure();
      okBegin = http.begin(secureClient, serverUrl);
    } else {
      okBegin = http.begin(plainClient, serverUrl);
    }
    if (!okBegin) {
      Serial.println("http.begin() FAILED — malformed URL? target=" + String(serverUrl));
      http.end();
      return;
    }
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(isHttps ? 4000 : 1500);   // TLS handshake needs more time
    int code = http.POST(json_buf);
    if (code > 0) {
      String resp = http.getString();
      if (resp.indexOf("\"command\":\"SHUTDOWN\"") >= 0) {
        Serial.println(">> SHUTDOWN command from server — flagging anomaly locally.");
      }
    } else {
      // -1 = connection failed (wrong IP / server not listening / firewall)
      // -2 = sent, no reply in time (server busy or hung)
      Serial.printf("POST failed (%d): %s  [target %s]\n",
                    code, HTTPClient::errorToString(code).c_str(), serverUrl);
    }
    http.end();
  } else {
    Serial.println("Wi-Fi lost — samples continue to serial.");
  }
}
