# IoT Water Quality Monitoring System with LoRa

Real-time water quality monitoring system using FreeRTOS, ESP32, and LoRa communication with end-to-end encryption.

## Overview

This project implements a distributed IoT water quality monitoring system with two main components:

- **IoT Measurement Node**: Autonomous sensor platform that reads water quality parameters and transmits encrypted data via LoRa
- **Gateway Node**: Central receiver that decrypts messages, validates authenticity, logs data, and triggers alerts

The system demonstrates real-time embedded systems design, cryptographic protocols, wireless communication, and FreeRTOS task management on resource-constrained devices.

### Key Features

- 🌊 **Multi-parameter monitoring**: pH, Total Dissolved Solids (TDS), turbidity, water temperature
- 📡 **Long-range LoRa communication**: 433 MHz ISM band, up to 10+ km line-of-sight
- 🔒 **End-to-end encryption**: AES-128 in ECB mode with HMAC-256 authentication
- 🎛️ **Real-time OS**: FreeRTOS task scheduling on ESP32/ESP32-S3
- 🎵 **Alert system**: Audio notifications on gateway for threshold violations
- 💾 **Data logging**: SD card storage with timestamp indexing
- 📊 **Live display**: OLED screen on sensor node
- 🔋 **Power monitoring**: Battery voltage, current (mA), and percentage tracking
- 🌐 **Event detection**: Accelerometer-based shake/motion detection

### Technical Stack

- **Hardware**: ESP32-S3 (sensor node), ESP32 DevKit (gateway), SX1276 LoRa module
- **OS**: FreeRTOS with dual-core support
- **Communication**: LoRa (Arduino library v0.8.0)
- **Serialization**: ArduinoJson (v7.4.3+)
- **Build System**: Arduino IDE 2.0+

## System Design

### Architecture

```
┌─────────────────────────────────────────────────────────┐
│                   IoT Measurement Node                   │
│                    (ESP32-S3 + LoRa)                    │
├─────────────────────────────────────────────────────────┤
│ Sensors:                                                 │
│  • Analog water quality (pH, TDS, turbidity)            │
│  • DS18B20 temperature probe (1-Wire)                   │
│  • MPU6050 accelerometer (I2C)                          │
│  • INA219 power monitor (I2C)                           │
│                                                          │
│ Processing:                                              │
│  • FreeRTOS multitask execution                         │
│  • Sensor fusion & calibration                          │
│  • JSON serialization (~135 bytes)                      │
│  • AES-128 ECB encryption                               │
│  • HMAC-256 authentication                              │
│                                                          │
│ Output:                                                  │
│  • LoRa transmission (spreading factor 7)               │
│  • OLED status display                                  │
│  • RGB LED indicators                                   │
└────────────┬────────────────────────────────────────────┘
             │
             │ LoRa Link (433 MHz)
             │ • Encrypted payload
             │ • Sequence number
             │ • Request-ACK handshake
             │
┌────────────▼────────────────────────────────────────────┐
│                    Gateway Node                          │
│                 (ESP32 DevKit + LoRa)                   │
├─────────────────────────────────────────────────────────┤
│ Reception:                                               │
│  • LoRa RX with CRC validation                          │
│  • HMAC verification                                    │
│  • AES-128 ECB decryption                               │
│  • Sequence tracking & deduplication                    │
│                                                          │
│ Processing:                                              │
│  • JSON parsing                                         │
│  • Threshold evaluation                                 │
│  • Alert generation                                     │
│  • Data formatting                                      │
│                                                          │
│ Output:                                                  │
│  • ACK transmission                                     │
│  • Serial console logging                               │
│  • SD card persistent storage                           │
│  • Audio alert playback                                 │
└─────────────────────────────────────────────────────────┘
```

### Communication Protocol

**LoRa Frame Structure**:
```
[IV (8B)] [Encrypted Payload] [HMAC (8B)]
```

**Encrypted Payload** (AES-128 ECB):
```
[Device ID (4B)] [Seq# (1B)] [JSON (~120B)] [Padding]
```

