import sqlite3
import os
from dotenv import load_dotenv

# Load .env variables so db.py can read the ESP32 secrets
load_dotenv()

DB_PATH = "meeting_companion.db"
AUDIO_DIR = "audio"
STAGING_DIR = "staging"
ORPHANS_DIR = os.path.join(AUDIO_DIR, "orphans")

# ==========================================
# MIGRATION DEFINITIONS
# ==========================================
MIGRATIONS = [
    {
        "version": 1,
        "description": "Initial v1 schema creation",
        "sql": """
            CREATE TABLE IF NOT EXISTS devices (
                device_id TEXT PRIMARY KEY,
                name TEXT,
                default_language TEXT,
                created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
                last_seen_at TIMESTAMP,
                disabled_at TIMESTAMP
            );

            CREATE TABLE IF NOT EXISTS device_keys (
                key_id TEXT PRIMARY KEY,
                device_id TEXT,
                secret TEXT,
                not_before TIMESTAMP,
                not_after TIMESTAMP,
                revoked_at TIMESTAMP,
                FOREIGN KEY(device_id) REFERENCES devices(device_id)
            );

            CREATE TABLE IF NOT EXISTS replay_nonces (
                device_id TEXT,
                request_id TEXT,
                expires_at TIMESTAMP,
                PRIMARY KEY(device_id, request_id)
            );

            CREATE TABLE IF NOT EXISTS meetings (
                job_id TEXT PRIMARY KEY,
                device_id TEXT,
                recording_id TEXT,
                recording_started_at INTEGER,
                clock_valid INTEGER,
                uploaded_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
                transcribed_at TIMESTAMP,
                completed_at TIMESTAMP,
                title TEXT,
                audio_relpath TEXT,
                audio_sha256 TEXT,
                audio_bytes INTEGER,
                audio_duration_s REAL,
                sample_rate INTEGER,
                state TEXT,
                attempts_asr INTEGER DEFAULT 0,
                attempts_extract INTEGER DEFAULT 0,
                next_attempt_at TIMESTAMP,
                first_transient_error_at TIMESTAMP,
                locked_by TEXT,
                lease_token TEXT,
                heartbeat_at TIMESTAMP,
                progress REAL DEFAULT 0.0,
                last_error_code TEXT,
                last_error_message TEXT,
                last_error_at TIMESTAMP,
                current_transcript_id INTEGER,
                current_extraction_id INTEGER,
                UNIQUE(device_id, recording_id)
            );

            CREATE TABLE IF NOT EXISTS transcripts (
                transcript_id INTEGER PRIMARY KEY AUTOINCREMENT,
                job_id TEXT,
                asr_runtime TEXT,
                asr_model TEXT,
                language TEXT,
                params_json TEXT,
                text TEXT,
                created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
                FOREIGN KEY(job_id) REFERENCES meetings(job_id)
            );

            CREATE TABLE IF NOT EXISTS transcript_segments (
                transcript_id INTEGER,
                segment_id INTEGER,
                start_ms INTEGER,
                end_ms INTEGER,
                text TEXT,
                PRIMARY KEY(transcript_id, segment_id),
                FOREIGN KEY(transcript_id) REFERENCES transcripts(transcript_id) ON DELETE CASCADE
            );

            CREATE TABLE IF NOT EXISTS extractions (
                extraction_id INTEGER PRIMARY KEY AUTOINCREMENT,
                job_id TEXT,
                transcript_id INTEGER,
                model TEXT,
                prompt_version TEXT,
                schema_version INTEGER,
                raw_json TEXT,
                summary TEXT,
                input_tokens INTEGER,
                output_tokens INTEGER,
                created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
                FOREIGN KEY(job_id) REFERENCES meetings(job_id)
            );

            CREATE TABLE IF NOT EXISTS speakers (
                speaker_id INTEGER PRIMARY KEY AUTOINCREMENT,
                extraction_id INTEGER,
                label TEXT,
                name_guess TEXT,
                participant_id INTEGER,
                verified_at TIMESTAMP,
                FOREIGN KEY(extraction_id) REFERENCES extractions(extraction_id) ON DELETE CASCADE
            );

            CREATE TABLE IF NOT EXISTS action_items (
                item_id INTEGER PRIMARY KEY AUTOINCREMENT,
                extraction_id INTEGER,
                text TEXT,
                owner_speaker_id INTEGER,
                due_date TEXT,
                status TEXT DEFAULT 'open',
                status_changed_at TIMESTAMP,
                evidence_json TEXT,
                FOREIGN KEY(extraction_id) REFERENCES extractions(extraction_id) ON DELETE CASCADE
            );

            CREATE TABLE IF NOT EXISTS commitments (
                item_id INTEGER PRIMARY KEY AUTOINCREMENT,
                extraction_id INTEGER,
                text TEXT,
                owner_speaker_id INTEGER,
                due_date TEXT,
                status TEXT DEFAULT 'open',
                status_changed_at TIMESTAMP,
                evidence_json TEXT,
                FOREIGN KEY(extraction_id) REFERENCES extractions(extraction_id) ON DELETE CASCADE
            );
            
            CREATE TABLE IF NOT EXISTS decisions (
                item_id INTEGER PRIMARY KEY AUTOINCREMENT,
                extraction_id INTEGER,
                text TEXT,
                evidence_json TEXT,
                FOREIGN KEY(extraction_id) REFERENCES extractions(extraction_id) ON DELETE CASCADE
            );
            
            CREATE TABLE IF NOT EXISTS risks (
                item_id INTEGER PRIMARY KEY AUTOINCREMENT,
                extraction_id INTEGER,
                text TEXT,
                evidence_json TEXT,
                FOREIGN KEY(extraction_id) REFERENCES extractions(extraction_id) ON DELETE CASCADE
            );
            
            CREATE TABLE IF NOT EXISTS follow_ups (
                item_id INTEGER PRIMARY KEY AUTOINCREMENT,
                extraction_id INTEGER,
                text TEXT,
                evidence_json TEXT,
                FOREIGN KEY(extraction_id) REFERENCES extractions(extraction_id) ON DELETE CASCADE
            );

            CREATE TABLE IF NOT EXISTS participants (
                participant_id INTEGER PRIMARY KEY AUTOINCREMENT,
                name TEXT,
                email TEXT UNIQUE,
                aliases_json TEXT,
                created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
                updated_at TIMESTAMP,
                archived_at TIMESTAMP
            );

            CREATE TABLE IF NOT EXISTS meeting_participants (
                job_id TEXT,
                participant_id INTEGER,
                source TEXT,
                PRIMARY KEY(job_id, participant_id),
                FOREIGN KEY(job_id) REFERENCES meetings(job_id) ON DELETE CASCADE,
                FOREIGN KEY(participant_id) REFERENCES participants(participant_id)
            );

            CREATE TABLE IF NOT EXISTS admins (
                admin_id INTEGER PRIMARY KEY AUTOINCREMENT,
                username TEXT UNIQUE,
                password_hash TEXT,
                created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
            );

            CREATE TABLE IF NOT EXISTS api_tokens (
                token_id INTEGER PRIMARY KEY AUTOINCREMENT,
                token_hash TEXT UNIQUE,
                created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
            );
            
            -- FTS5 Full-Text Search Virtual Table
            CREATE VIRTUAL TABLE IF NOT EXISTS search_fts USING fts5(
                job_id UNINDEXED, 
                kind UNINDEXED, 
                ref_id UNINDEXED, 
                text,
                tokenize="unicode61 remove_diacritics 2"
            );
        """
    }
]

