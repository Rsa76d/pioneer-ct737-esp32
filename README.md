# Pioneer CT-737 ESP32-S3 Controller

ESP32-S3 firmware for controlling the Pioneer CT-737 cassette deck via WiFi, BLE, MQTT, and Home Assistant.

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

---

## Features

- **VFD control** — original Pioneer CT-737 VFD display via 3 shift-register chips (24-bit x3)
- **VU meter** — real-time level meters on VFD (16 segments per channel, peak hold)
- **OLED display** — 128×64 I2C OLED with 14 display styles (Sd00–Sd13)
- **Transport control** — solenoids: PLAY / STOP / REWIND / FORWARD
- **Tape counter** — pulse counter with Tape Return and Remain (song repeat) modes
- **PCF8574** — reads transport sense signals (PLAY/PAUSE/FWD/REC/REW) and tape type switches
- **FFT spectrum analyzer** — 128-band FFT via PCM1808 I2S ADC
- **WS2812B LED strip** — 40 LEDs with spectrum animation
- **WiFi + WebSocket** — web interface with live VU meter
- **BLE GATT** — remote control via Chrome (Android) — `TapeControl_BLE.html`
- **MQTT + Home Assistant** — auto-discovery for all entities
- **OTA** — WiFi firmware update from Arduino IDE (hostname: `TapeDeck`)
- **FreeRTOS** — dual-core: Core 0 = WiFi/BLE/MQTT/FFT, Core 1 = VFD/OLED/solenoids/encoder
- **IR receiver** — NEC protocol remote control
- **Encoder** — menu navigation with click/double-click/triple-click/long-press

---

## Hardware

| Component | Details |
|-----------|---------|
| MCU | ESP32-S3-DevKitC-1 |
| VFD | Pioneer CT-737 original display (3× shift register) |
| ADC | PCM1808 (I2S stereo, 24-bit) |
| I/O expander | PCF8574 (I2C, 0x27) |
| Display | 128×64 OLED (I2C) |
| LED strip | WS2812B, 40 LEDs |
| Transport | 4× solenoids (PLAY/STOP/REW/FWD) |
| Encoder | Rotary encoder with push button |
| IR | 38 kHz IR receiver |
| Audio | Speaker 64 Ω + 100 Ω resistor (buzzer) |

---

## GPIO Pinout

| GPIO | Name | Function |
|------|------|----------|
| 1 | PIN_AUDIO_L | Analog left channel input |
| 2 | PIN_AUDIO_R | Analog right channel input |
| 4 | latchPins[2] | VFD Latch chip 3 |
| 5 | latchPins[1] | VFD Latch chip 2 |
| 6 | latchPins[0] | VFD Latch chip 1 |
| 7 | PIN_DATA | VFD Data (shift register) |
| 9 | OUT_STOP | Solenoid STOP |
| 10 | OUT_PLAY | Solenoid PLAY |
| 11 | OUT_REWIND | Solenoid REWIND |
| 12 | OUT_FORWARD | Solenoid FORWARD |
| 13 | ENC_CLK | Encoder CLK |
| 14 | ENC_DT | Encoder DT |
| 15 | PIN_CK | VFD Clock (shift register) |
| 16 | PIN_BRIGHTNESS_PWM | VFD backlight PWM |
| 17 | PCF_I2C_SDA | I2C SDA (OLED + PCF8574) |
| 18 | PCF_I2C_SCL | I2C SCL (OLED + PCF8574) |
| 0 | PIN_RESET | Encoder button (INPUT_PULLUP) |
| 21 | PIN_IR | IR receiver |
| 38 | G_SOURCE | Source mode output |
| 39 | PCM1808 BCK | I2S bit clock |
| 40 | PCM1808 LRC | I2S left/right clock |
| 41 | PCM1808 DOUT | I2S data |
| 42 | PCM1808 SCK | I2S master clock |
| 45 | PIN_BUZZER | Speaker (64 Ω + 100 Ω) |
| 46 | PIN_PULSE | Tape counter pulses |
| 48 | LED_PIN | WS2812B LED strip |

---

## Arduino IDE Setup