**Transmission Cycle**:
1. IoT node encrypts JSON payload with IV
2. Computes HMAC-256 over entire message
3. Sends via LoRa (1-2 seconds for SF=7)
4. Waits for ACK (800ms timeout, 3 retries)
5. Returns to RX mode between transmissions

**Security Properties**:
- Prevents message tampering (HMAC)
- Prevents eavesdropping (AES-128)
- Includes device identification (Device ID)
- Detects replay attacks (sequence numbers)
- Random IV per message (collision resistance)

### FreeRTOS Task Design

| Task | Core | Priority | Period | Stack | Purpose |
|------|------|----------|--------|-------|---------|
| SensorsTask | 0 | 2 | 60000ms | 4096B | Read analog sensors, serialize JSON, transmit via LoRa |
| MPUTask | 0 | 3 | 50ms | 2048B | Poll accelerometer, detect shake events |
| DisplayTask | 1 | 1 | 100ms | 2048B | Update OLED screen with latest readings |
| uartTask | 0 | 1 | - | 2048B | Serial console logging |

**Mutex Usage**:
- `gDataMutex`: Protects `gSensorData` structure (10000ms timeout)
- `gEventMutex`: Protects `gEventState` (10000ms timeout)
- `gLoRaMutex`: Protects LoRa radio state (5000ms timeout)

**Task Synchronization**:
```
SensorsTask:
  1. Acquire gDataMutex
  2. Read ADC (analog sensors)
  3. Read INA219 current
  4. Trigger DS18B20 measurement
  5. Release gDataMutex
  6. Create JSON from gSensorData
  7. Acquire gLoRaMutex
  8. Encrypt & transmit payload
  9. Wait for ACK
  10. Release gLoRaMutex
```

## JSON Payload Format

Optimized for <180 byte LoRa transmission limit:

```json
{
  "b": 75,          // Battery: percentage (0-100)
  "v": 4.15,        // Battery: voltage (volts)
  "m": 125,         // Battery: current (mA)
  "p": 6.93,        // pH: value (0-14)
  "ps": 10,         // pH: quality score (0-10)
  "t": 450,         // TDS: ppm (0-2000)
  "ts": 8,          // TDS: quality score (0-10)
  "u": 2.41,        // Turbidity: voltage (0-5V)
  "us": 4,          // Turbidity: quality score (0-10)
  "tw": 17.5,       // Temperature: water (°C)
  "tm": 37.8,       // Temperature: MPU6050 (°C)
  "te": 38.2,       // Temperature: ESP32-S3 (°C)
  "e": "None"       // Event: "None", "ALARM_SHAKE", etc.
}
```

**Size Optimization**:
- Field names: 1-2 characters (vs 8-10 in verbose version)
- Numeric values: 1-4 bytes when serialized
- Payload: ~135 bytes (88% efficient)
- Safety margin: 45 bytes to 180-byte limit

## Configuration System

All deployment parameters are defined in header files:

**IoT_Node/config.h** (included by IoT_Node.ino, app_state.h, lora_radio.cpp):
```cpp
// Device identification
static const uint32_t DEVICE_ID = 0x...;

// Security keys (MUST match gateway)
static const uint8_t ENC_KEY[16] = {...};
static const uint8_t HMAC_KEY[16] = {...};

// LoRa parameters (MUST match gateway)
static const long LORA_FREQ = 433E6;
static const int LORA_SF = 7;

// GPIO pins (hardware-specific)
static const int LORA_NSS = 5;
// ... more pins

// Sensor configuration
static const float PH_MIN = 6.5;
static const float PH_MAX = 8.5;
// ... more thresholds
```

**Gateway_Node/config.h** (included by Gateway_Node.ino):
- Must have identical DEVICE_ID, ENC_KEY, HMAC_KEY, LoRa parameters
- Contains additional gateway-specific thresholds
- SD card, audio, and alert parameters

**Templates Provided**:
- `IoT_Node/config.h.template`: Reference configuration for sensor node
- `Gateway_Node/config.h.template`: Reference configuration for gateway
- Actual `config.h` files are ignored by .gitignore for security

