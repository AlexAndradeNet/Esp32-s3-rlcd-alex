"""The background worker with stubbed MLX models (V2 §8–§10, fix 2 of v2.1).

process_background_tasks() loops forever. The tests replace mlx_whisper and mlx_lm with
stubs and stop the loop at its sleep, so each run is exactly one pass and no model is
downloaded.
"""
import sys
import types
import unittest
from unittest import mock

import support
import admin_service
import db
import ingest_service


class StopWorker(BaseException):
    """BaseException, so the worker's `except Exception` blocks don't swallow it."""


class FakeTime:
    def __init__(self, passes):
        self.passes = passes

    def sleep(self, _seconds):
        self.passes -= 1
        if self.passes <= 0:
            raise StopWorker()

    def time(self):
        import time
        return time.time()


class WorkerTests(unittest.TestCase):
    def setUp(self):
        support.reset_state()
        self.transcripts = []
        self.prompts = []
        self.fail_transcription = False

        def transcribe(path, **kwargs):
            if self.fail_transcription:
                raise RuntimeError("ffmpeg could not decode the file")
            return {"text": f"  {self.transcripts.pop(0)}  "}

        def generate(model, tokenizer, prompt, **kwargs):
            self.prompts.append(prompt)
            return f"summary of: {prompt.rsplit(chr(10), 1)[-1]}"

        whisper = types.ModuleType("mlx_whisper")
        whisper.transcribe = transcribe
        lm = types.ModuleType("mlx_lm")
        lm.load = lambda name: (object(), types.SimpleNamespace(chat_template=None))
        lm.generate = generate
        patcher = mock.patch.dict(sys.modules, {"mlx_whisper": whisper, "mlx_lm": lm})
        patcher.start()
        self.addCleanup(patcher.stop)

    def run_worker(self, passes=1):
        with mock.patch.object(ingest_service, "time", FakeTime(passes)):
            with self.assertRaises(StopWorker):
                ingest_service.process_background_tasks()

    def test_one_pass_transcribes_and_summarizes(self):
        job_id, _ = support.insert_job("QUEUED", support.mulaw_wav())
        self.transcripts = ["we ship on friday"]
        self.run_worker()

        row = support.job_row(job_id)
        self.assertEqual(row["state"], "COMPLETED")
        self.assertIsNotNone(row["transcribed_at"])
        self.assertIsNotNone(row["completed_at"])
        conn = db.get_db_connection()
        transcript = db.get_current_transcript(conn, job_id)
        extraction = conn.execute("SELECT * FROM extractions WHERE extraction_id = ?",
                                  (row["current_extraction_id"],)).fetchone()
        conn.close()
        self.assertEqual(transcript["text"], "we ship on friday")
        self.assertEqual(extraction["summary"], "summary of: we ship on friday")
        self.assertEqual(extraction["transcript_id"], transcript["transcript_id"])

    def test_retried_transcription_is_the_one_summarized(self):
        job_id, _ = support.insert_job("QUEUED", support.mulaw_wav())
        self.transcripts = ["first transcript"]
        self.run_worker()
        first_id = support.job_row(job_id)["current_transcript_id"]

        admin = admin_service.app.test_client()
        response = admin.post(f"/api/jobs/{job_id}/retry", json={"stage": "transcribe"},
                              headers={"Authorization": "Bearer dev-admin-token"})
        self.assertEqual(response.status_code, 202)

        self.transcripts = ["second transcript"]
        self.run_worker()

        row = support.job_row(job_id)
        self.assertEqual(row["state"], "COMPLETED")
        self.assertNotEqual(row["current_transcript_id"], first_id)
        self.assertTrue(self.prompts[-1].endswith("second transcript"))
        meeting = admin.get(f"/api/meetings/{job_id}", headers={"Authorization": "Bearer dev-admin-token"})
        self.assertEqual(meeting.get_json()["transcript"], "second transcript")

    def test_transcription_error_marks_failed_asr(self):
        job_id, _ = support.insert_job("QUEUED", support.mulaw_wav())
        self.fail_transcription = True
        self.run_worker()
        row = support.job_row(job_id)
        self.assertEqual(row["state"], "FAILED_ASR")
        self.assertIn("could not decode", row["last_error_message"])


if __name__ == "__main__":
    unittest.main()
