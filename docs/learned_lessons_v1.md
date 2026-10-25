# **Meeting Companion — Lessons Learned from v1**

These lessons come from building v1 (2026-10-14 → 2026-10-25). Sources: the commit history, the current firmware and backend code, and a comparison of `ImplementationPlan-V1.md` with what was built (`ImplementationPlan-V2.md`).

Lessons fixed in the v2.1 defect round are marked **Status (v2.1)**. §10 holds the lessons learned while making those fixes.

Each lesson has three parts:

* **What happened:** the event or finding.
* **Lesson:** the general rule.
* **Apply:** the concrete change for v2.x.

## **1. Planning and scope**

### **1.1 The plan was a production spec; the build was a prototype**

**What happened:** v1 specified leases, fencing, F_FULLFSYNC ordering, replay nonces, a 12-field signature, TLS with CA pinning, FTS, participants, retention, and backups. The code delivered a working pipeline: record, upload, transcribe, summarize, view. Most of the hardening was not built. Many tables were created and never used.

**Lesson:** a spec written as "the final system" does not show what to build first. The gaps also stay hidden, because the schema looks complete.

**Apply:** keep the plan in two layers: *as built* and *next milestone*, as v2 does. Add a table or column only when the code that writes it ships in the same change.

### **1.2 Spec drift happened silently at the device–server contract**

**What happened:** device and server agreed on a **5-field** HMAC string and a server-made `recording_id`. Neither matches v1. Nothing failed, because both sides drifted together.

**Lesson:** a contract that lives only in prose drifts. A contract with test vectors does not.

**Apply:** publish signature test vectors: fixed inputs and the expected hex. Check them in the Python tests and in a firmware self-test at boot (debug builds). Version the contract (`X-Sig-Version`) from day one.

### **1.3 Architecture decisions changed without a recorded decision**

**What happened:** v1 put Claude at the center of extraction. The code switched to a local MLX LLM (Qwen2.5-3B), but `anthropic` and `CLAUDE_API_KEY` stayed in the repo. The README tells users to accept a Llama license that the code never uses.

**Lesson:** when a core dependency changes, update the docs and remove dead config in the same change. Otherwise the next reader sets up the wrong thing.

**Apply:** record the change as decision D6 (done in v2). Remove the unused `anthropic`, `flask-cors`, `requests`, and `CLAUDE_API_KEY`, or mark them "reserved for D6". Fix the README model instructions.

## **2. Firmware: hardware and power**

### **2.1 SD card bandwidth is the recording bottleneck**

**What happened:** 16-bit PCM at 16 kHz (32 KB/s) caused SD card timeouts (0x107) while recording (commit `51fce7d`). Converting to G.711 µ-law halved the write rate to 16 KB/s and fixed the problem.

**Lesson:** on this board, plan for SD write latency, not just the average throughput. Lowering the data rate fixed it more reliably than any buffering change.

**Apply:** keep µ-law as the device format. If 16-bit audio is ever needed, add a double buffer in PSRAM with a separate writer task first, and measure the worst-case `fwrite` latency.

### **2.2 Wi-Fi TX bursts plus SD reads can brown out the 3.3 V rail**

**What happened:** uploads were unstable until TX power was dropped to 8.5 dBm and modem sleep was turned off during the transfer.

**Lesson:** a peak current problem looks like a network bug.

**Apply:** keep the reduced TX power during uploads. Log brownout resets (`esp_reset_reason()`) so a regression shows up.

**Regression (v2.1 → fixed in v2.2.1):** when the upload function was rewritten, these settings moved after the SHA-256 pass, so the full-file hash read ran at full TX power. The rule is "before **any** SD read of the recording", not "before sending". Put a comment next to a setting like this that says why it sits where it does, so a refactor doesn't move it.

### **2.3 Power: radio off by default, CPU scaled, light sleep between ticks**

**What happened:** battery life improved in steps:

* Wi-Fi is on only for syncs.
* The CPU runs at 80 MHz on the clock screen.
* The clock screen light-sleeps until the next minute.
* The codec and I2S clocks go to standby when leaving the meeting screen.

Commits `5f79b00`, `6cad91b`, `b9c2852`.

**Lesson:** the biggest savings came from turning whole subsystems off, not from tuning delays.

**Apply:** keep the rule "every screen states its radio, CPU, and codec state" (V2 §18.1). Retest the light-sleep GPIO hold list whenever the pin usage changes.

### **2.4 Avoid duplicate radio wake-ups**

**What happened:** on a sync minute, both the weather trigger and the offline retry tried to connect to Wi-Fi (fixed in `16eeb40`).

**Lesson:** when several schedulers can wake the radio, they need one owner, or an explicit "already tried this minute" flag.

**Apply:** move all network work into one sync function that a single scheduler calls.

### **2.5 Stack size and the watchdog**

**What happened:** the 2 KiB hashing and send buffers moved to the heap to prevent stack overflow. Short `delay(2)` calls keep the watchdog fed in long loops.

**Lesson:** Arduino `loop()` has a small stack, and long SHA-256/streaming loops starve the idle task.

**Apply:** allocate large buffers once, globally or in PSRAM, rather than with malloc on every call. Consider a dedicated upload task with its own stack.

### **2.6 Do not reach into private members**

**What happened:** `#define private public` exposes `I2cMasterBus::user_i2c_handle` for the SHTC3 driver.

**Lesson:** this works today, but it breaks silently when the BSP changes.

**Apply:** add a public getter to `i2c_bsp.h`.

## **3. Firmware: time**

### **3.1 Confirm NTP before you trust it**

**What happened:** the RTC was overwritten with bad time when NTP had not actually answered. The fix waits for the SNTP sync callback before setting the RTC (`7345175`).

**Lesson:** `configTime()` returning does not mean the time is valid.

**Apply:** keep the callback gate.

### **3.2 Resolve the timezone before syncing time**

**What happened:** the GMT offset has to be known before NTP sets the local RTC. The firmware therefore calls Open-Meteo (`utc_offset_seconds`) first, then NTP. Timezone strings were replaced with a numeric offset.

**Lesson:** order the dependencies explicitly: location → offset → time.

**Apply:** keep the order. Note that an integer-hour offset cannot represent offsets like UTC+5:30. Store seconds.

### **3.3 The RTC and the system clock are two clocks**

**What happened:** the RTC holds local time, and the system clock (`time(NULL)`) is set only by SNTP. After a boot with no internet, `time(NULL)` is near 1970. Every upload then fails with 401 `clock_skew`, even though the screen shows the right time.

**Lesson:** any clock that signs requests must be seeded at boot from the battery-backed RTC.

**Apply:** at boot, call `settimeofday()` from the RTC, converting local time to UTC with the stored offset. Store the RTC in UTC if possible.

**Status (v2.1): fixed.** `seedSystemClockFromRTC()` runs at boot, after the SD configs load.

### **3.4 `esp_sntp_get_sync_status()` is not a "clock is valid" flag**

**What happened:** `clock_valid` is computed from `esp_sntp_get_sync_status() == COMPLETED`. In ESP-IDF this status is reset after it is read. Also, SNTP resyncs cannot succeed while the radio is off. So the value says little about whether the clock is good.

**Lesson:** track your own flag (set it in the sync callback, with a timestamp) and define what "valid" means. For example: synced within the last 24 hours.

**Status (v2.1): fixed.** `clockSyncedSinceBoot` is set by a confirmed NTP sync, and its value at recording start is stored in the recording's filename (`c1`/`c0`).

## **4. Firmware: recording and upload**

### **4.1 Identity must persist with the file**

**What happened:** `current_recording_id` and `clock_valid` live in RAM. When the device retries an old `_failed.wav`, it sends the **current** recording's UUID. The server had to ignore `X-Recording-Id` and build an identity from the filename timestamp instead.