**Key Synchronization Requirement**:
Single-byte difference in ENC_KEY or HMAC_KEY between nodes causes message authentication failure. Both nodes must use identical keys.

## Security Implementation

### Cryptographic Architecture

**Encryption & Authentication Mode: AES-128-GCM**
- **Algorithm**: Galois/Counter Mode (GCM) authenticated encryption
- **Key Size**: 128-bit (16 bytes)
- **Nonce**: 96-bit (12 bytes) per message
- **Authentication Tag**: 128-bit (16 bytes) full-strength
- **Mode**: Provides both confidentiality AND authenticity in single operation

GCM was chosen over older modes (ECB, CBC) because it:
1. Eliminates need for separate HMAC (combined authenticated encryption)
2. Provides stronger authentication guarantees
3. Detects both ciphertext tampering AND message forgery
4. Better hardware support on embedded processors
5. Constant-time operations reduce timing attack surface

### Cryptographic Functions

**AES-128-GCM Encryption**:
```cpp
bool aesgcmEncrypt(
  const uint8_t *plaintext,     // Input message
  size_t plainLen,              // Message length
  const uint8_t nonce[12],      // 96-bit nonce (IV)
  const uint8_t *aad,           // Additional Authenticated Data (header)
  size_t aadLen,                // AAD length
  uint8_t *ciphertext,          // Output encrypted data
  uint8_t tag[16]               // Output authentication tag
);
// Encrypts plaintext and produces authentication tag
// AAD (header) is authenticated but not encrypted
```

**AES-128-GCM Decryption**:
```cpp
bool aesgcmDecrypt(
  const uint8_t *ciphertext,    // Encrypted message
  size_t cipherLen,             // Message length
  const uint8_t nonce[12],      // Same 96-bit nonce used during encryption
  const uint8_t *aad,           // Same Additional Authenticated Data (header)
  size_t aadLen,                // AAD length
  const uint8_t tag[16],        // Received authentication tag
  uint8_t *plaintext            // Output decrypted data
);
// Decrypts ciphertext and verifies authentication tag
// Returns false if tag verification fails
```

**CRC-16 (Physical Layer)**:
```cpp
uint16_t crc16Ccitt(uint8_t *data, int len);
// Detects random transmission errors at PHY layer (independent of crypto)
```

### Frame Structure (GCM Protocol)

```
[Nonce (12B)] [Ciphertext] [Auth Tag (16B)] [CRC (2B)]
     ↓             ↓              ↓             ↓
   IV-based    [Plaintext]   Verify Frame   Error Detection
   Counter     + Optional AAD  Integrity
```

**Encryption Process**:
1. Generate 96-bit nonce (Device ID + Sequence + Random)
2. Encrypt plaintext with AES-128-GCM using nonce
3. Compute 128-bit authentication tag over ciphertext + header
4. Append tag to frame
5. Compute CRC-16 over entire encrypted frame
6. Transmit: [Header+Nonce] [Ciphertext] [Tag] [CRC]

**Decryption Process**:
1. Verify CRC-16 (detect PHY errors)
2. Extract nonce, ciphertext, and tag
3. Decrypt with AES-128-GCM
4. Verify authentication tag automatically
5. Return plaintext if tag verification passes
6. Return error if tag verification fails

### Key Synchronization Requirement

Single-byte difference in ENC_KEY between nodes causes ALL messages to fail authentication. Both nodes must use IDENTICAL keys:
```cpp
// Both IoT_Node and Gateway_Node config.h MUST have:
static const uint8_t ENC_KEY[16] = {
  0xAA, 0xBB, ... // SAME VALUES
};
```

### Security Properties

**GCM Provides**:
- ✅ **Confidentiality**: Ciphertext reveals nothing about plaintext (IND-CPA security)
- ✅ **Authenticity**: Detects any bit-level tampering (INT-CTXT security)
- ✅ **Integrity**: Combined encryption-authentication prevents forgery
- ✅ **Replay Detection**: Sequence numbers + device ID prevent replayed packets
- ✅ **Uniqueness**: Random nonce component prevents message pattern recognition

