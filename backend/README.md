# Meeting Companion — Backend

These Python services run on a Mac Mini (Apple Silicon). They accept recordings from the ESP32 device, transcribe them with **MLX Whisper**, and summarize them with a local **MLX LLM** (Qwen2.5-3B-Instruct, 4-bit). Everything is stored in SQLite.

| File | Role |
| ----- | ----- |
| `ingest_service.py` | `POST /api/upload` (HMAC-signed) and `GET /health`, on `0.0.0.0:8443` (plain HTTP). Also runs startup recovery and the background worker (ASR, then summary) |
| `admin_service.py` | REST API and operator dashboard, on `127.0.0.1:8080` |
| `db.py` | SQLite schema and migrations, device seeding from `.env`, startup recovery |

## Prerequisites

```shell
brew install python@3.12 sqlite ffmpeg   # ffmpeg decodes the device's µ-law WAV files
```

You do **not** need a Hugging Face token. The models (`mlx-community/whisper-turbo` and `mlx-community/Qwen2.5-3B-Instruct-4bit`) are public. They download automatically the first time the worker starts, which takes a few GB.

## Install

```shell
cd backend
python3.12 -m venv venv
source venv/bin/activate
pip install --upgrade pip
pip install -r requirements.txt
```

## Configure

Copy `example.env` to `.env` and edit it:

```text
ESP32_UUID=esp32-rlcd-0001-a1b2c3d4   # must match device_id in the SD card's config_server.txt
ESP32_SECRET=change-me                # must match hmac_secret in config_server.txt
INGEST_PORT=8443
ADMIN_PORT=8080
```

The device is registered with key ID `primary-key-01`, so `config_server.txt` must use `key_id=primary-key-01`.

The device is registered only on the first run. If you change `ESP32_SECRET` later, the database keeps the old secret. To change it, update the `device_keys` row, or delete `meeting_companion.db` and start fresh.

## Run

Run both services **from the `backend/` folder**. The database, `audio/`, and `staging/` paths are relative to it.

```shell
source venv/bin/activate
python3.12 ingest_service.py      # terminal 1
python3.12 admin_service.py       # terminal 2
```

Add `--debug` to either one for Flask debug mode.

* Device uploads: `http://<mac-mini-lan-ip>:8443/api/upload`
* Dashboard: http://127.0.0.1:8080 (localhost only; use an SSH tunnel for remote access)

## Tests

```shell
source venv/bin/activate
python3.12 -m unittest discover -s tests -v
```

The tests need nothing beyond `requirements.txt`, and they don't touch your data:
* **Sandbox:** each run uses a temporary folder with its own database, `audio/` and `staging/`, and a test device and secret.
* **No models:** MLX Whisper and the LLM are replaced by stubs, so nothing is downloaded and a run takes a couple of seconds.

| File | Covers |
| ----- | ----- |
| `test_ingest.py` | HMAC checks, clock skew, new uploads, recording IDs, duplicates and conflicts, digest mismatch, audio parked in `orphans/` when the DB insert fails |
| `test_expect_continue.py` | A real Werkzeug server: `100 Continue` only after the header checks pass, and a body cut short is discarded |
| `test_recovery.py` | `recover_on_startup()` after a simulated crash |
| `test_worker.py` | One worker pass, re-transcription using the current transcript, ASR failure |
| `test_admin.py` | Audio playback (µ-law decoded to PCM, file untouched, 404/410), dashboard template |

## Data on disk

| Path | Content |
| ----- | ----- |
| `meeting_companion.db` | Jobs, transcripts, summaries (SQLite, WAL mode) |
| `audio/YYYY/MM/<job_id>.wav` | Uploaded recordings |
| `audio/orphans/` | Verified audio with no database row: found at startup, or left by a failed DB insert |
| `staging/` | Uploads in progress (cleared at startup) |

**The services never delete recordings.** Everything under `audio/` is kept until you remove it yourself, so watch the disk (about 58 MB per audio hour) and back it up. Only partial uploads in `staging/` are cleaned up.

## Listening to recordings

Each job in the dashboard has a **Listen** button. Completed jobs also show the player in **View Report**. Both include a **Download .wav** link. µ-law recordings from the device are decoded to 16-bit PCM while they're served, so they play in any browser, and the stored file is not modified.

## Job lifecycle

`QUEUED → TRANSCRIBING → TRANSCRIBED → EXTRACTING → COMPLETED`, or `FAILED_ASR` / `FAILED_EXTRACTION`.

* A failed or completed job can be retried from the dashboard, or with `POST /api/jobs/<id>/retry` and the body `{"stage": "transcribe"|"extract"}`.
* After a crash, restarting the ingest service puts interrupted jobs back in the queue.

See [`../docs/ImplementationPlan-V2.md`](../docs/ImplementationPlan-V2.md) for the full API, the upload protocol, and the known gaps.