**Lesson:** anything needed for an idempotent retry must be written to storage at recording start. This is the v1 sidecar rule, and skipping it caused real bugs.

**Apply:** store the identity with the file at creation, and read it back for every retry.

**Status (v2.2): fixed.** v2.1 used a sidecar `.json`. v2.2 puts the `recording_id` in the filename instead (lesson 10.9). The server honors `X-Recording-Id`.

### **4.2 Only retrying after the next success leaves recordings stranded**

**What happened:** failed uploads are retried only after a later recording uploads successfully.

**Lesson:** a retry should be triggered by the network being available, not by the user's next action.

**Apply:** run a retry pass on the scheduled sync minutes when Wi-Fi is up and the device is not recording. Use backoff with jitter.

### **4.3 Classify the response before you retry**

**What happened:** success means "the status line contains 200 or 202". Everything else, including 409 duplicate (the server already has the file) and permanent 4xx errors, is marked `_failed` and retried forever.

**Lesson:** "retry everything" turns permanent errors into endless traffic, and turns success into data loss once the cleanup runs.

**Apply:** implement the v1 §18 response table. At minimum: 409 duplicate → uploaded; 401 `clock_skew` → resync and retry; other 4xx → stop and show the error.

**Status (v2.1): fixed.** `classifyUploadResponse()` sorts each response into OK, retry, clock-skew, or rejected, and the file gets an `_OK`, `_failed`, or `_rejected` suffix.

### **4.4 Automatic deletion must never remove unsent data**

**What happened:** the boot cleanup deletes every `.wav` older than 48 h, including `_failed` and `_pending` files.

**Lesson:** retention rules must depend on the status, not only on age.

**Apply:** delete only `_OK` files automatically.

**Status (v2.1): fixed.** Only `_OK` files expire.

### **4.5 Plan for power loss mid-recording**

**What happened:** the WAV header is written as zeros and fixed only on stop. A power loss leaves a `_pending.wav` with an invalid header that is never uploaded.

**Apply:** at boot, repair `_pending` files (data size = file size − 44), then queue them for upload.

**Status (v2.1): fixed.** `recoverPendingRecordings()` repairs `_pending` files at boot.

## **5. Firmware: connectivity and configuration**

### **5.1 Configuration on SD is convenient but plaintext**

**What happened:** Wi-Fi passwords, the device ID, and the HMAC secret are in text files on a removable card.

**Lesson:** this is fine for a prototype. Anyone with the card has the device's identity.

**Apply:** move the HMAC secret to NVS (encrypted when flash encryption is on). Keep only non-secret settings on SD.

### **5.2 Geolocation needs fallbacks, and the user needs an override**

**What happened:** BSSID geolocation (mylnikov) sometimes fails. IP geolocation is the fallback, and `override=true` lets the user pin the location (`7951d49`).

**Lesson:** free location APIs are best-effort. Always cache the last good result per network.

### **5.3 "Connected to Wi-Fi" is not "online"**

**What happened:** retries had to handle a network with no internet (`5f59071`).

**Apply:** treat each API result (weather OK, NTP OK) as the health signal. Do not use `WL_CONNECTED` for that.

### **5.4 The captive portal is an open AP**

**What happened:** the portal is an open AP, and the form inserts scanned SSIDs into HTML without escaping.

**Apply:** escape SSIDs, and add a timeout that closes the AP.

## **6. Backend: ingest and durability**

### **6.1 Keep the DB lock away from slow I/O**

**What happened:** the ingest handler closes its SQLite connection while the body streams. The worker closes its connection during MLX inference.

**Lesson:** this was the right call. Keep it whenever code changes.

### **6.2 A complete-looking schema hid missing behavior**

**What happened:** `replay_nonces`, `search_fts`, `speakers`, the item tables, `admins`, and `api_tokens` all exist and are all empty. `current_transcript_id` is never set, so re-transcription can summarize an old transcript.

**Lesson:** a pointer column (`current_*_id`) is worthless unless every read goes through it.

