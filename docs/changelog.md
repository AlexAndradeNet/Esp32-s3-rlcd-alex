# **Changelog**

**v2.5 (2026-10-27): clear audio and a sturdier SD path.** See `progress_lessons/261027.md` for the story behind these changes.

* **µ-law encoder fixed:** `pcm16_to_ulaw()` now follows G.711. The old one mapped nearly every sample to the wrong segment, so every µ-law recording since 2026-10-23 is distorted: 1.8 dB SNR on a test tone, against 37.9 dB now.
* **SD recovery without a restart:**
  * The card is remounted when it stops answering, and a stale SD error is cleared on Screen 2 or at the next recording.
  * The SD pins keep their configuration during light sleep.
  * The card clock is 10 MHz.
* **Uploads:**
  * The whole recording is read (hashed) before the request opens, with one retry after a remount. A file the card can't read stays `_failed`.
  * The server answers **499 `incomplete_body`** when a body is cut short (it used to log a 500), and the device retries it later.
* **Server address:** `config_server.txt` is re-read before each upload batch. The parser ignores a BOM, spaces and CRLF. Screen 2 shows "Syncing to <host>".
* **Radio:** Wi-Fi stays at 8.5 dBm for every SD access (the whole upload batch, plus a scoped guard everywhere else).
* **Flash wear:**
  * Config files are written only when their content changes.
  * Recordings are written through a 16 KB buffer (32 sectors per write).
* **Battery:** no recording starts below 3.55 V. Below 3.45 V a running one is closed and kept for a later upload, without Wi-Fi.
* **Development:** with a computer on the USB port at boot, the clock screen skips light sleep, so the console stays connected.
* **Dashboard:** the Listen button is gone. Recordings play from View Report.

**Hardware note:** the SD errors of 2026-10-27 came from the card, not the firmware. It was recovered with `diskutil secureErase 0 disk4`: one pass of zeros over the whole device, which makes the card's controller rewrite every flash page and swap out blocks that no longer write. Then it was reformatted as FAT32 with a Master Boot Record. A quick format would not have helped, because it only rewrites the file table.

**v2.4.3 (2026-10-26): automated backend tests.** `backend/tests/` turns the §22 checks into 34 repeatable `unittest` tests that need no extra dependency (§22).

**v2.4.2 (2026-10-26):** documentation only. New §1.1 explains why the recorder is a dedicated device rather than a phone app.

**v2.4.1 (2026-10-25):** Bluetooth is explicitly off. `btStop()` runs in `setup()` and at record start. It was never started before this either, so this is a guard against future libraries, not a behavior change.

**v2.4 (2026-10-25): quiet recording.** While a recording runs, only the microphones are on (§18.2):

* **Internet off:** the radio is forced off, SNTP is stopped, and every Wi-Fi path refuses to run while recording.
* **Speaker off:** the ES8311 speaker DAC and its amplifier are now off on the Meeting screen. They used to be powered there even though nothing ever plays.
* **Sensors idle:** the SHTC3 sensor sleeps, and sensor and battery reads are refused while recording.
* **SHTC3 bug fixed:** a failed SHTC3 read (I2C or CRC error) used to return before sending the sensor back to sleep. It now always does.

**Still to check on the board:** a recording still captures sound after the speaker change. The serial log should show `Pending out channel for in channel running` when entering screen 2.

**v2.3 (2026-10-25): listen to recordings; audio is never deleted.**

* The dashboard has a **Listen** button on every job, and a player with **Download .wav** in the report.
* The audio route decodes µ-law to 16-bit PCM on the fly, so every browser can play it. It also returns 410 for a missing file (it used to return 500).
* The server no longer deletes a stored recording in any path. A verified upload whose DB insert fails is moved to `audio/orphans/` (§15.1).
* `numpy` is now a direct dependency.

**v2.2.4 (2026-10-25):** a duplicate of a `COMPLETED` job now returns `{job_id, status: "COMPLETED", duplicate: true}` (200). The response used to include the full extraction as `data`, sent over plain HTTP to a device that ignored it. The device classifies by status code, so its behavior is unchanged.

