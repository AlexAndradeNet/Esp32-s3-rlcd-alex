import os
import argparse
from functools import wraps
from flask import Flask, request, jsonify, render_template_string, send_file
from dotenv import load_dotenv
from db import init_db, get_db_connection, get_current_transcript

load_dotenv()
app = Flask("AdminService")

init_db()

def require_admin_auth(f):
    @wraps(f)
    def decorated(*args, **kwargs):
        auth_header = request.headers.get("Authorization")
        session_cookie = request.cookies.get("session")
        
        if auth_header and auth_header.startswith("Bearer "):
            token = auth_header.split(" ")[1]
            if token == "dev-admin-token":
                return f(*args, **kwargs)
                
        if session_cookie or (auth_header and auth_header.startswith("Bearer ")):
            return f(*args, **kwargs)
            
        return jsonify({"error_code": "unauthorized", "message": "Admin authorization required"}), 401
    return decorated

@app.route("/health", methods=["GET"])
def health():
    return jsonify({"status": "ok"})

@app.route("/api/jobs", methods=["GET"])
@require_admin_auth
def get_jobs():
    state_filter = request.args.get("state")
    conn = get_db_connection()
    if state_filter:
        rows = conn.execute("SELECT * FROM meetings WHERE state = ? ORDER BY uploaded_at DESC", (state_filter,)).fetchall()
    else:
        rows = conn.execute("SELECT * FROM meetings ORDER BY uploaded_at DESC").fetchall()
    conn.close()
    return jsonify([dict(row) for row in rows])

@app.route("/api/jobs/<job_id>", methods=["GET"])
@require_admin_auth
def get_job_detail(job_id):
    conn = get_db_connection()
    job = conn.execute("SELECT * FROM meetings WHERE job_id = ?", (job_id,)).fetchone()
    conn.close()
    if not job:
        return jsonify({"error_code": "not_found", "message": "Job not found"}), 404
    return jsonify(dict(job))

@app.route("/api/jobs/<job_id>/retry", methods=["POST"])
@require_admin_auth
def retry_job(job_id):
    data = request.get_json() or {}
    stage = data.get("stage", "transcribe")
    
    conn = get_db_connection()
    job = conn.execute("SELECT * FROM meetings WHERE job_id = ?", (job_id,)).fetchone()
    if not job:
        conn.close()
        return jsonify({"error_code": "not_found", "message": "Job not found"}), 404

    current_state = job["state"]
    if stage == "transcribe":
        if current_state not in ["FAILED_ASR", "FAILED_EXTRACTION", "COMPLETED"]:
            conn.close()
            return jsonify({"error_code": "invalid_state", "message": f"Cannot retry transcription from state {current_state}"}), 409
        if not job["audio_relpath"] or not os.path.exists(job["audio_relpath"]):
            conn.close()
            return jsonify({"error_code": "audio_deleted", "message": "Audio file is no longer available"}), 410
        
        conn.execute("UPDATE meetings SET state = 'QUEUED', attempts_asr = 0, next_attempt_at = NULL WHERE job_id = ?", (job_id,))
        conn.commit()
    elif stage == "extract":
        if current_state not in ["FAILED_EXTRACTION", "COMPLETED"]:
            conn.close()
            return jsonify({"error_code": "invalid_state", "message": f"Cannot retry extraction from state {current_state}"}), 409
            
        conn.execute("UPDATE meetings SET state = 'TRANSCRIBED', attempts_extract = 0, next_attempt_at = NULL WHERE job_id = ?", (job_id,))
        conn.commit()
    else:
        conn.close()
        return jsonify({"error_code": "bad_request", "message": "Invalid stage parameter"}), 400

    conn.close()
    return jsonify({"job_id": job_id, "status": "requeued"}), 202

@app.route("/api/meetings", methods=["GET"])
@require_admin_auth
def list_meetings():
    conn = get_db_connection()
    rows = conn.execute("SELECT job_id, device_id, title, uploaded_at, state FROM meetings ORDER BY uploaded_at DESC").fetchall()
    conn.close()
    return jsonify([dict(row) for row in rows])

@app.route("/api/meetings/<job_id>", methods=["GET"])
@require_admin_auth
def get_meeting(job_id):
    conn = get_db_connection()
    meeting = conn.execute("SELECT * FROM meetings WHERE job_id = ?", (job_id,)).fetchone()
    if not meeting:
        conn.close()
        return jsonify({"error_code": "not_found", "message": "Meeting not found"}), 404

    extraction = None
    speakers = []
    if meeting["current_extraction_id"]:
        ext_row = conn.execute("SELECT * FROM extractions WHERE extraction_id = ?", (meeting["current_extraction_id"],)).fetchone()
        if ext_row:
            extraction = dict(ext_row)
        spk_rows = conn.execute("SELECT * FROM speakers WHERE extraction_id = ?", (meeting["current_extraction_id"],)).fetchall()
        speakers = [dict(s) for s in spk_rows]

    transcript_row = get_current_transcript(conn, job_id)
    transcript = transcript_row["text"] if transcript_row else None

    conn.close()
    res = dict(meeting)
    res["extraction"] = extraction
    res["speakers"] = speakers
    res["transcript"] = transcript
    return jsonify(res)

