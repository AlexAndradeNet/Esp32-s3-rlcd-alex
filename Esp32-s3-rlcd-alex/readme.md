# Meeting Companion — ESP32-S3 Firmware

This is the Arduino firmware for the **Waveshare ESP32-S3 RLCD 4.2** board. The board works as a low-power desk clock and as a meeting recorder that uploads to the Mac Mini backend.

## Screens and buttons

| Screen | Shows |
| ----- | ----- |
| 1. Clock | Time, calendar, weather (Open-Meteo), room temperature and humidity (SHTC3), battery %, IP address |
| 2. Meeting | Recording status, timer, upload result, SD card errors |
| 3. Wi-Fi setup | QR code for an open setup access point, plus a web form at `http://192.168.5.1` |

* **Left button (GPIO18):** switch screens, Clock ↔ Meeting. Screen 3 appears only when the Meeting screen could not reach Wi-Fi.
* **Right button (BOOT, GPIO0):** start or stop recording on the Meeting screen.

Bluetooth is never used and stays off. Wi-Fi stays off except during syncs and uploads. On the Clock screen the CPU runs at 80 MHz and light-sleeps between minutes.

**While recording, only the microphones are on.** Wi-Fi and NTP are off, the speaker DAC and its amplifier are off, and the temperature/humidity sensor and battery readings are paused. The recording uploads after you stop it.

## Build and flash

* Arduino IDE 2.3.x with the **esp32 core 3.x** (tested with 3.3.12), board **ESP32S3 Dev Module**.
* Libraries: `Adafruit GFX Library`, `U8g2_for_Adafruit_GFX`, and `ArduinoJson`.
* Use these tool settings:

  <img width="600" height="1050" alt="Arduino IDE tool settings" src="https://github.com/user-attachments/assets/fdee8493-62f7-4b51-9330-78610d117484" />

Command-line build (`arduino-cli` comes bundled with the IDE):

```shell
arduino-cli compile --fqbn "esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=huge_app" Esp32-s3-rlcd-alex
```

## SD card setup

The firmware reads its settings from the root of the SD card. That way you can change Wi-Fi, location, or server settings without reflashing. Start from the `conf/example_config_*.txt` files.

| File | Keys |
| ----- | ----- |
| `config_wifi.txt` | `wifi_ssid`, `wifi_pass`: the default network |
| `config_clock.txt` | `city`, `latitude`, `longitude`, `gmt` (UTC offset in hours), `override`. Set `override=true` to use these values instead of auto-location |
| `config_server.txt` | `mac_server` (e.g. `http://192.168.1.100:8443`), `device_id`, `key_id` (must be `primary-key-01`), `hmac_secret` |

The firmware creates and maintains these:

* `wifi_database.json`: known networks, each with its location and UTC offset. The device uses it to reconnect automatically.
* `meetings/`: recordings (see below).

## Recordings

The audio is 16 kHz mono G.711 µ-law, about 58 MB per hour.

Each recording is a single file, and **its name is its whole state**:

```
meetings/YYYYMMDD_HHMMSS_<recording_id>_c<0|1>_<status>.wav
```

* The timestamp, the `recording_id` (UUIDv4), and the clock flag (`c1` = time was NTP-synced) are set when the file is created.
* The status changes only by renaming the file, which is atomic. A battery cut or brownout can't leave a recording half-tracked, and there's no RAM state or manifest file to corrupt.
* Every retry reuses the ID from the name, so the server never creates a second job for the same recording.
* Older files named `YYYYMMDD_HHMMSS_<status>.wav` still upload. The server identifies them by their timestamp.

| Status | Meaning |
| ----- | ----- |
| `pending` | Recording in progress. After a power loss, it is repaired at the next boot and becomes `failed` |
| `OK` | Uploaded. Deleted 48 h after recording |
| `failed` | Not uploaded yet. Retried after the next successful upload |
| `rejected` | The server refused it permanently (for example a wrong key or a conflict). Kept, and never retried |

## Internet services used by the clock

Open-Meteo (weather and UTC offset), mylnikov.org and ip-api.com (location), BigDataCloud (city name), and NTP pools.

Meeting audio is only ever sent to your own `mac_server`.

See [`../docs/ImplementationPlan-V2.md`](../docs/ImplementationPlan-V2.md) (§18) for the full device contract.
