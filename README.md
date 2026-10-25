# Meeting Companion

This project is a desk clock and a meeting recorder in one device: a Waveshare **ESP32-S3 RLCD 4.2** board. It works with a backend on a **Mac Mini** (Apple Silicon) that transcribes and summarizes the meetings **locally**. No audio or transcript leaves your network.

```
ESP32-S3 (record → SD card → signed HTTP upload)  ──Wi-Fi──▶  Mac Mini (Whisper → local LLM → SQLite → dashboard)
```

## Repository layout

| Path | What it is |
| ----- | ----- |
| [`Esp32-s3-rlcd-alex/`](Esp32-s3-rlcd-alex/readme.md) | Arduino firmware: clock, calendar, weather, recorder, Wi-Fi setup portal |
| [`backend/`](backend/README.md) | Python services: ingest (upload + processing worker) and admin (API + dashboard) |
| `example_config_*.txt` | Templates for the config files that go on the device's SD card |
| [`docs/`](docs/) | Implementation plans and lessons learned |

## Quick start

1. **Backend:** set up the Mac Mini by following [`backend/README.md`](backend/README.md). Put a device ID and a shared secret in `backend/.env`.
2. **SD card:** copy the `example_config_*.txt` files to the card's root as `config_wifi.txt`, `config_clock.txt`, and `config_server.txt`. Then edit them. In `config_server.txt`:
   * Set `device_id` and `hmac_secret` to the same values as `ESP32_UUID` and `ESP32_SECRET` in `backend/.env`.
   * Set `key_id=primary-key-01`.
   * Set `mac_server` to the Mini's LAN address, for example `http://192.168.1.100:8443`.
3. **Firmware:** flash it by following [`Esp32-s3-rlcd-alex/readme.md`](Esp32-s3-rlcd-alex/readme.md).
4. **Record:** press the left button to go to the meeting screen. Press the right button to start recording, and press it again to stop. The device then uploads the file.
5. **Review:** open the dashboard on the Mini at http://127.0.0.1:8080.

## Documentation

* [`docs/ImplementationPlan-V2.md`](docs/ImplementationPlan-V2.md): how the system works today, its known gaps, and the roadmap.
* [`docs/learned_lessons_v1.md`](docs/learned_lessons_v1.md): what we learned building v1.
* [`docs/ImplementationPlan-V1.md`](docs/ImplementationPlan-V1.md): the original target design.

## Status

This is a working prototype for a trusted home or office LAN:

* Uploads use plain HTTP, signed with HMAC.
* The admin dashboard has only placeholder auth and listens on localhost only.
* It is not ready for internet-facing use. See the roadmap in the V2 plan.