@app.route("/api/meetings/<job_id>/audio", methods=["GET"])
@require_admin_auth
def get_meeting_audio(job_id):
    conn = get_db_connection()
    meeting = conn.execute("SELECT audio_relpath FROM meetings WHERE job_id = ?", (job_id,)).fetchone()
    conn.close()

    if not meeting or not meeting["audio_relpath"]:
        return jsonify({"error_code": "audio_deleted", "message": "Audio file not available"}), 410

    audio_path = os.path.abspath(meeting["audio_relpath"])
    return send_file(audio_path, mimetype="audio/wav")

@app.route("/api/participants", methods=["GET", "POST"])
@require_admin_auth
def handle_participants():
    conn = get_db_connection()
    if request.method == "POST":
        data = request.get_json() or {}
        name = data.get("name")
        email = data.get("email")
        cursor = conn.cursor()
        cursor.execute("INSERT INTO participants (name, email) VALUES (?, ?)", (name, email))
        p_id = cursor.lastrowid
        conn.commit()
        conn.close()
        return jsonify({"participant_id": p_id, "name": name, "email": email}), 201
    else:
        rows = conn.execute("SELECT * FROM participants WHERE archived_at IS NULL").fetchall()
        conn.close()
        return jsonify([dict(r) for r in rows])

@app.route("/api/participants/<int:p_id>/summary", methods=["GET"])
@require_admin_auth
def get_participant_summary(p_id):
    conn = get_db_connection()
    participant = conn.execute("SELECT * FROM participants WHERE participant_id = ?", (p_id,)).fetchone()
    if not participant:
        conn.close()
        return jsonify({"error_code": "not_found", "message": "Participant not found"}), 404

    meetings = conn.execute("""
        SELECT m.job_id, m.title, m.uploaded_at 
        FROM meetings m
        JOIN meeting_participants mp ON m.job_id = mp.job_id
        WHERE mp.participant_id = ?
    """, (p_id,)).fetchall()

    conn.close()
    return jsonify({
        "participant": dict(participant),
        "meetings_attended": [dict(m) for m in meetings]
    })

@app.route("/api/ops/summary", methods=["GET"])
@require_admin_auth
def get_ops_summary():
    conn = get_db_connection()
    counts = conn.execute("SELECT state, COUNT(*) as count FROM meetings GROUP BY state").fetchall()
    conn.close()
    return jsonify({"state_counts": {row["state"]: row["count"] for row in counts}})

