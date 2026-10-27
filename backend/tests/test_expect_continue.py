"""Expect: 100-continue on a real Werkzeug server (V2 §5, fix 8 of v2.1).

The firmware sends its headers, waits for "100 Continue", and only then streams the file.
The server must answer a rejected or duplicate request with its final status first, so
the device never uploads a body the server won't keep.
"""
import socket
import threading
import unittest

from werkzeug.serving import make_server

import support
import ingest_service


class ExpectContinueTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = make_server("127.0.0.1", 0, ingest_service.app, threaded=True,
                                 request_handler=ingest_service.DeferredContinueRequestHandler)
        cls.port = cls.server.server_port
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()

    def setUp(self):
        support.reset_state()

    def open_upload(self, body, **kwargs):
        """Sends only the request head, as the device does before waiting for 100 Continue."""
        headers = support.signed_headers(body, **kwargs)
        headers["Expect"] = "100-continue"
        head = "POST /api/upload HTTP/1.1\r\nHost: test\r\nConnection: close\r\n"
        head += "".join(f"{k}: {v}\r\n" for k, v in headers.items()) + "\r\n"
        sock = socket.create_connection(("127.0.0.1", self.port), timeout=5)
        sock.sendall(head.encode())
        return sock

    @staticmethod
    def read_head(sock):
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = sock.recv(1)
            if not chunk:
                break
            data += chunk
        return data.decode("latin-1")

    def test_bad_signature_gets_401_without_100_continue(self):
        sock = self.open_upload(support.mulaw_wav(), recording_id=support.new_recording_id(),
                                secret="not-the-secret")
        try:
            self.assertTrue(self.read_head(sock).startswith("HTTP/1.1 401"))
        finally:
            sock.close()

    def test_valid_upload_gets_100_continue_then_202(self):
        body = support.mulaw_wav()
        sock = self.open_upload(body, recording_id=support.new_recording_id())
        try:
            self.assertTrue(self.read_head(sock).startswith("HTTP/1.1 100 Continue"))
            sock.sendall(body)
            self.assertTrue(self.read_head(sock).startswith("HTTP/1.1 202"))
        finally:
            sock.close()

    def test_body_cut_short_is_discarded(self):
        body = support.mulaw_wav()
        sock = self.open_upload(body, recording_id=support.new_recording_id())
        try:
            self.read_head(sock)
            sock.sendall(body[:len(body) // 2])
            sock.shutdown(socket.SHUT_WR)  # The device gives up mid-body, as on an SD read error
            self.assertTrue(self.read_head(sock).startswith("HTTP/1.1 499"))
        finally:
            sock.close()
        self.assertEqual(support.files_under(ingest_service.STAGING_DIR), [])
        self.assertEqual(support.files_under(ingest_service.AUDIO_DIR), [])

    def test_duplicate_gets_its_answer_before_the_body_is_sent(self):
        body = support.mulaw_wav()
        recording_id = support.new_recording_id()
        first = self.open_upload(body, recording_id=recording_id)
        try:
            self.read_head(first)
            first.sendall(body)
            self.read_head(first)
        finally:
            first.close()

        second = self.open_upload(body, recording_id=recording_id)
        try:
            self.assertTrue(self.read_head(second).startswith("HTTP/1.1 202"))
        finally:
            second.close()


if __name__ == "__main__":
    unittest.main()