def init_directories():
    """Ensure required storage folders exist."""
    for folder in [STAGING_DIR, AUDIO_DIR, ORPHANS_DIR]:
        os.makedirs(folder, exist_ok=True)

def get_db_connection():
    """Returns a SQLite connection with required WAL pragmas."""
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL;")
    conn.execute("PRAGMA synchronous=FULL;")
    conn.execute("PRAGMA fullfsync=ON;")
    conn.execute("PRAGMA busy_timeout=5000;")
    conn.execute("PRAGMA foreign_keys=ON;")
    return conn

def register_env_device(cursor):
    """Reads .env variables and registers the ESP32 device if not already present."""
    device_id = os.getenv("ESP32_UUID")
    secret = os.getenv("ESP32_SECRET")
    key_id = "primary-key-01"

    if device_id and secret:
        # Register the hardware device (ignores if already exists)
        cursor.execute("""
            INSERT OR IGNORE INTO devices (device_id, name, default_language)
            VALUES (?, ?, ?)
        """, (device_id, "ESP32 Meeting Companion", "en"))

        # Assign the cryptographic key (ignores if already exists)
        cursor.execute("""
            INSERT OR IGNORE INTO device_keys (key_id, device_id, secret, not_before, not_after)
            VALUES (?, ?, ?, CURRENT_TIMESTAMP, datetime('now', '+10 years'))
        """, (key_id, device_id, secret))
        
        print(f"[DB] Verified device registration for {device_id}")

def init_db():
    """Runs pending database migrations and registers default devices."""
    init_directories()
    conn = get_db_connection()
    cursor = conn.cursor()
    
    # 1. Create the migrations tracking table if it doesn't exist
    cursor.execute("""
        CREATE TABLE IF NOT EXISTS schema_migrations (
            version INTEGER PRIMARY KEY,
            applied_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        );
    """)
    conn.commit()
    
    # 2. Check which versions are already applied
    cursor.execute("SELECT version FROM schema_migrations")
    applied_versions = {row["version"] for row in cursor.fetchall()}
    
    # 3. Apply missing migrations sequentially
    for migration in sorted(MIGRATIONS, key=lambda x: x["version"]):
        version = migration["version"]
        if version not in applied_versions:
            print(f"[DB] Applying migration v{version}: {migration.get('description', '')}")
            try:
                cursor.executescript(migration["sql"])
                cursor.execute("INSERT INTO schema_migrations (version) VALUES (?)", (version,))
                conn.commit()
            except Exception as e:
                print(f"[DB] Migration v{version} failed: {e}")
                conn.rollback()
                raise
    
    # 4. Automatically seed the environment device
    register_env_device(cursor)
    conn.commit()
                
    conn.close()