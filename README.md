# IoT Node and Gateway Node Audit

This README documents the current firmware found in `IoT_Node/` and `Gateway_Node/` only.

The implementation is no longer just a simple "sensor node sends data to gateway" sketch pair. It now includes:

- Gateway BLE provisioning for WiFi credentials and a provisioning token
- Node pairing over WiFi SoftAP + HTTP challenge/proof exchange
- Backend-assisted pairing and runtime AES key delivery
- Encrypted LoRa command/response traffic after pairing
- Gateway-side MQTT control, cloud telemetry upload, SD-based audio alerts, and command scheduling

## Current System Flow

1. `Gateway_Node` boots in `PROVISIONING`, `NODE_PAIRING`, or `NORMAL` mode.
2. If WiFi credentials are missing, the gateway starts BLE provisioning and stores credentials in NVS.
3. If a node is not yet paired, the gateway scans for nearby node SoftAPs named `IOT-<NODE_ID>`.
4. The gateway publishes the detected candidate over MQTT and waits for backend confirmation.
5. After confirmation, the gateway joins the node AP, fetches `/identity`, requests `/prove`, verifies the proof through the backend, then sends `/provision`.
6. The node reboots into STA mode, joins home WiFi, calls the backend pairing API, stores the runtime AES key, and reboots into normal mode.
7. In normal mode, the gateway drives the LoRa session with `ACTIVATE`, `HEARTBEAT_REQ`, and `MEASURE_REQ`.
8. The node replies with encrypted frames, and the gateway logs, uploads, and alerts on received telemetry.

## Folder Audit

### `IoT_Node/`

Measurement-node firmware for the ESP32-S3. It has two boot paths:

- `PAIRING`: starts a temporary WiFi AP and HTTP endpoints used by the gateway pairing flow
- `NORMAL`: starts FreeRTOS tasks, initializes sensors and LoRa, and waits for gateway activation

#### File Map

| File | Purpose |
|------|---------|
| `IoT_Node.ino` | Main boot entry. Chooses pairing mode vs normal runtime using `PairingStore`. |
| `app_state.h/.cpp` | Shared runtime state, sensor models, mutexes, sensor helpers, CRC helpers, AES-GCM helpers, and app initialization. |
| `pairing_mode.h/.cpp` | Node-side pairing server. Hosts `/identity`, `/prove`, and `/provision`, manages SoftAP lifecycle, joins home WiFi, and finalizes pairing with the backend. |
| `pairing_store.h/.cpp` | NVS storage for paired gateway/node metadata and pending provision data across reboot. |
| `lora_radio.h/.cpp` | Secure LoRa transport with CAD-based collision avoidance, ACK handling, command polling, and DATA/ACK/ACTIVATE/HEARTBEAT support. |
| `task_sensors.h/.cpp` | FreeRTOS task that wakes on `MEASURE_REQ`, reads sensors, builds compact JSON, and sends `MEASURE_RESP`. |
| `task_control.h/.cpp` | FreeRTOS task that polls LoRa command frames and dispatches pending shake alerts. |
| `task_mpu.h/.cpp` | 20 Hz shake-monitoring task. |
| `task_display.h/.cpp` | OLED/RGB refresh task. |
| `display_oled.h/.cpp` | OLED rendering helpers for sensor bars, temperatures, and event state. |
| `config.h.template` | Template for board IDs, LoRa radio parameters, sensor pins, and timing values. Copy to `config.h`. |

#### Node Behavior Notes

- The node does not auto-sample continuously in normal mode. Sensor reads are triggered by gateway `MEASURE_REQ`.
- Shake events are asynchronous and can generate immediate `SHAKE` data frames.
- Runtime encryption is loaded from the saved pairing data, not from a long-term fixed session after pairing completes.
- Pairing depends on `NODE_DEVICE_SECRET` and will not proceed if it is unset.

### `Gateway_Node/`

Gateway firmware for the ESP32. It has three boot paths:

- `PROVISIONING`: BLE-based WiFi and token setup
- `NODE_PAIRING`: scan and pair with one nearby node
- `NORMAL`: run LoRa command scheduling, telemetry upload, MQTT integration, SD/audio alerting

#### File Map

