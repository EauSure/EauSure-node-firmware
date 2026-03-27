# IoT Water Quality Monitoring System with LoRa

Real-time water quality monitoring using FreeRTOS, ESP32, and secure LoRa communication.

## System Overview

**Two-node architecture:**
- **Measurement Node**: Sensor platform reading pH, TDS, turbidity, temperature, and acceleration
- **Gateway Node**: Central receiver collecting encrypted data, validating packets, and logging alerts

Both nodes communicate via encrypted LoRa (433 MHz) with AES-128-GCM authenticated encryption.

## Architecture

```
Measurement Node (ESP32-S3)          Gateway Node (ESP32)
├─ Water Quality Sensors             ├─ LoRa Receiver
├─ MPU6050 Accelerometer            ├─ SD Card Logger
├─ INA219 Power Monitor              ├─ Audio Alerts
├─ OLED Display                      └─ Serial Console
└─ FreeRTOS Task Scheduler
    ↓ (LoRa Encrypted Link)
    ↓
    Encrypted JSON Payload
```

## Data Flow

1. **Measurement Node** reads sensors every 60 seconds
2. Serializes to compact JSON (~135 bytes)
3. Encrypts with AES-128-GCM and transmits via LoRa
4. **Gateway Node** receives, decrypts, and validates authentication tag
5. Parses JSON and displays results on console
6. Stores data to SD card
7. Triggers audio alerts for threshold violations

## Encryption Protocol

- **Algorithm**: AES-128-GCM (Galois/Counter Mode)
- **Nonce**: 12 bytes (Device ID + Sequence + Random)
- **Authentication Tag**: 16 bytes (full strength, NIST approved)
- **Frame Structure**: [Header (24B)] [Nonce] [Ciphertext] [Auth Tag (16B)] [CRC (2B)]

## JSON Format (Compact)

**Sensor Data Example:**
```json
{"b": 85, "v": 3.9, "m": 150, "p": 6.8, "ps": 8, "t": 280, "ts": 7, "u": 2.1, "us": 6, "tw": 22.5, "tm": 45.2, "te": 38.1, "e": "None"}
```

**Shake Event Example:**
```json
{"e": "ALARM_SHAKE", "ag": 2.45, "dg": 1.45}
```

**Field Reference:**
| Field | Description | Unit |
|-------|-------------|------|
| b | Battery percentage | % |
| v | Battery voltage | V |
| m | Battery current | mA |
| p | pH value | - |
| ps | pH score | 0-10 |
| t | TDS concentration | ppm |
| ts | TDS score | 0-10 |
| u | Turbidity voltage | V |
| us | Turbidity score | 0-10 |
| tw | Water temperature | °C |
| tm | MPU temperature | °C |
| te | ESP32 temperature | °C |
| e | Event type | - |
| ag | Acceleration magnitude (shake) | G |
| dg | Dynamic acceleration (shake) | G |

## Configuration

All environment variables (keys, device IDs, pins) are stored in `config.h` files:
- `IoT_Node/config.h` - Measurement node configuration
- `Gateway_Node/config.h` - Gateway configuration

These files are git-ignored for security. Use the `.template` files as reference.

## Build & Deploy

1. Open each `.ino` file in Arduino IDE 2.0+
2. Configure board: ESP32-S3 for measurement node, ESP32 DevKit for gateway
3. Verify dependencies installed (LoRa, ArduinoJson, Wire, SPI)
4. Upload to respective boards

## Sensors & Hardware

**Measurement Node:**
- ESP32-S3 microcontroller
- SX1276 LoRa transceiver (433 MHz)
- MPU6050 6-axis accelerometer
- INA219 power monitor
- DS18B20 temperature probe (1-Wire)
- Analog sensors: pH, TDS, turbidity (0-3.3V ADC)
- 128×64 OLED display
- RGB LED indicator

**Gateway Node:**
- ESP32 DevKit microcontroller
- SX1276 LoRa transceiver (433 MHz)
- Micro-SD card adapter for data logging

## Serial Output Example

```
[LoRa] init ok
[SENSORS TASK] Reading sensors...
[SEND_JSON] JSON length: 135
[SEC TX] seq=42 attempt=1
[SEC ACK] seq=42

========= NOUVELLES DONNEES EAU (SECURE) =========
SEQ         : 42
Signal LoRa : RSSI -45 dBm | SNR 11.2 dB
--------------------------------------------------
BATTERIE    : 85% | 3.90 V | 150 mA
pH          : 6.82 | Score: 8/10
TDS         : 280 ppm | Score: 7/10
TURBIDITE   : 2.10 V | Score: 6/10
TEMP. EAU   : 22.5 °C
TEMP. CARTE : MPU 45.2 °C
TEMP. ESP32 : S3 38.1 °C
EVENT       : None
==================================================
```

## Performance Metrics

- **Payload size**: ~135 bytes (fits within 180-byte LoRa limit with 45-byte safety margin)
- **Transmission interval**: 60 seconds (sensor readings)
- **LoRa spreading factor**: SF7 (~100ms per frame)
- **ACK round-trip**: <500ms typical
- **Range**: 10+ km line-of-sight (LoRa capability)
- **Power consumption**: ~200mA active (measurement node)

## Development Notes

- FreeRTOS runs sensor reading and LoRa TX/RX as separate tasks
- Mutex synchronization prevents concurrent sensor access
- Sequence numbers detect duplicate frames and out-of-order delivery
- GCM authentication tags verify packet integrity and prevent tampering
- All timestamps UTC-relative (milliseconds since boot)

## License & Attribution

This is a project implementation for water quality monitoring research.
