import os
import hmac
import hashlib
import time
import uuid
import argparse
import json
import threading
from datetime import datetime, timezone
from functools import wraps
from flask import Flask, request, jsonify
from werkzeug.serving import WSGIRequestHandler
from dotenv import load_dotenv
from db import init_db, recover_on_startup, get_db_connection, get_current_transcript, STAGING_DIR, AUDIO_DIR

load_dotenv()
app = Flask("IngestService")

init_db()
ALLOWED_TIMESTAMP_DRIFT_SEC = 300

EXPECT_100_ENV_KEY = "meeting_companion.expect_100"

class DeferredContinueRequestHandler(WSGIRequestHandler):
    """Werkzeug writes '100 Continue' in run_wsgi() before the app runs, so a rejected
    request would still upload its whole body. Hide the Expect header from Werkzeug and
    let upload_audio() send the 100 Continue only after every header check has passed."""
    def handle_expect_100(self):
        return True

    def run_wsgi(self):
        self._expect_100 = self.headers.get("Expect", "").lower().strip(" \t") == "100-continue"
        if self._expect_100:
            del self.headers["Expect"]
        super().run_wsgi()

    def make_environ(self):
        environ = super().make_environ()
        environ[EXPECT_100_ENV_KEY] = getattr(self, "_expect_100", False)
        return environ

def send_100_continue_if_expected():
    if not request.environ.get(EXPECT_100_ENV_KEY):
        return
    sock = request.environ.get("werkzeug.socket")
    if sock:
        sock.sendall(b"HTTP/1.1 100 Continue\r\n\r\n")

def require_device_hmac(f):
    @wraps(f)
    def decorated(*args, **kwargs):
        device_id = request.headers.get("X-Device-Id")
        key_id = request.headers.get("X-Key-Id")
        req_timestamp = request.headers.get("X-Request-Timestamp")
        signature = request.headers.get("X-Signature")
        
        if not all([device_id, key_id, req_timestamp, signature]):
            return jsonify({"error_code": "unauthorized", "message": "Missing authentication headers"}), 401
            
        try:
            req_time = int(req_timestamp)
        except ValueError:
            return jsonify({"error_code": "bad_request", "message": "Invalid timestamp format"}), 400

        if abs(int(time.time()) - req_time) > ALLOWED_TIMESTAMP_DRIFT_SEC:
            return jsonify({"error_code": "clock_skew", "message": "Timestamp expired or skewed"}), 401

        conn = get_db_connection()
        row = conn.execute(
            "SELECT secret FROM device_keys WHERE device_id = ? AND key_id = ? AND revoked_at IS NULL", 
            (device_id, key_id)
        ).fetchone()
        conn.close()

        if not row:
            return jsonify({"error_code": "unknown_key", "message": "Invalid or revoked device key"}), 401

        expected_secret = row["secret"]
        content_sha256 = request.headers.get("X-Content-SHA256", "")
        content_length = request.headers.get("Content-Length", "")
        
        canonical_string = f"{device_id}\n{key_id}\n{req_timestamp}\n{content_sha256}\n{content_length}"
        computed_signature = hmac.new(
            expected_secret.encode("utf-8"),
            canonical_string.encode("utf-8"),
            hashlib.sha256
        ).hexdigest()

        if not hmac.compare_digest(computed_signature, signature.lower()):
            return jsonify({"error_code": "bad_signature", "message": "Invalid cryptographic signature"}), 401

        return f(*args, **kwargs)
    return decorated

@app.route("/health", methods=["GET"])
def health():
    return jsonify({"status": "ok"})