**v2.2.3 (2026-10-25): clock follows the UTC offset.** The RTC holds local time, but before this change it was rewritten only at boot, on a first-time network, and at 00:00. A new offset (a known network in another city, a DST change, or an Open-Meteo correction) left the displayed time wrong for up to a day. The comment "if city changed, sync time immediately" had no matching code, even in the original firmware. Now `rtc_gmt_offset` records the offset the RTC was last written with. After every localization or weather step, `resyncClockIfOffsetChanged()` runs NTP when the offset differs. `syncNTP()` updates `TZ` together with the RTC. Also in this change: string checks on the Wi-Fi network list are null-safe (ArduinoJson `==`), so a hand-edited entry with a non-string `essid` or `city` can no longer crash the device, and a leftover `.wav` literal was removed from the retention `sscanf`.

**v2.2.2 (2026-10-25): ArduinoJson 7.** The firmware moved from the deprecated v6 API to ArduinoJson 7.4.3. Each `DynamicJsonDocument(N)` became an elastic `JsonDocument`, `containsKey()` became `isNull()` checks, and `createNestedObject()` became `add<JsonObject>()`. Documents now grow on the heap as needed, so a large response no longer fails against a fixed capacity. One example: Open-Meteo's hourly arrays used to be parsed into a 12 KB document whose parse result was never checked.

**v2.2.1 (2026-10-25):** the upload's Wi-Fi power settings (no modem sleep, 8.5 dBm) are applied before the SHA-256 pass again, as in the original firmware. v2.1 had moved them after the hash, which left that long SD read at full TX power (§18.3).

**v2.2 (2026-10-25): filesystem as state machine.** The recording sidecar `.json` from v2.1 is gone. The filename now holds the recording's whole state: `YYYYMMDD_HHMMSS_<recording_id>_c<0|1>_<status>.wav`. The identity and clock flag are fixed when `fopen()` creates the file, and the status changes only through an atomic `rename()`. There is no RAM tracking and no second file that a brownout could leave half-written (§18.2). The `current_recording_*` globals were removed. No sidecar firmware had been flashed, so no migration is needed.

**v2.1 (2026-10-25): defect fixes.** The first v2 review found eight defects. All eight are now fixed:

| # | Defect | Fix | Where |
| ----- | ----- | ----- | ----- |
| 1 | A crash left jobs stuck in `TRANSCRIBING`/`EXTRACTING` forever | `recover_on_startup()` runs before the worker and listener: it deletes staging files, sweeps orphans, marks missing audio, and returns in-flight jobs (§16) | `db.py`, `ingest_service.py` |
| 2 | Re-transcription could summarize an old transcript | The worker sets `current_transcript_id`, and every read goes through `get_current_transcript()` (§8) | `db.py`, `ingest_service.py`, `admin_service.py` |
| 3 | Offline boot left the system clock near 1970, so uploads got 401 `clock_skew` | `seedSystemClockFromRTC()` at boot. `clock_valid` now comes from a flag set by a confirmed NTP sync (§18.5) | firmware |
| 4 | Retries of older files sent the latest recording's ID | The `recording_id` is stored with the file at recording start (since v2.2 in the filename) and read back on every retry. The server honors `X-Recording-Id` and returns `recording_conflict` when the digest differs (§6, §18.2) | firmware, `ingest_service.py` |
| 5 | Boot cleanup deleted unsent recordings after 48 h | Only `_OK` files expire (§15.2) | firmware |
| 6 | 409 duplicate was retried forever | The device parses the status code and `error_code`. 409 `duplicate` counts as uploaded, and permanent 4xx errors become `_rejected` (§18.4) | firmware, `ingest_service.py` |
| 7 | `_pending` files from a power loss were never uploaded | `recoverPendingRecordings()` rewrites the header from the file size at boot and queues the file as `_failed` (§18.2) | firmware |
| 8 | Werkzeug sent `100 Continue` before auth, so rejected requests still sent the whole body (confirmed by test) | `DeferredContinueRequestHandler` hides `Expect` from Werkzeug. The handler sends `100 Continue` only after every header check passes (§5) | `ingest_service.py` |

**v2.0 (2026-10-25):** first as-built description.