**Additional Protections**:
- ✅ **Device Identification**: Device ID embedded in frame (prevents spoofing)
- ✅ **Sequence Tracking**: Sequence numbers prevent replay attacks
- ✅ **Physical Layer CRC**: Independent error detection at PHY layer
- ✅ **Constant-Time Operations**: mbedTLS GCM implementation resists timing attacks

### Threat Model

**Protected Against**:
- ✅ **Passive Eavesdropping**: Ciphertext is encrypted with AES-128
- ✅ **Message Tampering**: GCM tag detects any bit-level modifications
- ✅ **Replay Attacks**: Sequence numbers prevent old messages being replayed
- ✅ **Device Spoofing**: Device ID + sequence + tag prevent impersonation
- ✅ **Message Forgery**: GCM provides unforgeability (INT-CTXT)
- ✅ **Bit Corruption**: Both CRC and GCM tag detect errors

**NOT Protected Against**:
- ❌ **Key Compromise**: If ENC_KEY is exposed, system is completely compromised
- ❌ **Endpoint Compromise**: If a device is physically captured and firmware dumped
- ❌ **Gateway Compromise**: If gateway code is modified, it can decrypt/modify messages
- ❌ **Brute Force**: AES-128 has 2^128 possible keys (computationally infeasible to brute-force)
- ❌ **Side-Channel Attacks**: Timing analysis, power analysis, electromagnetic analysis (with sufficient equipment and access)

**Deployment Recommendations**:
1. Generate unique keys per deployment (use `openssl rand -hex 16`)
2. Store keys securely, never in version control
3. Physical security: Encapsulate nodes in tamper-evident housings
4. Key rotation: Change keys periodically (quarterly)
5. Monitoring: Alert on repeated authentication failures

## Debugging & Diagnostics

### Serial Console Output

**IoT Node** (sample output):
```
[LoRa] init ok
[SENSORS TASK] Calling readSensorsRoutine (initial)
[SENSORS TASK] Reading sensors...
[SEND_JSON] Starting JSON creation
[SEND_JSON] JSON: {"b":75,"v":4.15,"m":125,...}
[SEC_SEND] Received JSON of length: 135
[SEC_SEND] First 100 chars: {"b":75,"v":4.15,"m":125...
[SEC] TX payload: 135 bytes, IV: 0x1a2b3c4d5e6f7a8b
[SEC TX] seq=1 attempt=1
[SEC WAIT ACK] got packet len=30 seq=1
[SEC ACK] seq=1
```

**Gateway Node** (sample output):
```
Waiting for secure telemetry data...
[ACK] seq=1 sent=yes
========= NOUVELLES DONNEES EAU (SECURE) =========
SEQ         : 1
BATTERIE    : 75% | 4.15 V | 125 mA
pH          : 6.93 | Score: 10/10
TDS         : 450 ppm | Score: 8/10
TURBIDITE   : 2.41 V | Score: 4/10
TEMP. EAU   : 17.5 °C
TEMP. CARTE : MPU 37.8 °C
TEMP. ESP32 : S3 38.2 °C
EVENT       : None
==================================================
```

### Common Issues & Diagnosis

**Issue: Gateway doesn't receive messages**
- Check: DEVICE_ID, ENC_KEY, HMAC_KEY match exactly
- Check: LoRa frequency, SF, BW, CR parameters identical
- Check: Antenna connections; try increasing SF to 10-12
- Logs: Look for `[SEC] invalid plaintext length` (payload too large)

**Issue: Periodic measurements not transmitting (shake events work)**
- Root cause: JSON payload >180 bytes
- Solution: Check field names, remove unnecessary fields
- Logs: `[SEC] invalid plaintext length: XXX (max: 180)`

**Issue: Garbled serial output**
- Check: Baud rate is 115200
- Check: USB cable and connection
- Try: Different USB port on computer

**Issue: Sensor readings stuck/not updating**
- Check: Mutex timeout insufficient (default 10000ms)
- Check: Sensor I2C/1-Wire communication errors
- Logs: `[SENSORS TASK] Attempting to acquire gDataMutex... TIMEOUT`

