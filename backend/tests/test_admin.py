"""Admin API: audio playback and the dashboard template (V2 §12.2, §13, changelog v2.3)."""
import hashlib
import os
import struct
import unittest
import warnings

import support
import admin_service

AUTH = {"Authorization": "Bearer dev-admin-token"}


def ulaw_reference(byte):
    """Scalar G.711 µ-law decoder, written independently of the server's numpy table."""
    u = ~byte & 0xFF
    sign, exponent, mantissa = u & 0x80, (u >> 4) & 0x07, u & 0x0F
    magnitude = ((mantissa << 3) + 0x84) << exponent
    return 0x84 - magnitude if sign else magnitude - 0x84


class AudioTests(unittest.TestCase):
    def setUp(self):
        support.reset_state()
        self.client = admin_service.app.test_client()

    def test_requires_admin_auth(self):
        job_id, _ = support.insert_job("COMPLETED", support.mulaw_wav())
        self.assertEqual(self.client.get(f"/api/meetings/{job_id}/audio").status_code, 401)

    def test_ulaw_is_decoded_to_pcm16_and_the_stored_file_is_untouched(self):
        body = support.mulaw_wav(n_samples=2048)
        job_id, path = support.insert_job("COMPLETED", body)

        response = self.client.get(f"/api/meetings/{job_id}/audio", headers=AUTH)
        self.assertEqual(response.status_code, 200)
        wav = response.data
        self.assertEqual(int(response.headers["Content-Length"]), len(wav))

        fmt_tag, channels, rate, byte_rate, align, bits = struct.unpack("<HHIIHH", wav[20:36])
        self.assertEqual((fmt_tag, channels, rate, byte_rate, align, bits), (1, 1, 16000, 32000, 2, 16))
        samples = struct.unpack(f"<{2048}h", wav[44:])
        source = body[44:]
        self.assertEqual(list(samples), [ulaw_reference(b) for b in source])

        with open(path, "rb") as f:
            self.assertEqual(hashlib.sha256(f.read()).hexdigest(), hashlib.sha256(body).hexdigest())

    def test_known_ulaw_values(self):
        table = admin_service.ULAW_TO_PCM16
        self.assertEqual(int(table[0xFF]), 0)
        self.assertEqual(int(table[0x00]), -32124)
        self.assertEqual(int(table[0x80]), 32124)

    def test_a_header_longer_than_the_file_is_capped_to_the_bytes_on_disk(self):
        body = support.mulaw_wav(n_samples=1000)
        body = body[:40] + struct.pack("<I", 999999) + body[44:]
        job_id, _ = support.insert_job("COMPLETED", body)
        response = self.client.get(f"/api/meetings/{job_id}/audio", headers=AUTH)
        self.assertEqual(len(response.data), 44 + 1000 * 2)

    def test_pcm16_is_sent_as_stored(self):
        body = support.pcm16_wav()
        job_id, _ = support.insert_job("COMPLETED", body)
        response = self.client.get(f"/api/meetings/{job_id}/audio", headers=AUTH)
        self.addCleanup(response.close)
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.data, body)

    def test_unknown_job_is_404(self):
        response = self.client.get("/api/meetings/no-such-job/audio", headers=AUTH)
        self.assertEqual(response.status_code, 404)

    def test_missing_file_is_410(self):
        job_id, path = support.insert_job("COMPLETED", support.mulaw_wav())
        os.remove(path)
        response = self.client.get(f"/api/meetings/{job_id}/audio", headers=AUTH)
        self.assertEqual(response.status_code, 410)
        self.assertEqual(response.get_json()["error_code"], "audio_missing")


class DashboardTests(unittest.TestCase):
    def test_admin_service_compiles_without_warnings(self):
        with open(admin_service.__file__, encoding="utf-8") as f:
            source = f.read()
        with warnings.catch_warnings():
            warnings.simplefilter("error")
            compile(source, admin_service.__file__, "exec")

    def test_dashboard_plays_audio_only_in_the_report(self):
        response = admin_service.app.test_client().get("/dashboard")
        self.assertEqual(response.status_code, 200)
        self.assertNotIn(b"Listen", response.data)
        self.assertIn(b'id="reportAudio"', response.data)
        self.assertIn(b"Download .wav", response.data)


if __name__ == "__main__":
    unittest.main()