> ⚠️ **CRITICAL**: Board must be configured with **PSRAM: QSPI PSRAM** enabled. Without PSRAM the firmware will crash on boot.

**Board settings:**
- Board: `ESP32S3 Dev Module`
- PSRAM: `QSPI PSRAM` ← mandatory
- Flash size: 16MB (or match your module)
- Partition scheme: Huge APP (3MB No OTA / 1MB SPIFFS) or custom
- Upload speed: 921600

**OTA (WiFi upload):**  
After first flash via USB, the board appears in Arduino IDE as a network port named **`TapeDeck`**. Select it for subsequent uploads over WiFi.

---

## Required Libraries

Install via Arduino Library Manager or ZIP:

| Library | Purpose |
|---------|---------|
| `ESP32` core (Espressif) | Board support |
| `WiFi` | Built-in with ESP32 core |
| `ESPAsyncWebServer` | Async web server + WebSocket |
| `AsyncTCP` | Required by ESPAsyncWebServer |
| `PubSubClient` | MQTT client |
| `ArduinoJson` | JSON for MQTT payloads |
| `NimBLE-Arduino` | BLE GATT server |
| `FastLED` | WS2812B LED strip |
| `arduinoFFT` | FFT spectrum analyzer |
| `Adafruit SSD1306` | OLED display |
| `Adafruit GFX` | Graphics for OLED |
| `PCF8574` | I2C I/O expander |
| `IRremoteESP8266` | IR receiver (NEC protocol) |
| `ArduinoOTA` | WiFi OTA updates |

---

## Configuration

Edit these defines at the top of `ble_wifi_control_v12_80.ino`:

```cpp
#define WIFI_SSID     "your_wifi_ssid"
#define WIFI_PASSWORD "your_wifi_password"
#define MQTT_SERVER   "192.168.1.x"   // Home Assistant IP
#define MQTT_PORT     1883
#define MQTT_USER     "your_mqtt_user"
#define MQTT_PASSWORD "your_mqtt_password"
```

---

## Home Assistant Integration

### MQTT Auto-Discovery

The firmware publishes HA auto-discovery messages on boot. All entities appear automatically under device **"Tape Deck"**.

**Transport buttons** (entity IDs):
- `button.tape_deck_tape_play`
- `button.tape_deck_tape_stop`
- `button.tape_deck_tape_rew`
- `button.tape_deck_tape_fwd`

**Sensors:** tape counter, VU level L/R, transport state, tape type, OLED style, brightness, BLE status, etc.

### Custom Lovelace Card

1. Copy `akai-tape-card.js` to `/config/www/` on your HA instance
2. In HA → Settings → Dashboards → Resources, add:
   ```
   /local/akai-tape-card.js
   ```
   Type: JavaScript module
3. Add card in your dashboard:
   ```yaml
   type: custom:akai-tape-card
   ```
4. Hard refresh the browser / clear cache in HA mobile app

The card shows animated VFD-style display with transport controls and real-time status indicators.

---

## BLE Remote Control

Open `TapeControl_BLE.html` in **Chrome on Android** (Chrome desktop doesn't support Web Bluetooth).

Controls available: PLAY, STOP, REW, FWD, volume, brightness, OLED style selection.

---

## VFD Details

Three shift-register chips, 24 bits each (72 bits total):

- **Chip 1** — 4× 7-segment digits (tape counter) + level scale indicators
- **Chip 2** — transport indicators (PLAY/PAUSE/FWD/REW), tape type (Metal/CrO2/Norm), Dolby B/C/NR, Filter, right channel VU
- **Chip 3** — left channel VU (16 segments), REC/Mute/Tape/Source indicators

---

## Project Structure

```
pioneer-ct737-esp32/
├── ble_wifi_control_v12_80.ino   # Main firmware (ESP32-S3, Arduino IDE)
├── akai-tape-card.js             # Home Assistant custom Lovelace card
├── TapeControl_BLE.html          # BLE remote control (Chrome Android)
└── README.md
```

---

## License

MIT License — free to use, modify, and distribute.

---

## Author

Developed for Pioneer CT-737 restoration project.  
ESP32-S3 controller replacing original transport logic with WiFi/BLE/MQTT control.