@app.route("/api/upload", methods=["POST"])
@require_device_hmac
def upload_audio():
    content_length = request.headers.get("Content-Length")
    if not content_length:
        return jsonify({"error_code": "length_required", "message": "Content-Length header missing"}), 411
        
    if request.headers.get("Content-Type") != "audio/wav":
        return jsonify({"error_code": "unsupported_media", "message": "Content-Type must be audio/wav"}), 415

    device_id = request.headers.get("X-Device-Id")
    recording_timestamp = request.headers.get("X-Recording-Started-At")
    
    if not recording_timestamp:
        return jsonify({"error_code": "bad_request", "message": "Missing X-Recording-Started-At header"}), 400

    recording_id = f"{device_id}_{recording_timestamp}"
    
    # Check deduplication quickly and immediately close connection
    conn = get_db_connection()
    existing_job = conn.execute("SELECT * FROM meetings WHERE recording_id = ?", (recording_id,)).fetchone()
    conn.close()
    
    if existing_job:
        state = existing_job["state"]
        job_id = existing_job["job_id"]
        if state in ["COMPLETED"]:
            conn = get_db_connection()
            ext_row = conn.execute("SELECT raw_json FROM extractions WHERE extraction_id = ?", (existing_job["current_extraction_id"],)).fetchone()
            conn.close()
            payload = json.loads(ext_row["raw_json"]) if ext_row and ext_row["raw_json"] else {}
            return jsonify({"job_id": job_id, "status": "processed", "data": payload}), 200
        elif state in ["FAILED_ASR", "FAILED_EXTRACTION"]:
            return jsonify({"job_id": job_id, "status": state, "duplicate": True, "error_code": existing_job["last_error_code"]}), 409
        else:
            return jsonify({"job_id": job_id, "status": state, "duplicate": True}), 202

    # All header checks passed: only now let the client start sending the body
    send_100_continue_if_expected()

    job_id = str(uuid.uuid4())
    staging_path = os.path.join(STAGING_DIR, f"{job_id}.part")
    hasher = hashlib.sha256()
    audio_bytes = 0

    print(f"[UPLOAD] Accepting stream for new job: {job_id}...")
    try:
        # DB connection is intentionally closed during this slow network I/O
        with open(staging_path, "wb") as f:
            while chunk := request.stream.read(8192):
                f.write(chunk)
                hasher.update(chunk)
                audio_bytes += len(chunk)
    except Exception as e:
        print(f"[UPLOAD ERROR] Failed reading stream: {e}")
        if os.path.exists(staging_path):
            os.remove(staging_path)
        return jsonify({"error_code": "internal", "message": f"Error reading stream: {str(e)}"}), 500

    computed_sha256 = hasher.hexdigest()
    client_sha256 = request.headers.get("X-Content-SHA256", "").lower()

    if client_sha256 and computed_sha256 != client_sha256:
        print(f"[UPLOAD ERROR] Hash mismatch. Expected {computed_sha256}, got {client_sha256}")
        os.remove(staging_path)
        return jsonify({"error_code": "hash_mismatch", "message": "Audio checksum validation failed"}), 400

    now = datetime.now(timezone.utc)
    final_dir = os.path.join(AUDIO_DIR, f"{now.year}", f"{now.month:02d}")
    os.makedirs(final_dir, exist_ok=True)
    final_path = os.path.join(final_dir, f"{job_id}.wav")
    os.rename(staging_path, final_path)

    uploaded_at = datetime.fromtimestamp(int(request.headers.get("X-Request-Timestamp")), tz=timezone.utc).isoformat()

    # Re-open connection to insert the finalized job
    conn = get_db_connection()
    try:
        conn.execute("""
            INSERT INTO meetings (job_id, device_id, recording_id, recording_started_at, uploaded_at, audio_relpath, audio_sha256, audio_bytes, state)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, 'QUEUED')
        """, (job_id, device_id, recording_id, recording_timestamp, uploaded_at, final_path, computed_sha256, audio_bytes))
        conn.commit()
    except Exception as e:
        print(f"[UPLOAD ERROR] Database insertion failed: {e}")
        conn.close()
        if os.path.exists(final_path):
            os.remove(final_path)
        return jsonify({"error_code": "internal", "message": f"Database insertion failed: {str(e)}"}), 500

    conn.close()
    print(f"[UPLOAD] Successfully saved and queued job: {job_id} ({audio_bytes} bytes)")
    return jsonify({"job_id": job_id, "status": "QUEUED", "duplicate": False}), 202