**Apply:** add a test that re-transcribes a job and checks that the summary uses the newest transcript.

**Status (v2.1): pointer fixed.** All transcript reads go through `get_current_transcript()`. This was verified with a re-transcription run. The other empty tables remain.

### **6.3 Crash recovery is not optional, even for one user**

**What happened:** a job in `TRANSCRIBING` or `EXTRACTING` when the process stops is stuck forever.

**Apply:** do the startup recovery step (V2 §16.2) before anything else in P1.

**Status (v2.1): fixed.** `recover_on_startup()` runs in the ingest service, before the worker starts.

### **6.4 The duplicate check without a digest compare is a silent overwrite risk**

**What happened:** a different file with the same `recording_started_at` returns "duplicate", and the new audio is dropped. Two concurrent uploads of one recording hit a UNIQUE error and get a 500.

**Apply:** compare `audio_sha256` and return `recording_conflict`. Hold an in-memory "uploading" set and return `upload_in_progress`.

**Status (v2.1): conflict fixed.** The server compares digests and returns `recording_conflict`. The `upload_in_progress` guard is still open.

### **6.5 Know what your HTTP server does with `Expect: 100-continue`**

**What happened:** the device sends `Expect: 100-continue` so the server can reject a request before the body. Werkzeug's dev server probably answers 100 Continue itself, before Flask's auth runs. If so, the body is sent even for rejected requests.

**Apply:** spike this (or move to a server where the app controls the 100 response), or add a `/api/upload/preflight` endpoint as v1 already proposed.

**Status (v2.1): confirmed and fixed.** See lesson 10.1.

### **6.6 Seeding with `INSERT OR IGNORE` hides config changes**

**What happened:** changing `ESP32_SECRET` in `.env` has no effect after the first run. `key_id` is also hard-coded to `primary-key-01`, and the device config must match it.

**Apply:** add a small CLI (`meetingctl device add / key rotate`), or upsert with a log line. Document the required `key_id`.

### **6.7 Relative paths tie the services to their working directory**

**What happened:** `meeting_companion.db`, `audio/`, and `staging/` are relative, and `audio_relpath` stores a path relative to the working directory.

**Apply:** resolve paths from `__file__` or an env var. Store `audio_relpath` relative to `AUDIO_DIR`.

## **7. Backend: AI pipeline**

### **7.1 Local-only is feasible on Apple Silicon**

**What happened:** `whisper-turbo` plus Qwen2.5-3B 4-bit run in one process, with models loaded once, and produce a usable summary. No network calls are needed after the model download.

**Lesson:** the privacy boundary can be "nothing leaves the Mini". This is stronger than v1 assumed.

### **7.2 Free-text summaries are not extraction**

**What happened:** the prompt asks for a summary, speakers, and actions, but the output is unstructured text. `max_tokens=1024`, and nothing checks the context length.

**Apply:** before building the participants or items features, get schema-valid JSON from whichever model D6 picks. Count tokens first. Store the real model ID and the `prompt_version`.

### **7.3 Store what you will need later**

**What happened:** only the full transcript text is stored. There are no segments or timestamps, so evidence links and "jump to audio" are impossible without transcribing again.

**Apply:** store Whisper segments (`transcript_segments`) now, even before the UI uses them.

## **8. Backend: admin and dashboard**

### **8.1 Dev auth shortcuts become the real auth**

**What happened:** any bearer token or any session cookie passes. The token is in the page source. The localhost bind is the only protection.

**Apply:** before any remote access (even an SSH tunnel shared with others), replace this with one hashed admin token, and protect `/dashboard` too.

### **8.2 Escape everything that comes from outside**

**What happened:** the dashboard uses `innerText` for the transcript and summary (good), but `innerHTML` for job rows that include `device_id`.

**Apply:** use `textContent` everywhere, and add a CSP header.

## **9. Process**

