"""Shared sandbox for the backend tests.

The services use paths relative to the working directory and run init_db() at import,
so every test runs inside one temporary directory with its own database, staging/ and
audio/. The real backend data is never touched. Import this module before db,
ingest_service or admin_service.
"""
import hashlib
import hmac
import os
import shutil
import struct
import sys
import tempfile
import time
import uuid

BACKEND_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEVICE_ID = "test-device-0001"
KEY_ID = "primary-key-01"
SECRET = "test-secret-not-a-real-key"

# The env values win over backend/.env: load_dotenv() never overrides existing variables
os.environ["ESP32_UUID"] = DEVICE_ID
os.environ["ESP32_SECRET"] = SECRET

SANDBOX = tempfile.mkdtemp(prefix="meeting-companion-tests-")
os.chdir(SANDBOX)
if BACKEND_DIR not in sys.path:
    sys.path.insert(0, BACKEND_DIR)

import db  # noqa: E402  (must follow the chdir)

db.init_db()


def reset_state():
    """Empties the job tables and the storage folders between tests."""
    conn = db.get_db_connection()
    for table in ("extractions", "transcripts", "meetings"):
        conn.execute(f"DELETE FROM {table}")
    conn.commit()
    conn.close()
    for folder in (db.STAGING_DIR, db.AUDIO_DIR):
        shutil.rmtree(folder, ignore_errors=True)
    db.init_directories()


def mulaw_wav(n_samples=1600, sample_rate=16000):
    """A G.711 µ-law WAV like the device writes: format tag 7, mono, 8 bits."""
    data = bytes(i % 256 for i in range(n_samples))
    fmt = struct.pack("<HHIIHH", 7, 1, sample_rate, sample_rate, 1, 8)
    return (b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE" +
            b"fmt " + struct.pack("<I", 16) + fmt +
            b"data" + struct.pack("<I", len(data)) + data)


def pcm16_wav(n_samples=800, sample_rate=16000):
    data = struct.pack(f"<{n_samples}h", *((i * 37) % 30000 - 15000 for i in range(n_samples)))
    fmt = struct.pack("<HHIIHH", 1, 1, sample_rate, sample_rate * 2, 2, 16)
    return (b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE" +
            b"fmt " + struct.pack("<I", 16) + fmt +
            b"data" + struct.pack("<I", len(data)) + data)


def signed_headers(body, recording_id=None, timestamp=None, secret=SECRET, key_id=KEY_ID,
                   device_id=DEVICE_ID, sha256=None, clock_valid="1", started_at="1761400000"):
    """Headers exactly as the firmware sends them, signed over the 5-field canonical string."""
    ts = str(int(time.time()) if timestamp is None else timestamp)
    digest = hashlib.sha256(body).hexdigest() if sha256 is None else sha256
    length = str(len(body))
    canonical = f"{device_id}\n{key_id}\n{ts}\n{digest}\n{length}"
    headers = {
        "X-Device-Id": device_id,
        "X-Key-Id": key_id,
        "X-Request-Timestamp": ts,
        "X-Signature": hmac.new(secret.encode(), canonical.encode(), hashlib.sha256).hexdigest(),
        "X-Content-SHA256": digest,
        "X-Recording-Started-At": started_at,
        "X-Clock-Valid": clock_valid,
        "Content-Type": "audio/wav",
        "Content-Length": length,
    }
    if recording_id is not None:
        headers["X-Recording-Id"] = recording_id
    return headers


def new_recording_id():
    return str(uuid.uuid4())


def insert_job(state, audio_bytes=None, recording_id=None, transcript=None, summary=None):
    """Creates a meetings row directly, with its audio file when audio_bytes is given."""
    job_id = str(uuid.uuid4())
    path = None
    if audio_bytes is not None:
        folder = os.path.join(db.AUDIO_DIR, "2026", "10")
        os.makedirs(folder, exist_ok=True)
        path = os.path.join(folder, f"{job_id}.wav")
        with open(path, "wb") as f:
            f.write(audio_bytes)
    conn = db.get_db_connection()
    conn.execute("""
        INSERT INTO meetings (job_id, device_id, recording_id, recording_started_at, clock_valid,
                              audio_relpath, audio_sha256, audio_bytes, state)
        VALUES (?, ?, ?, 1761400000, 1, ?, ?, ?, ?)
    """, (job_id, DEVICE_ID, recording_id or new_recording_id(), path,
          hashlib.sha256(audio_bytes).hexdigest() if audio_bytes is not None else None,
          len(audio_bytes) if audio_bytes is not None else None, state))
    if transcript is not None:
        cur = conn.execute("INSERT INTO transcripts (job_id, text) VALUES (?, ?)", (job_id, transcript))
        conn.execute("UPDATE meetings SET current_transcript_id = ? WHERE job_id = ?", (cur.lastrowid, job_id))
    if summary is not None:
        cur = conn.execute("INSERT INTO extractions (job_id, summary) VALUES (?, ?)", (job_id, summary))
        conn.execute("UPDATE meetings SET current_extraction_id = ? WHERE job_id = ?", (cur.lastrowid, job_id))
    conn.commit()
    conn.close()
    return job_id, path


def job_row(job_id):
    conn = db.get_db_connection()
    row = conn.execute("SELECT * FROM meetings WHERE job_id = ?", (job_id,)).fetchone()
    conn.close()
    return row


def files_under(folder):
    return sorted(os.path.relpath(os.path.join(root, name), folder)
                  for root, _, names in os.walk(folder) for name in names)
