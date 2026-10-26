"""POST /api/upload: authentication, identity, duplicates and the never-delete rule (V2 §5–§7)."""
import hashlib
import os
import sqlite3
import time
import unittest
from unittest import mock

import support
import db
import ingest_service


class IngestTestCase(unittest.TestCase):
    def setUp(self):
        support.reset_state()
        self.client = ingest_service.app.test_client()

    def upload(self, body, **kwargs):
        return self.client.post("/api/upload", data=body, headers=support.signed_headers(body, **kwargs))


class AuthenticationTests(IngestTestCase):
    def test_missing_headers_are_rejected(self):
        response = self.client.post("/api/upload", data=b"x", headers={"Content-Type": "audio/wav"})
        self.assertEqual(response.status_code, 401)
        self.assertEqual(response.get_json()["error_code"], "unauthorized")

    def test_wrong_secret_is_a_bad_signature(self):
        response = self.upload(support.mulaw_wav(), secret="not-the-secret")
        self.assertEqual(response.status_code, 401)
        self.assertEqual(response.get_json()["error_code"], "bad_signature")

    def test_unknown_key_id(self):
        response = self.upload(support.mulaw_wav(), key_id="secret_key_id")
        self.assertEqual(response.status_code, 401)
        self.assertEqual(response.get_json()["error_code"], "unknown_key")

    def test_timestamp_outside_the_drift_window(self):
        stale = int(time.time()) - ingest_service.ALLOWED_TIMESTAMP_DRIFT_SEC - 60
        response = self.upload(support.mulaw_wav(), timestamp=stale)
        self.assertEqual(response.status_code, 401)
        self.assertEqual(response.get_json()["error_code"], "clock_skew")

    def test_rejected_requests_store_nothing(self):
        self.upload(support.mulaw_wav(), secret="not-the-secret")
        self.assertEqual(support.files_under(db.AUDIO_DIR), [])
        self.assertEqual(support.files_under(db.STAGING_DIR), [])


class NewUploadTests(IngestTestCase):
    def test_new_upload_is_queued_and_stored(self):
        body = support.mulaw_wav()
        recording_id = support.new_recording_id()
        response = self.upload(body, recording_id=recording_id)

        self.assertEqual(response.status_code, 202)
        payload = response.get_json()
        self.assertEqual(payload["status"], "QUEUED")
        self.assertFalse(payload["duplicate"])

        row = support.job_row(payload["job_id"])
        self.assertEqual(row["recording_id"], recording_id)
        self.assertEqual(row["clock_valid"], 1)
        self.assertEqual(row["audio_sha256"], hashlib.sha256(body).hexdigest())
        with open(row["audio_relpath"], "rb") as f:
            self.assertEqual(f.read(), body)
        self.assertEqual(support.files_under(db.STAGING_DIR), [])

    def test_clock_valid_zero_is_stored(self):
        response = self.upload(support.mulaw_wav(), recording_id=support.new_recording_id(), clock_valid="0")
        self.assertEqual(support.job_row(response.get_json()["job_id"])["clock_valid"], 0)

    def test_recording_id_must_be_a_uuid(self):
        response = self.upload(support.mulaw_wav(), recording_id="not-a-uuid")
        self.assertEqual(response.status_code, 400)
        self.assertEqual(response.get_json()["error_code"], "invalid_headers")

    def test_legacy_upload_without_recording_id(self):
        response = self.upload(support.mulaw_wav(), started_at="1761400123")
        self.assertEqual(response.status_code, 202)
        row = support.job_row(response.get_json()["job_id"])
        self.assertEqual(row["recording_id"], f"{support.DEVICE_ID}_1761400123")

    def test_wrong_content_type(self):
        body = support.mulaw_wav()
        headers = support.signed_headers(body, recording_id=support.new_recording_id())
        headers["Content-Type"] = "application/octet-stream"
        response = self.client.post("/api/upload", data=body, headers=headers)
        self.assertEqual(response.status_code, 415)

    def test_digest_mismatch_keeps_nothing(self):
        body = support.mulaw_wav()
        wrong = hashlib.sha256(b"something else").hexdigest()
        response = self.upload(body, recording_id=support.new_recording_id(), sha256=wrong)
        self.assertEqual(response.status_code, 400)
        self.assertEqual(response.get_json()["error_code"], "hash_mismatch")
        self.assertEqual(support.files_under(db.AUDIO_DIR), [])
        self.assertEqual(support.files_under(db.STAGING_DIR), [])


class DuplicateTests(IngestTestCase):
    def upload_twice(self, state):
        body = support.mulaw_wav()
        recording_id = support.new_recording_id()
        job_id = self.upload(body, recording_id=recording_id).get_json()["job_id"]
        conn = db.get_db_connection()
        conn.execute("UPDATE meetings SET state = ?, last_error_code = 'asr_failed' WHERE job_id = ?", (state, job_id))
        conn.commit()
        conn.close()
        return job_id, self.upload(body, recording_id=recording_id)

    def test_duplicate_of_an_in_progress_job(self):
        job_id, response = self.upload_twice("QUEUED")
        self.assertEqual(response.status_code, 202)
        self.assertEqual(response.get_json(), {"job_id": job_id, "status": "QUEUED", "duplicate": True})

    def test_duplicate_of_a_completed_job_carries_no_meeting_content(self):
        job_id, response = self.upload_twice("COMPLETED")
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.get_json(), {"job_id": job_id, "status": "COMPLETED", "duplicate": True})

    def test_duplicate_of_a_failed_job(self):
        job_id, response = self.upload_twice("FAILED_ASR")
        self.assertEqual(response.status_code, 409)
        payload = response.get_json()
        self.assertEqual(payload["error_code"], "duplicate")
        self.assertEqual(payload["job_id"], job_id)

    def test_same_recording_id_with_a_different_file_is_a_conflict(self):
        recording_id = support.new_recording_id()
        first = self.upload(support.mulaw_wav(1600), recording_id=recording_id)
        second = self.upload(support.mulaw_wav(3200), recording_id=recording_id)
        self.assertEqual(second.status_code, 409)
        self.assertEqual(second.get_json()["error_code"], "recording_conflict")
        self.assertEqual(second.get_json()["job_id"], first.get_json()["job_id"])

    def test_duplicates_create_no_new_job(self):
        self.upload_twice("QUEUED")
        conn = db.get_db_connection()
        count = conn.execute("SELECT COUNT(*) FROM meetings").fetchone()[0]
        conn.close()
        self.assertEqual(count, 1)


class FailingInsertConnection:
    """Wraps a real connection; only the INSERT of a new meeting fails."""
    def __init__(self, conn):
        self._conn = conn

    def execute(self, sql, *args):
        if "INSERT INTO meetings" in sql:
            raise sqlite3.OperationalError("database is locked")
        return self._conn.execute(sql, *args)

    def __getattr__(self, name):
        return getattr(self._conn, name)


class NeverDeleteTests(IngestTestCase):
    def test_verified_audio_goes_to_orphans_when_the_insert_fails(self):
        body = support.mulaw_wav()
        with mock.patch.object(ingest_service, "get_db_connection",
                               lambda: FailingInsertConnection(db.get_db_connection())):
            response = self.upload(body, recording_id=support.new_recording_id())

        self.assertEqual(response.status_code, 500)
        orphans = support.files_under(db.ORPHANS_DIR)
        self.assertEqual(len(orphans), 1)
        with open(os.path.join(db.ORPHANS_DIR, orphans[0]), "rb") as f:
            self.assertEqual(f.read(), body)


if __name__ == "__main__":
    unittest.main()