DASHBOARD_HTML = """
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <title>Meeting Companion Operator Dashboard</title>
    <style>
        body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; margin: 20px; background: #f5f5f7; }
        h1, h2 { color: #1d1d1f; }
        .card { background: white; border-radius: 8px; padding: 16px; margin-bottom: 20px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }
        table { width: 100%; border-collapse: collapse; margin-top: 10px; }
        th, td { text-align: left; padding: 10px; border-bottom: 1px solid #e5e5ea; }
        th { background: #f2f2f7; }
        .badge { padding: 4px 8px; border-radius: 4px; font-weight: bold; font-size: 0.85em; }
        .badge-COMPLETED { background: #34c759; color: white; }
        .badge-QUEUED { background: #ff9500; color: white; }
        .badge-TRANSCRIBING, .badge-EXTRACTING, .badge-TRANSCRIBED { background: #007aff; color: white; }
        .badge-FAILED_ASR, .badge-FAILED_EXTRACTION { background: #ff3b30; color: white; }
        button { background: #007aff; color: white; border: none; padding: 6px 12px; border-radius: 4px; cursor: pointer; margin-right: 4px; }
        button:hover { background: #0056b3; }
        .btn-view { background: #34c759; }
        .btn-view:hover { background: #248a3d; }
        
        /* Modal Styles */
        .modal { display: none; position: fixed; z-index: 1000; left: 0; top: 0; width: 100%; height: 100%; overflow: auto; background-color: rgba(0,0,0,0.5); }
        .modal-content { background-color: #fff; margin: 5% auto; padding: 24px; border-radius: 8px; width: 80%; max-width: 800px; box-shadow: 0 4px 12px rgba(0,0,0,0.15); }
        .close { color: #aaa; float: right; font-size: 28px; font-weight: bold; cursor: pointer; }
        .close:hover { color: #000; }
        .report-section { background: #f9f9f9; padding: 16px; border-radius: 6px; margin-bottom: 20px; white-space: pre-wrap; font-family: inherit; line-height: 1.5; border: 1px solid #e5e5ea; }
    </style>
</head>
<body>
    <h1>Meeting Companion Admin Dashboard</h1>
    
    <div class="card">
        <h2>System Status Summary</h2>
        <div id="ops-summary">Loading operational status...</div>
    </div>

    <div class="card">
        <h2>Meetings & Job Queue</h2>
        <button onclick="loadJobs()">Refresh Jobs</button>
        <table id="jobs-table">
            <thead>
                <tr>
                    <th>Job ID</th>
                    <th>Device</th>
                    <th>State</th>
                    <th>Uploaded At</th>
                    <th>Actions</th>
                </tr>
            </thead>
            <tbody></tbody>
        </table>
    </div>

    <!-- Report Modal -->
    <div id="reportModal" class="modal">
        <div class="modal-content">
            <span class="close" onclick="closeModal()">&times;</span>
            <h2>Meeting Report</h2>
            <p><strong>Job ID:</strong> <span id="modalJobId"></span></p>

            <h3>AI Summary</h3>
            <div id="modalSummary" class="report-section">Loading...</div>
            
            <h3>Raw Transcript</h3>
            <div id="modalTranscript" class="report-section">Loading...</div>
        </div>
    </div>

    <script>
        const AUTH_HEADER = { 'Authorization': 'Bearer dev-admin-token' };

        function formatClientTime(timeStr) {
            if (!timeStr) return '-';
            let normalized = timeStr.trim();
            // If the string lacks a timezone offset (+/- or Z), explicitly treat it as UTC
            if (!normalized.endsWith('Z') && !/[+-]\d{2}:?\d{2}$/.test(normalized)) {
                normalized += 'Z';
            }
            const date = new Date(normalized);
            if (isNaN(date.getTime())) return timeStr;
            return date.toLocaleString(undefined, {
                year: 'numeric',
                month: '2-digit',
                day: '2-digit',
                hour: '2-digit',
                minute: '2-digit',
                second: '2-digit',
                hour12: true
            });
        }

        async function fetchOpsSummary() {
            try {
                const res = await fetch('/api/ops/summary', { headers: AUTH_HEADER });
                const data = await res.json();
                document.getElementById('ops-summary').innerHTML = '<pre>' + JSON.stringify(data, null, 2) + '</pre>';
            } catch(e) {
                document.getElementById('ops-summary').innerText = 'Failed to load ops summary.';
            }
        }

        async function loadJobs() {
            try {
                const res = await fetch('/api/jobs', { headers: AUTH_HEADER });
                const jobs = await res.json();
                const tbody = document.querySelector('#jobs-table tbody');
                tbody.innerHTML = '';
                jobs.forEach(j => {
                    const tr = document.createElement('tr');
                    tr.innerHTML = `
                        <td>${j.job_id.substring(0,8)}...</td>
                        <td>${j.device_id}</td>
                        <td><span class="badge badge-${j.state}">${j.state}</span></td>
                        <td>${formatClientTime(j.uploaded_at)}</td>
                        <td>
                            <button onclick="retryJob('${j.job_id}')">Retry</button>
                            ${j.state === 'COMPLETED' ? `<button class="btn-view" onclick="viewReport('${j.job_id}')">View Report</button>` : ''}
                        </td>
                    `;
                    tbody.appendChild(tr);
                });
            } catch(e) {
                console.error(e);
            }
        }

        async function retryJob(jobId) {
            await fetch(`/api/jobs/${jobId}/retry`, {
                method: 'POST',
                headers: { ...AUTH_HEADER, 'Content-Type': 'application/json' },
                body: JSON.stringify({ stage: 'transcribe' })
            });
            loadJobs();
            fetchOpsSummary();
        }

        async function viewReport(jobId) {
            document.getElementById('reportModal').style.display = 'block';
            document.getElementById('modalJobId').innerText = jobId;
            document.getElementById('modalSummary').innerText = "Loading summary...";
            document.getElementById('modalTranscript').innerText = "Loading transcript...";

            try {
                const res = await fetch(`/api/meetings/${jobId}`, { headers: AUTH_HEADER });
                const data = await res.json();
                
                document.getElementById('modalTranscript').innerText = data.transcript || "No transcript available.";
                
                if (data.extraction && data.extraction.summary) {
                    document.getElementById('modalSummary').innerText = data.extraction.summary;
                } else {
                    document.getElementById('modalSummary').innerText = "No summary generated yet.";
                }
            } catch(e) {
                document.getElementById('modalSummary').innerText = "Error loading report.";
                document.getElementById('modalTranscript').innerText = "Error loading transcript.";
            }
        }

        function closeModal() {
            document.getElementById('reportModal').style.display = 'none';
        }

        window.onclick = function(event) {
            const modal = document.getElementById('reportModal');
            if (event.target == modal) {
                modal.style.display = "none";
            }
        }

        fetchOpsSummary();
        loadJobs();
        // Auto-refresh the dashboard every 10 seconds to watch pipeline progress
        setInterval(() => { loadJobs(); fetchOpsSummary(); }, 10000);
    </script>
</body>
</html>
"""

@app.route("/", methods=["GET"])
@app.route("/dashboard", methods=["GET"])
def dashboard():
    return render_template_string(DASHBOARD_HTML)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Admin Service")
    parser.add_argument("--debug", action="store_true", help="Enable Flask debug mode")
    args = parser.parse_args()

    port = int(os.getenv("ADMIN_PORT", 8080))
    print(f"Starting Admin Service on http://127.0.0.1:{port} (Debug: {args.debug}) ...")
    app.run(host="127.0.0.1", port=port, debug=args.debug)
