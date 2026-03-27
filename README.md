# IoT Water Quality Monitoring System with LoRa

Real-time water quality monitoring using FreeRTOS, ESP32, and LoRa communication with end-to-end encryption.

## About The Project

A comprehensive IoT system that monitors water quality parameters (pH, TDS, turbidity, temperature) using distributed sensor nodes with secure LoRa communication to a central gateway. Features real-time alerts, SD card logging, and audio notifications.

**Key Features:**
- 🌊 Multi-parameter water quality monitoring (pH, TDS, turbidity, temperature, battery)
- 📡 Long-range LoRa communication (up to 10+ km in optimal conditions)
- 🔒 End-to-end encryption (AES-128 + HMAC-256)
- 🎛️ FreeRTOS real-time task management
- 🎵 Audio alert system on gateway
- 💾 SD card data logging
- 📊 Real-time OLED display on sensor node
- 🔋 Battery monitoring and optimization
- 🌐 Shake detection (accelerometer-based event triggering)

### Built With

* [![Arduino][Arduino.cc]][Arduino-url]
* [![ESP32][ESP32]][ESP32-url]
* [![FreeRTOS][FreeRTOS]][FreeRTOS-url]
* [![LoRa][LoRa]][LoRa-url]
* [![ArduinoJson][ArduinoJson]][ArduinoJson-url]

<p align="right">(<a href="#readme-top">back to top</a>)</p>

## Getting Started

### Prerequisites

- Arduino IDE 2.0 or later
- ESP32 Dev Kit or ESP32-S3 board
- LoRa module (SX1276 or similar)
- Water quality sensors (pH, TDS, Turbidity)
- DS18B20 temperature sensor
- MPU6050 accelerometer
- INA219 current monitor
- OLED display (SSD1306)
- NeoPixel RGB LED
- SD card module (for gateway)

### Installation

1. **Clone the repository**
   ```sh
   git clone https://github.com/your-username/MyFreeRTOSProject.git
   cd MyFreeRTOSProject
   ```

2. **Install Arduino libraries** via Arduino IDE Library Manager:
   - LoRa (Arduino)
   - Adafruit GFX Library
   - Adafruit SSD1306
   - Adafruit NeoPixel
   - Adafruit INA219
   - ArduinoJson (v7.4.3+)
   - OneWire
   - DallasTemperature

3. **Configure your deployment**
   ```sh
   # IoT_Node/config.h
   cp IoT_Node/config.h.template IoT_Node/config.h
   # Edit IoT_Node/config.h with your parameters
   
   # Gateway_Node/config.h
   cp Gateway_Node/config.h.template Gateway_Node/config.h
   # Edit Gateway_Node/config.h with your parameters
   ```

4. **Generate security keys** (IMPORTANT!)
   ```bash
   # Generate AES encryption key
   openssl rand -hex 16
   
   # Generate HMAC authentication key
   openssl rand -hex 16
   ```
   
   Update both `config.h` files with identical keys.

5. **Upload firmware**
   - Open `IoT_Node/IoT_Node.ino` in Arduino IDE
   - Select board: ESP32-S3 (or your board)
   - Upload to first ESP32 board
   - Repeat for `Gateway_Node/Gateway_Node.ino` using ESP32 DevKit

6. **Verify communication**
   - Open Serial Monitor at 115200 baud
   - Should see periodic sensor readings and transmissions

<p align="right">(<a href="#readme-top">back to top</a>)</p>

## Usage

### IoT Measurement Node

Automatically:
- Reads sensors every 60 seconds
- Sends data via encrypted LoRa to gateway
- Displays readings on OLED screen
- Triggers alerts on shake detection
- Monitors battery status

Serial output shows:
```
[SENSORS TASK] Attempting to acquire gDataMutex... SUCCESS
[SENSORS TASK] Reading sensors...
[SEND_JSON] JSON delivery successful
[SEC ACK] seq=2
```

### Gateway Node

Automatically:
- Receives encrypted messages from IoT node
- Sends ACK confirmation
- Logs data to SD card
- Displays readings on serial output
- Plays audio alerts on threshold violations

