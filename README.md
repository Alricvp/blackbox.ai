# BLACKBOX-Z — Machine Flight Recorder

Industrial predictive-maintenance telemetry: two ESP nodes measure a machine,
POST real readings to a receiver, and a SCADA-style dashboard visualizes
machine state, findings, and incident replay.

```
┌──────────────────────┐   Wi-Fi POST 1 Hz   ┌──────────────────────┐
│ Node 1 — ESP32       │ ──────────────────▶ │ receiver/server.py   │
│ MPU6050 + moisture   │  /api/telemetry     │ (laptop now,         │
├──────────────────────┤                     │  LinuxONE later)     │
│ Node 2 — ESP32-S3    │ ──────────────────▶ │ 0.0.0.0:5000         │
│ INMP441 mic + MAX98  │  /api/telemetry     └──────────────────────┘
└──────────────────────┘                              │
                                              dashboard/index.html
                                              (SCADA UI — bridge pending)
```

## Status

| Piece | State |
|---|---|
| `firmware/node1_esp32_telemetry_relay` — ESP32 vibration/tilt/leak node | Static-checked; bench serial-verified (sensors + Wi-Fi + POST path) |
| `firmware/node2_esp32s3_mic_alarm` — ESP32-S3 mic + alarm node | Static-checked; flash/bench test pending |
| `receiver/server.py` — dependency-free receiver + live status page | Tested: valid/invalid POSTs, latest/history/page |
| `dashboard/index.html` — SCADA-style interface | Browser-verified; **bridge to receiver pending** (see below) |

Honest gaps: the dashboard speaks the BLACKBOX-Z gateway protocol
(`ws://…/ws`, `/api/history/{node}`, `/api/incidents`, `/api/rewind/{node}`,
`POST …/freeze`) while the nodes/receiver speak `/api/telemetry`. Wiring the
dashboard onto the receiver (or growing the receiver into that gateway) is
the next integration step. Until then the dashboard correctly shows
"Waiting for data / Disconnected" rather than fake readings.

## The two nodes

One physical box each. Same repository, two sketches, no forks.

### Node 1 — ESP32 (vibration + leak)

| Component | ESP32 pin |
|---|---|
| MPU6050 SDA / SCL | GPIO 21 / GPIO 22 (addr 0x68, AD0→GND) |
| MPU6050 VCC / GND | 3.3V / GND |
| Moisture module AO | GPIO 34 (ADC1) |
| Moisture VCC / GND | 3.3V / GND |

Reads every second: `rms_vib` (m/s²), `tilt` (deg), `moisture_pct`,
`leak` (0/1), SHA-256 `hash`. Serial mirror works with Wi-Fi down.

### Node 2 — ESP32-S3 (mic + alarm)

| Component | ESP32-S3 pin |
|---|---|
| INMP441 SCK / WS / SD | GPIO 12 / GPIO 13 / GPIO 14 (L/R→GND) |
| MAX98357A BCLK / LRC / DIN | GPIO 12 / GPIO 13 / GPIO 15 |
| Both VCC / GND | 3.3V / GND; speaker on SPK± only (4–8 Ω) |

Reads sound level (`sound_db`, dBFS) at 1 Hz and plays a 2 kHz alarm tone
when sound crosses the threshold or the server replies `SHUTDOWN`/`ALARM`.
Pins avoid the S3 flash (26–32) and octal-PSRAM (33–37) ranges.

> Bluetooth A2DP is **not** used: the ESP32-S3 has BLE only, no Classic
> Bluetooth. Mic + amp run over plain I2S, which the S3 handles natively.

## Receiver

```bash
python receiver/server.py          # listens on 0.0.0.0:5000
```

- Live status page: `http://127.0.0.1:5000/` (real values only, empty state
  until a node posts)
- `GET /api/latest` — last reading per node · `GET /api/history?n=50`
- First Windows run: allow Python inbound on TCP 5000 in the firewall.

Prove the path without hardware:

```bash
curl -i -X POST http://127.0.0.1:5000/api/telemetry \
  -H "Content-Type: application/json" \
  -d '{"id":"node1","ms":1,"count":1,"rms_vib":8.8,"tilt":-19.0,"moisture_pct":0,"leak":0,"hash":"00"}'
```

Then point each sketch's `serverUrl` at **this machine's LAN IP**
(`ipconfig` / `ip a`) — e.g. `http://192.168.1.23:5000/api/telemetry`.

## Flashing

1. Arduino IDE + esp32 board package (if a sketch fails to compile on
   Arduino-ESP32 3.x, use Boards Manager → "esp32 by Espressif" → 2.0.x).
2. Libraries: **Adafruit MPU6050** + **Adafruit Unified Sensor** (node 1).
   Node 2 needs nothing beyond the core.
3. Put your Wi-Fi SSID/password in the `EDIT ONLY THIS BLOCK` section
   (placeholders only are committed — keep credentials out of Git).
4. Boards: node 1 → "ESP32 Dev Module" (or S3), node 2 → "ESP32S3 Dev
   Module" with PSRAM enabled if your module has it.
5. Serial Monitor @115200: node 1 prints readings + Wi-Fi status decode;
   node 2 prints dBFS. `serverUrl` placeholder warnings print at boot.

## Server contract (nodes → receiver)

`POST /api/telemetry`, JSON:

```json
{"id":"node1","ms":123456,"count":42,"rms_vib":8.82,"tilt":-19.1,
 "moisture_pct":0,"leak":0,"hash":"<sha256 hex>"}
{"id":"node2","ms":1234,"count":7,"sound_db":-42.5,"alarm":0,
 "hash":"<sha256 hex>"}
```

Receiver replies `{"ok":true,"command":null}`; a reply containing
`"command":"SHUTDOWN"` (or `"ALARM"` for node 2) triggers local action.

## Roadmap

- **LinuxONE deployment** — receiver is stdlib-only Python so it runs on any
  Linux runtime IBM provides; swap the URL into `serverUrl`.
- **Dashboard bridge** — serve the dashboard's gateway endpoints from the
  receiver so live telemetry, incidents, and replay light up.
- **Calibration** — `MOISTURE_THRESHOLD` (node 1) and `SOUND_TRIGGER_DBFS`
  (node 2) on real hardware.