# ==========================================
# BACKGROUND WORKER (Apple MLX Foundation Models & Whisper)
# ==========================================
def process_background_tasks():
    """Polls database for ready tasks and runs transcription and summarization via Apple MLX."""
    from mlx_lm import load, generate
    import mlx_whisper
    
    print("[WORKER] Loading MLX LLM Model into Apple Silicon Unified Memory...")
    llm_model, llm_tokenizer = load("mlx-community/Qwen2.5-3B-Instruct-4bit")
    whisper_model_path = "mlx-community/whisper-turbo"
    print("[WORKER] MLX Models ready.")
    
    while True:
        # --- STEP 1: TRANSCRIBE QUEUED AUDIO ---
        conn = get_db_connection()
        queued_job = None
        try:
            queued_job = conn.execute("SELECT job_id, audio_relpath FROM meetings WHERE state = 'QUEUED' LIMIT 1").fetchone()
            if queued_job:
                conn.execute("UPDATE meetings SET state = 'TRANSCRIBING' WHERE job_id = ?", (queued_job["job_id"],))
                conn.commit()
        except Exception as e:
            print(f"[WORKER ERROR] Database check failed: {e}")
        finally:
            conn.close()
            
        if queued_job:
            job_id = queued_job["job_id"]
            audio_path = queued_job["audio_relpath"]
            print(f"[WORKER] Starting MLX-Whisper transcription for job {job_id}...")
            
            try:
                # MLX computation happens while DB connection is safely closed
                result = mlx_whisper.transcribe(audio_path, path_or_hf_repo=whisper_model_path, fp16=True)
                transcript_text = result["text"].strip()
                
                conn = get_db_connection()
                try:
                    cursor = conn.cursor()
                    cursor.execute("""
                        INSERT INTO transcripts (job_id, asr_runtime, asr_model, text)
                        VALUES (?, ?, ?, ?)
                    """, (job_id, "mlx-whisper", whisper_model_path, transcript_text))
                    # Older transcripts stay for audit; every read goes through current_transcript_id
                    cursor.execute("""
                        UPDATE meetings
                        SET state = 'TRANSCRIBED', current_transcript_id = ?, transcribed_at = CURRENT_TIMESTAMP
                        WHERE job_id = ?
                    """, (cursor.lastrowid, job_id))
                    conn.commit()
                    print(f"[WORKER] Transcription complete for job {job_id}.")
                finally:
                    conn.close()
            except Exception as e:
                print(f"[WORKER ERROR] Transcription failed: {e}")
                conn = get_db_connection()
                conn.execute("UPDATE meetings SET state = 'FAILED_ASR', last_error_message = ? WHERE job_id = ?", (str(e), job_id))
                conn.commit()
                conn.close()

        # --- STEP 2: SUMMARIZE TRANSCRIBED MEETINGS ---
        conn = get_db_connection()
        transcribed_job = None
        transcript_text = None
        transcript_id = None
        try:
            transcribed_job = conn.execute("SELECT job_id FROM meetings WHERE state = 'TRANSCRIBED' LIMIT 1").fetchone()
            if transcribed_job:
                job_id = transcribed_job["job_id"]
                conn.execute("UPDATE meetings SET state = 'EXTRACTING' WHERE job_id = ?", (job_id,))
                conn.commit()
                transcript_row = get_current_transcript(conn, job_id)
                if transcript_row:
                    transcript_text = transcript_row["text"]
                    transcript_id = transcript_row["transcript_id"]
                else:
                    conn.execute("UPDATE meetings SET state = 'FAILED_EXTRACTION', last_error_message = 'Missing transcript' WHERE job_id = ?", (job_id,))
                    conn.commit()
        except Exception as e:
            print(f"[WORKER ERROR] Database check failed: {e}")
        finally:
            conn.close()
            
        if transcribed_job and transcript_text:
            job_id = transcribed_job["job_id"]
            print(f"[WORKER] Starting MLX LLM summarization for job {job_id}...")
            
            try:
                prompt = f"Summarize the following meeting transcript, identify key speakers, and extract actionable items:\n\n{transcript_text}"
                
                if hasattr(llm_tokenizer, "apply_chat_template") and llm_tokenizer.chat_template:
                    messages = [{"role": "user", "content": prompt}]
                    prompt = llm_tokenizer.apply_chat_template(messages, tokenize=False, add_generation_prompt=True)
                
                # Heavy MLX generation happens while DB connection is safely closed
                summary = generate(llm_model, llm_tokenizer, prompt=prompt, max_tokens=1024, verbose=False)
                
                conn = get_db_connection()
                try:
                    cursor = conn.cursor()
                    cursor.execute("""
                        INSERT INTO extractions (job_id, transcript_id, model, summary, raw_json)
                        VALUES (?, ?, ?, ?, ?)
                    """, (job_id, transcript_id, "mlx-llm", summary, json.dumps({"summary": summary})))

                    extraction_id = cursor.lastrowid
                    cursor.execute("""
                        UPDATE meetings
                        SET state = 'COMPLETED', current_extraction_id = ?, completed_at = CURRENT_TIMESTAMP
                        WHERE job_id = ?
                    """, (extraction_id, job_id))
                    conn.commit()
                    print(f"[WORKER] Successfully summarized job {job_id} using Apple MLX.")
                finally:
                    conn.close()
            except Exception as e:
                print(f"[WORKER ERROR] LLM Extraction failed: {e}")
                conn = get_db_connection()
                conn.execute("UPDATE meetings SET state = 'FAILED_EXTRACTION', last_error_message = ? WHERE job_id = ?", (str(e), job_id))
                conn.commit()
                conn.close()

        time.sleep(3)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Ingest Service")
    parser.add_argument("--debug", action="store_true", help="Enable Flask debug mode")
    args = parser.parse_args()

    # Recovery must finish before the worker claims jobs and before any upload arrives
    recover_on_startup()

    # Start the Apple MLX summarization thread
    worker_thread = threading.Thread(target=process_background_tasks, daemon=True)
    worker_thread.start()

    port = int(os.getenv("INGEST_PORT", 8443))
    print(f"Starting Ingest Service on http://0.0.0.0:{port} (Debug: {args.debug}) ...")

    app.run(host="0.0.0.0", port=port, debug=args.debug, use_reloader=False,
            request_handler=DeferredContinueRequestHandler)