| File | Purpose |
|------|---------|
| `Gateway_Node.ino` | Main boot entry. Chooses provisioning, pairing, or normal mode and coordinates MQTT startup. |
| `normal_mode.h/.cpp` | Initializes WiFi, backend provisioning, LoRa, SD, audio, and the OTAA-style command scheduler. |
| `provisioning_mode.h/.cpp` | BLE provisioning mode that receives WiFi credentials and a provisioning token, validates WiFi, and persists data. |
| `ble_provisioning.h/.cpp` | BLE GATT service for receiving provisioning JSON over a custom RX/TX characteristic pair. |
| `wifi_store.h/.cpp` | NVS persistence for provisioned WiFi credentials and gateway name/token. |
| `node_pairing_mode.h/.cpp` | Node discovery and pairing state machine. Scans node APs, performs proof exchange, sends provisioning, polls for pairing keys, and stores final pairing. |
| `node_pairing_store.h/.cpp` | NVS persistence for paired node metadata and AES key. |
| `mqtt_gateway.h/.cpp` | MQTT connection management, topic subscription, pairing command handling, and event publishing. |
| `api_client.h/.cpp` | HTTPS calls to backend endpoints for gateway provisioning, proof verification, rollback, pending key fetch, and command acknowledgement. |
| `wifi_manager.h/.cpp` | WiFi connection management plus HTTPS telemetry submission helpers. |
| `lora_radio.h/.cpp` | Secure LoRa command sender and frame parser for `ACTIVATE`, `HEARTBEAT_REQ`, `MEASURE_REQ`, ACKs, and DATA frames. |
| `otaa_manager.h/.cpp` | Command scheduler controlling activation retries, periodic heartbeat, periodic measure requests, and timeout recovery. |
| `telemetry.h/.cpp` | Parses incoming measurement/shake payloads, queues cloud uploads, prints serial summaries, and schedules audio alerts. |
| `audio_alert.h/.cpp` | WAV playback queue and alarm control. |
| `sd_logger.h/.cpp` | SD card mount/recovery helpers and OTA file access helper. |
| `app_state.h/.cpp` | Shared protocol constants, CRC/AES helpers, paired-node device ID lookup, and runtime key loading. |
| `config.h.template` | Template for radio pins and thresholds. It appears older than the current code path and should be reviewed before reuse. |

#### Gateway Behavior Notes

- MQTT is used during pairing and normal runtime.
- The gateway pauses MQTT temporarily when exclusive TLS heap is needed for telemetry uploads.
- `telemetry.cpp` buffers unsent records and retries uploads instead of dropping them immediately.
- Audio playback is deferred if cloud uploads are still queued or heap is too low.

## Secure LoRa Protocol

Both folders implement the same frame types in `app_state.h`:

| Type | Direction | Meaning |
|------|-----------|---------|
| `0x01` | Node -> Gateway | `DATA` (`MEASURE_RESP` or `SHAKE`) |
| `0x02` | Both | transport `ACK` |
| `0x03` | Gateway -> Node | `MEASURE_REQ` |
| `0x04` | Gateway -> Node | `HEARTBEAT_REQ` |
| `0x05` | Node -> Gateway | `HEARTBEAT_ACK` |
| `0x06` | Gateway -> Node | `ACTIVATE` |
| `0x07` | Node -> Gateway | `ACTIVATE_OK` |

Protocol characteristics:

- AES-128-GCM authenticated encryption
- CRC-16 on the frame
- LoRa CAD-based collision avoidance
- sequence-based duplicate/replay protection

## Data Payloads

The node builds compact JSON in `IoT_Node/task_sensors.cpp`.

### Measurement Payload

```json
{"b":85,"v":3.9,"m":150,"p":6.8,"ps":8,"t":280,"ts":7,"u":2.1,"us":6,"tw":22.5,"tm":45.2,"te":38.1,"e":"None"}
```

### Shake Payload

```json
{"e":"SHAKE","ag":2.45,"dg":1.45}
```

### Field Reference

| Field | Meaning |
|------|---------|
| `b` | battery percent |
| `v` | battery/load voltage |
| `m` | battery current in mA |
| `p` | pH value |
| `ps` | pH score |
| `t` | TDS value |
| `ts` | TDS score |
| `u` | turbidity sensor voltage |
| `us` | turbidity score |
| `tw` | water temperature |
| `tm` | MPU temperature |
| `te` | ESP32 temperature |
| `e` | event string |
| `ag` | acceleration magnitude for shake |
| `dg` | dynamic acceleration for shake |

## Configuration Notes

- Real secrets are intentionally excluded by `.gitignore` through `**/config.h`.
- Start from `IoT_Node/config.h.template` and `Gateway_Node/config.h.template`.
- Pairing and runtime operation currently depend on values that are not fully represented by the gateway template anymore.

## Audit Findings

- The previous README described an older architecture and missed the current BLE provisioning, node pairing, backend, and MQTT flows.
- `Gateway_Node/config.h.template` looks stale relative to the code. The gateway source currently expects symbols such as `API_BASE_URL`, `GATEWAY_FIRMWARE_VERSION`, `GATEWAY_DEVICE_SECRET`, `MQTT_BROKER_HOST`, `MQTT_COMMAND_TOPIC_PREFIX`, and related MQTT settings.
- The README in this file is now aligned to the code that is present in `IoT_Node/` and `Gateway_Node/`, not to the older template comments.