Serial output shows:
```
========= NOUVELLES DONNEES EAU (SECURE) =========
SEQ         : 2
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

### Configuration

All settings are in `config.h` files:

**Critical (must match between nodes):**
- `DEVICE_ID`: Node identification
- `ENC_KEY`: AES encryption key (16 bytes)
- `HMAC_KEY`: HMAC authentication key (16 bytes)
- LoRa parameters: frequency, spreading factor, bandwidth, coding rate

**Optional:**
- Sensor thresholds (Gateway only)
- Timing parameters
- GPIO pins (hardware-specific)

See `IoT_Node/config.h` and `Gateway_Node/config.h` for complete reference.

<p align="right">(<a href="#readme-top">back to top</a>)</p>

## System Architecture

```
┌─────────────────────────────────────────────────────────┐
│                   IoT Measurement Node                   │
│                    (ESP32-S3 + LoRa)                    │
├─────────────────────────────────────────────────────────┤
│ • Water Quality Sensors (pH, TDS, Turbidity)            │
│ • Temperature Sensors (Water + MPU6050)                 │
│ • Battery Monitor (INA219)                              │
│ • Accelerometer (MPU6050 - Shake Detection)             │
│ • OLED Display                                           │
│ • RGB LED Status Indicator                              │
└────────────┬────────────────────────────────────────────┘
             │
             │  Encrypted LoRa
             │  AES-128 + HMAC-256
             │