* **Small, frequent commits worked.** Most lessons above can be traced to a single commit message.
* **Hardware bugs looked like software bugs:** SD timeouts, brownouts, and a 1970 clock. When something is flaky, measure the voltage, latency, and timestamps before rewriting the logic.
* **No automated tests yet.** The first tests should be the cheapest guards: HMAC test vectors, the duplicate and conflict rules, restart recovery, and the newest-transcript rule.
* **Write the "as built" doc at each milestone.** This comparison found real defects, such as the RTC/system-clock gap, retry identity, and the stale transcript, that neither the plan nor the code showed by itself.

## **10. Lessons from the v2.1 fix round**

### **10.1 The documented hook is not always the one the server uses**

**What happened:** the plan was to override `handle_expect_100()`. That is the hook Python's `http.server` documents for this purpose. A socket test showed the override changed nothing: Werkzeug 3.1 writes `100 Continue` directly in `WSGIRequestHandler.run_wsgi()`, before the app runs. The working fix hides the `Expect` header from `run_wsgi()`, passes the flag through the WSGI environ, and writes `100 Continue` to `werkzeug.socket` once the header checks pass.

**Lesson:** for protocol behavior, test against the real server. A five-line raw-socket client found the problem in seconds, while reading docs pointed at the wrong hook.

**Apply:** keep a raw-socket check of "rejected request → no `100 Continue`" in the future test suite. Recheck it on every Werkzeug upgrade, because the fix depends on Werkzeug internals (`run_wsgi`, `werkzeug.socket`). A production server (or a preflight endpoint) would remove that dependency.

### **10.2 Changing an identity scheme needs a path for old data**

**What happened:** the server switched from a filename-derived `recording_id` to the device's UUID. Files already on SD cards carry no ID. If the device made up a new UUID for them, a retry would create a second job for a recording the server already had.

**Lesson:** when an idempotency key changes, existing data must keep its old key.

**Apply (done):** the device omits `X-Recording-Id` for legacy names that carry no ID, and the server then uses the legacy key. Remove the fallback only when no legacy files can remain (after all `_failed` files from before v2.1 are uploaded).

### **10.3 Recovery belongs to the process that owns the work**

**What happened:** both services call `init_db()` at startup. Putting job recovery there would let an admin-service restart push a job that the ingest worker is processing back to `QUEUED`, so it would run twice.

**Lesson:** "return in-flight jobs" is only safe in the process that owns the workers, and only before they start.

**Apply (done):** `recover_on_startup()` runs only in the ingest service's `__main__`. When workers move to another process, use leases instead.

### **10.4 A pointer column needs a fallback for rows written before it existed**

**What happened:** existing jobs have `current_transcript_id = NULL`. `get_current_transcript()` uses the pointer when it is set, and falls back to the newest transcript when it is not.

**Lesson:** every new invariant needs a rule for the old rows. Here a backfill was not needed, because "newest" is the correct answer for legacy rows.

### **10.5 Look at the whole response, not a substring**

**What happened:** the old upload code waited for a status line, and treated any line that did not contain " 200" or " 202" as an early rejection. A duplicate answered early with `202` (now normal, since the server skips the body for duplicates) would have left the device waiting, then sending the body to a closed socket, and then marking the file failed.

**Lesson:** parse the status code. Treat a final response before the body as final. Read `error_code` from the body instead of guessing from the status.

### **10.6 Rename only after readdir() is done**

**What happened:** the retry pass renamed files while `readdir()` was still walking the same FAT directory. That can skip entries or return a renamed file twice.

**Apply (done):** collect the names first, then process them.

### **10.7 A compile check is cheap, even without the board**

**What happened:** the Arduino IDE bundles `arduino-cli`, so the firmware could be compiled from the command line (`esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=huge_app`, `--warnings all`). The firmware still has not run on hardware.

A second lesson came out of this: a rebuild into a reused `--build-path` does not recompile unchanged objects, so it prints no warnings. The first check reported "no warnings", but a full rebuild showed ArduinoJson v6-API deprecations (the installed library is 7.4.3).