## Performance Characteristics

- **Sensor Read Time**: 4.2 seconds (DS18B20 conversion + analog settling)
- **JSON Serialization**: <1ms
- **Encryption Time**: ~5ms (AES-128 + HMAC-256)
- **LoRa Transmission**: 1.2 seconds (SF=7, 135 bytes)
- **ACK Round-trip**: 800ms timeout
- **Periodic Interval**: 60 seconds
- **Payload Efficiency**: 135/180 = 75%

**Power Consumption** (estimated):
- IoT Node sleep: ~50 mA
- IoT Node during transmission: ~500 mA
- LoRa module active RX: ~15 mA
- Gateway node active: ~200-300 mA

## File Structure

```
MyFreeRTOSProject/
├── IoT_Node/
│   ├── IoT_Node.ino               # Firmware entry point
│   ├── config.h                   # Configuration (git-ignored)
│   ├── config.h.template          # Reference template
│   ├── app_state.h                # Global state structs
│   ├── app_state.cpp              # State initialization
│   ├── lora_radio.h               # LoRa API
│   ├── lora_radio.cpp             # LoRa implementation (encryption, TX)
│   ├── task_sensors.h             # Sensor reading task
│   ├── task_sensors.cpp           # Sensor reads, JSON creation
│   ├── task_mpu.h                 # Accelerometer task
│   ├── task_mpu.cpp               # MPU6050, shake detection
│   ├── task_display.h             # OLED display task
│   ├── task_display.cpp           # Display rendering
│   ├── display_oled.h             # Display utilities
│   └── display_oled.cpp           # Graphics functions
│
├── Gateway_Node/
│   ├── Gateway_Node.ino           # Firmware entry point
│   ├── config.h                   # Configuration (git-ignored)
│   ├── config.h.template          # Reference template
│   └── (single-file firmware)
│
├── .gitignore                     # Ignores config.h, .vscode
├── README.md                      # This file
└── LICENSE                        # MIT License
```

## Development History

**Key Design Decisions**:

1. **LoRa Protocol**: Request-ACK handshake ensures delivery; automatic retries recover from packet loss
2. **JSON Field Names**: Short names (1-2 chars) chosen over readability to fit 180-byte limit
3. **FreeRTOS Multitasking**: Separates sensor I/O, LoRa communication, and display updates to meet real-time constraints
4. **Centralized Config**: Single config.h per node avoids duplicate constant definitions
5. **Encryption in Firmware**: AES-128 ECB + HMAC-256 computed on device; IV generated per message
6. **Dual-Core Design**: Sensor reading + display on Core 0; system tasks on Core 1

**Evolution**:
- v1: Initial prototype with basic sensor reading and LoRa transmission
- v2: Added AES-128 encryption and HMAC authentication
- v3: Implemented FreeRTOS task management
- v4: Added battery current monitoring and ESP32 temperature
- v5: Optimized JSON payload (224→135 bytes) to fit LoRa limit
- v6: Centralized configuration system

## Known Limitations

1. **Single IoT Node**: Gateway designed for one sensor node; would need sequence tracking for multiple nodes
2. **No OTA Updates**: Firmware updates require physical access and USB upload
3. **Limited Storage**: OLED only shows current readings; no history on display
4. **Fixed LoRa SF**: Spreading Factor not adaptive to link quality
5. **No Power Management**: Node doesn't sleep between transmissions; assumes external power or large battery
6. **Manual Key Management**: Keys must be synchronized manually between nodes

## Future Enhancements

- [ ] Multi-node support (multiple sensors → single gateway)
- [ ] Adaptive LoRa spreading factor
- [ ] Mobile app for remote monitoring
- [ ] Web dashboard with historical graphs
- [ ] OTA firmware updates
- [ ] Cloud data synchronization
- [ ] Low-power sleep modes
- [ ] Solar power integration
- [ ] GPS location tracking
- [ ] Temperature-compensated calibration

---

**Author**: Water Quality IoT Project  
**License**: MIT  
**Repository**: Personal research project