┌────────────▼────────────────────────────────────────────┐
│                    Gateway Node                          │
│                 (ESP32 DevKit + LoRa)                   │
├─────────────────────────────────────────────────────────┤
│ • LoRa Receiver                                          │
│ • Message Decryption & Verification                     │
│ • SD Card Data Logger                                   │
│ • Audio Alert System                                    │
│ • Serial Monitor Display                                │
└─────────────────────────────────────────────────────────┘
```

## JSON Payload Format

Optimized for LoRa transmission (<180 bytes):

```json
{
  "b": 75,          // Battery percentage
  "v": 4.15,        // Battery voltage
  "m": 125,         // Battery current (mA)
  "p": 6.93,        // pH value
  "ps": 10,         // pH score (0-10)
  "t": 450,         // TDS ppm
  "ts": 8,          // TDS score (0-10)
  "u": 2.41,        // Turbidity voltage
  "us": 4,          // Turbidity score (0-10)
  "tw": 17.5,       // Water temperature
  "tm": 37.8,       // MPU temperature
  "te": 38.2,       // ESP32 temperature
  "e": "None"       // Event type
}
```

## Security

### Encryption & Authentication
- **Algorithm**: AES-128 (ECB mode) + HMAC-256
- **Key Management**: Centralized in config.h
- **Initialization Vector**: Random 8-byte IV per message
- **Authentication**: 8-byte HMAC truncated

### Best Practices
1. Generate new keys for each deployment
2. Add `config.h` to `.gitignore` to prevent accidental commits
3. Store keys securely (password manager, HSM, etc.)
4. Rotate keys periodically
5. Verify board firmware integrity before deployment

### .gitignore Configuration
```
# Security - Never commit configuration files
*/config.h
.vscode/
*.pem
*.key
```

<p align="right">(<a href="#readme-top">back to top</a>)</p>

## Troubleshooting

### Gateway doesn't receive data
- Verify `DEVICE_ID` matches between both nodes
- Check `ENC_KEY` and `HMAC_KEY` are identical
- Confirm LoRa frequency matches
- Check antenna connections and positioning
- Look for `[SEC] invalid plaintext length` errors (payload too large)

### Communication range too short
- Increase Spreading Factor: `LORA_SF = 10` or `12`
- Reduce Bandwidth: `LORA_BW = 62.5E3`
- Check antenna positioning (vertical orientation optimal)
- Remove obstacles between nodes

### Periodic sensor measurements not transmitting
- Check JSON payload size (<180 bytes)
- Verify mutex isn't timing out (check logs)
- Ensure sensor reads complete within timeout period
- Check LoRa radio is in RX mode after transmissions

### Serial output shows garbled text
- Verify baud rate: 115200
- Check USB cable connection
- Try different USB port

<p align="right">(<a href="#readme-top">back to top</a>)</p>

## Project Structure

```
MyFreeRTOSProject/
├── IoT_Node/
│   ├── IoT_Node.ino          # Main entry point
│   ├── config.h              # Configuration (ignored by git)
│   ├── app_state.h           # Global state definitions
│   ├── app_state.cpp         # Initialization & helpers
│   ├── lora_radio.h          # LoRa communication
│   ├── lora_radio.cpp        # LoRa implementation
│   ├── task_sensors.h        # Sensor reading task
│   ├── task_sensors.cpp      # Sensor implementation
│   ├── task_mpu.h            # Accelerometer task
│   ├── task_mpu.cpp          # MPU6050 implementation
│   ├── task_display.h        # Display task
│   ├── task_display.cpp      # OLED implementation
│   ├── display_oled.h        # Display utilities
│   └── display_oled.cpp      # Display functions
│
├── Gateway_Node/
│   ├── Gateway_Node.ino      # Main entry point
│   ├── config.h              # Configuration (ignored by git)
│   └── (compiled from main)
│
├── .gitignore                # Git ignore rules
├── README.md                 # This file
└── LICENSE                   # Project license
```

## FreeRTOS Task Structure

| Task | Priority | Period | Purpose |
|------|----------|--------|---------|
| SensorsTask | 2 | 60s | Read water sensors, transmit data |
| MPUTask | 3 | 50ms | Read accelerometer, detect shakes |
| DisplayTask | 1 | 100ms | Update OLED display |
| Core0 | - | - | System tasks |
| Core1 | - | - | User tasks |

## Performance Metrics

- **Sensor Read Time**: ~4.2 seconds
- **JSON Serialization**: <1ms
- **LoRa Transmission**: 1-2 seconds (SF=7)
- **ACK Wait Time**: 800ms
- **Periodic Interval**: 60 seconds
- **Payload Size**: ~135 bytes (88% efficient)

## Roadmap

- [x] Water quality sensor integration
- [x] LoRa encrypted communication
- [x] FreeRTOS task management
- [x] Battery monitoring
- [x] Shake event detection
- [x] Real-time display
- [ ] Mobile app integration
- [ ] Cloud data synchronization
- [ ] Web dashboard
- [ ] Multi-node support
- [ ] OTA firmware updates

See the [open issues](https://github.com/your-username/MyFreeRTOSProject/issues) for a full list of proposed features and known issues.

<p align="right">(<a href="#readme-top">back to top</a>)</p>

## Contributing

Contributions are what make the open source community such an amazing place. Any contributions you make are **greatly appreciated**.

1. Fork the Project
2. Create your Feature Branch (`git checkout -b feature/AmazingFeature`)
3. Commit your Changes (`git commit -m 'Add some AmazingFeature'`)
4. Push to the Branch (`git push origin feature/AmazingFeature`)
5. Open a Pull Request

<p align="right">(<a href="#readme-top">back to top</a>)</p>

## License

Distributed under the MIT License. See `LICENSE.txt` for more information.

<p align="right">(<a href="#readme-top">back to top</a>)</p>

## Contact

Project Maintainer - [@yourtwitter](https://twitter.com/yourtwitter) - your.email@example.com

Project Repository: [https://github.com/your-username/MyFreeRTOSProject](https://github.com/your-username/MyFreeRTOSProject)

<p align="right">(<a href="#readme-top">back to top</a>)</p>

## Acknowledgments

* Arduino community for excellent IoT libraries
* LoRa-Alliance for long-range communication specs
* FreeRTOS for real-time operating system
* Adafruit for sensor libraries and drivers

<p align="right">(<a href="#readme-top">back to top</a>)</p>

---

[Arduino.cc]: https://img.shields.io/badge/Arduino-00979D?style=for-the-badge&logo=Arduino&logoColor=white
[Arduino-url]: https://www.arduino.cc
[ESP32]: https://img.shields.io/badge/ESP32-E7352C?style=for-the-badge&logo=espressif&logoColor=white
[ESP32-url]: https://www.espressif.com
[FreeRTOS]: https://img.shields.io/badge/FreeRTOS-3498DB?style=for-the-badge
[FreeRTOS-url]: https://www.freertos.org
[LoRa]: https://img.shields.io/badge/LoRa-1F77B9?style=for-the-badge
[LoRa-url]: https://lora-alliance.org
[ArduinoJson]: https://img.shields.io/badge/ArduinoJson-5C6AC4?style=for-the-badge
[ArduinoJson-url]: https://arduinojson.org