**Apply:** compile from the CLI before every firmware commit, with a fresh build path when you are checking warnings. Plan a hardware checklist (V2 §22) for behavior a compiler cannot check: power loss, offline boot, real HTTP exchanges.

### **10.8 Test fixtures need the same path rules as the code**

**What happened:** the first scratch test failed with `unknown_key`. `load_dotenv()` looks for `.env` next to the module, not in the working directory, while the DB and audio paths are relative to the working directory.

**Lesson:** this is lesson 6.7 again. When paths come from two different bases, setup mistakes are easy, and they surprise people in tests as well as in production.


### **10.9 Let the filesystem be the state machine**

**What happened:** the v2.1 fix for recording identity added a sidecar `.json` per recording. A review pointed out that this undoes the strength of the original design. The transmission state lived in the filename (`_pending` → `_OK` / `_failed`), and a `rename()` is atomic, so a battery cut can never leave the state half-written. A second file adds a second write that can be torn (it could be truncated, or exist without its WAV). It also needs cleanup rules of its own.

**Lesson:** on a battery device with removable storage, keep every piece of per-file state in the filename, as long as it fits:

* Fixed facts (identity, clock flag) are written once, when `fopen()` creates the file.
* Changing facts (status) change only through `rename()`.

Use a manifest only when the state cannot fit in a name.

**Apply (v2.2):** the file is named `YYYYMMDD_HHMMSS_<recording_id>_c<0|1>_<status>.wav`. The sidecar, its read/write helpers, and the `current_recording_*` RAM globals were removed.

### **10.10 A local-time clock and its timezone must change together**

**What happened:** a review pointed out two things. The firmware never resynced the clock after a city change. Separately, it suggested calling `applyTimezone()` whenever `gmt_offset` changes. The first was a real, original bug: the comment promised "sync time immediately", but no code did it. The second fix would have caused a new bug. The RTC and the filename timestamps are in local time **as written with the old offset**. Switching `TZ` without rewriting the RTC would shift every `mktime()` result (recording start times, the 48 h cleanup) by the offset difference.

The review also assumed `time(NULL)` depends on `TZ`. It doesn't: `time(NULL)` is UTC, so the HMAC request timestamp was never affected.

**Lesson:** when a clock stores local time, the offset is part of the stored data. Change the stored time and the offset in one step, or not at all. Trigger the resync on what actually changed (the offset), not on a proxy like the city name, which misses DST and fires needlessly between cities in one timezone.

**Apply (v2.2.3):** `rtc_gmt_offset`, `resyncClockIfOffsetChanged()`, and `syncNTP()` setting `TZ` only after the RTC write succeeds.

### **10.11 Re-verify review claims against the current code**

**What happened:** two reviews in a row reported that the backend reads `request.files` and rejects raw uploads with 400. It doesn't, and never did in this version: it streams `request.stream`, and real µ-law uploads are `COMPLETED`.

**Lesson:** a review's "fix" is only as good as the version it read. Check each claim with a grep or a test before acting on it.

### **10.12 "Off" has to be checked on every path, including errors and inherited setup**

**What happened:** while making recording quiet, three gaps turned up:

* The meeting screen powered the speaker DAC and drove the amplifier high, although the firmware never plays sound. This came from the vendor's echo example.
* A failed SHTC3 read returned before sending the sleep command, so a single I2C or CRC error left the sensor awake.
* "Wi-Fi off while recording" held only because each caller remembered to check `!isRecording`.

**Lesson:** a power state must be enforced where the resource is woken (`ensureWiFiConnected()`, `shtc3_read_i2cbus()`), not only by each caller. Every early `return` after a wake-up needs the matching sleep. Vendor examples carry setup for features you don't use, so review what they switch on.

On a shared full-duplex I2S port, read the driver before closing one direction. Here `esp_codec_dev` keeps TX clocked while RX is open, which makes closing the speaker safe for the mics. The opposite assumption could have silenced every recording.
