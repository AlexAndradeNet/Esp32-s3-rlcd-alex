"""recover_on_startup(): the disk and the database agree again after a crash (V2 §16, fix 1 of v2.1)."""
import os
import unittest

import support
import db


class RecoveryTests(unittest.TestCase):
    def setUp(self):
        support.reset_state()

    def test_recovery_after_a_crash(self):
        with open(os.path.join(db.STAGING_DIR, "partial.part"), "wb") as f:
            f.write(b"half an upload")
        orphan_dir = os.path.join(db.AUDIO_DIR, "2026", "10")
        os.makedirs(orphan_dir, exist_ok=True)
        with open(os.path.join(orphan_dir, "unregistered.wav"), "wb") as f:
            f.write(support.mulaw_wav())

        transcribing, _ = support.insert_job("TRANSCRIBING", support.mulaw_wav())
        extracting, _ = support.insert_job("EXTRACTING", support.mulaw_wav(), transcript="text")
        queued_missing, _ = support.insert_job("QUEUED")
        completed, completed_path = support.insert_job("COMPLETED", support.mulaw_wav())

        db.recover_on_startup()

        self.assertEqual(os.listdir(db.STAGING_DIR), [])
        self.assertEqual(os.listdir(db.ORPHANS_DIR), ["unregistered.wav"])
        self.assertEqual(support.job_row(transcribing)["state"], "QUEUED")
        self.assertEqual(support.job_row(extracting)["state"], "TRANSCRIBED")
        missing = support.job_row(queued_missing)
        self.assertEqual(missing["state"], "FAILED_ASR")
        self.assertEqual(missing["last_error_code"], "audio_missing")
        self.assertEqual(support.job_row(completed)["state"], "COMPLETED")
        self.assertTrue(os.path.exists(completed_path))

    def test_registered_audio_is_never_moved(self):
        _, path = support.insert_job("COMPLETED", support.mulaw_wav())
        db.recover_on_startup()
        self.assertTrue(os.path.exists(path))
        self.assertEqual(os.listdir(db.ORPHANS_DIR), [])


if __name__ == "__main__":
    unittest.main()
