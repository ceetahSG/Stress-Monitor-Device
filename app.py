from flask import Flask, render_template, request, jsonify
from flask_sqlalchemy import SQLAlchemy
from flask_socketio import SocketIO, emit
from datetime import datetime
from math import isfinite


# ============================================================
# ======================= APPLICATION ========================
# ============================================================

app = Flask(__name__)

app.config["SQLALCHEMY_DATABASE_URI"] = "sqlite:///health.db"
app.config["SQLALCHEMY_TRACK_MODIFICATIONS"] = False
app.config["SECRET_KEY"] = "stress-monitor-secret-key"

db = SQLAlchemy(app)

socketio = SocketIO(
    app,
    cors_allowed_origins="*"
)


# ============================================================
# ====================== DATABASE MODELS =====================
# ============================================================

class Session(db.Model):
    id = db.Column(db.Integer, primary_key=True)
    timestamp = db.Column(
        db.DateTime,
        default=datetime.now,
        nullable=False
    )
    avg_bpm = db.Column(db.Integer, default=0)
    avg_spo2 = db.Column(db.Integer, default=0)
    avg_stress = db.Column(db.Float, default=0.0)

    measurements = db.relationship(
        "Measurement",
        backref="session",
        lazy=True,
        cascade="all, delete-orphan"
    )


class Measurement(db.Model):
    id = db.Column(db.Integer, primary_key=True)

    session_id = db.Column(
        db.Integer,
        db.ForeignKey("session.id"),
        nullable=False
    )

    offset_seconds = db.Column(db.Integer, default=0)
    bpm = db.Column(db.Integer, default=0)
    spo2 = db.Column(db.Integer, default=0)
    stress = db.Column(db.Float, default=0.0)


# Create database tables if they do not exist.
with app.app_context():
    db.create_all()


# Used only by the old real-time endpoint.
active_sessions = {}


# ============================================================
# ======================== HELPERS ===========================
# ============================================================

def valid_number(value):
    """
    Check whether a value is a finite number.

    Booleans are excluded because bool is a subclass of int in Python.
    """
    return (
        isinstance(value, (int, float))
        and not isinstance(value, bool)
        and isfinite(float(value))
    )


def calculate_session_averages(measurements):
    """
    Calculate averages while ignoring zero values.

    Zero values can represent unavailable sensor values.
    """

    bpm_values = [
        measurement.bpm
        for measurement in measurements
        if measurement.bpm > 0
    ]

    spo2_values = [
        measurement.spo2
        for measurement in measurements
        if measurement.spo2 > 0
    ]

    stress_values = [
        measurement.stress
        for measurement in measurements
        if measurement.stress > 0
    ]

    avg_bpm = (
        int(sum(bpm_values) / len(bpm_values))
        if bpm_values
        else 0
    )

    avg_spo2 = (
        int(sum(spo2_values) / len(spo2_values))
        if spo2_values
        else 0
    )

    avg_stress = (
        sum(stress_values) / len(stress_values)
        if stress_values
        else 0.0
    )

    return avg_bpm, avg_spo2, avg_stress


def session_summary(session):
    """Return a session summary as a JSON-compatible dictionary."""

    return {
        "session_id": session.id,
        "avg_bpm": session.avg_bpm,
        "avg_spo2": session.avg_spo2,
        "avg_stress": session.avg_stress
    }


# ============================================================
# ==================== WEBSOCKET EVENTS =====================
# ============================================================

@socketio.on("connect")
def handle_connect():
    print("🔌 Client connected to live dashboard")
    emit("status", {
        "data": "Connected to server"
    })


@socketio.on("disconnect")
def handle_disconnect():
    print("🔌 Client disconnected from live dashboard")


# ============================================================
# ======================== PAGE ROUTES =======================
# ============================================================

@app.route("/")
def dashboard():
    """Render the historical dashboard."""

    sessions = Session.query.order_by(
        Session.timestamp.desc()
    ).limit(20).all()

    return render_template(
        "dashboard.html",
        sessions=sessions
    )


@app.route("/api/live")
def live_dashboard():
    """Render the live monitoring dashboard."""

    return render_template("live_dashboard.html")


# ============================================================
# ================= BATCH RECORD ENDPOINT ====================
# ============================================================

@app.route("/api/record", methods=["POST"])
def record_batch():
    """
    Receive one complete session from the ESP32.

    Expected payload:

    {
        "readings": [
            {
                "o": 0,
                "b": 72,
                "s": 97,
                "h": 35.2
            }
        ],
        "avg_bpm": 72,
        "avg_spo2": 97,
        "avg_stress": 35.2
    }

    ESP32 field meanings:

    o = offset in seconds
    b = BPM
    s = SpO2
    h = HRV-derived stress value
    """

    try:
        data = request.get_json(silent=True)

        if not isinstance(data, dict):
            return jsonify({
                "error": "Request body must be a JSON object"
            }), 400

        readings = data.get("readings")

        if not isinstance(readings, list):
            return jsonify({
                "error": "readings must be an array"
            }), 400

        if len(readings) == 0:
            return jsonify({
                "error": "readings array is empty"
            }), 400

        valid_readings = []

        for index, item in enumerate(readings):
            if not isinstance(item, dict):
                continue

            offset = item.get("o", index)
            bpm = item.get("b", 0)
            spo2 = item.get("s", 0)
            stress = item.get("h", 0)

            if not all([
                valid_number(offset),
                valid_number(bpm),
                valid_number(spo2),
                valid_number(stress)
            ]):
                continue

            offset = int(offset)
            bpm = int(bpm)
            spo2 = int(spo2)
            stress = float(stress)

            # Validate incoming values.
            if offset < 0:
                continue

            if bpm < 0 or bpm > 250:
                continue

            if spo2 < 0 or spo2 > 100:
                continue

            if stress < 0 or stress > 100:
                continue

            valid_readings.append({
                "offset_seconds": offset,
                "bpm": bpm,
                "spo2": spo2,
                "stress": stress
            })

        if not valid_readings:
            return jsonify({
                "error": "No valid readings found"
            }), 400

        # Create one database session for the complete ESP32 batch.
        session = Session(
            avg_bpm=0,
            avg_spo2=0,
            avg_stress=0.0
        )

        db.session.add(session)
        db.session.flush()

        # Save every reading as a Measurement row.
        for reading in valid_readings:
            measurement = Measurement(
                session_id=session.id,
                offset_seconds=reading["offset_seconds"],
                bpm=reading["bpm"],
                spo2=reading["spo2"],
                stress=reading["stress"]
            )

            db.session.add(measurement)

        db.session.flush()

        # Retrieve the measurements belonging to this new session.
        saved_measurements = Measurement.query.filter_by(
            session_id=session.id
        ).order_by(
            Measurement.offset_seconds.asc()
        ).all()

        # Calculate final averages from stored measurements.
        (
            session.avg_bpm,
            session.avg_spo2,
            session.avg_stress
        ) = calculate_session_averages(saved_measurements)

        db.session.commit()

        # Send every batch reading using the existing event format.
        # This keeps the current live dashboard compatible.
        for measurement in saved_measurements:
            socketio.emit("new_data", {
                "session_id": session.id,
                "measurement_id": measurement.id,
                "offset_seconds": measurement.offset_seconds,
                "bpm": measurement.bpm,
                "spo2": measurement.spo2,
                "stress": measurement.stress
            })

        # Tell the frontend that the completed session is available.
        socketio.emit("session_ended", {
            "session_id": session.id,
            "avg_bpm": session.avg_bpm,
            "avg_spo2": session.avg_spo2,
            "avg_stress": session.avg_stress
        })

        print("=" * 60)
        print(f"✅ BATCH SESSION SAVED: {session.id}")
        print(f"📊 Measurements: {len(saved_measurements)}")
        print(f"❤️ Average BPM: {session.avg_bpm}")
        print(f"🫁 Average SpO2: {session.avg_spo2}")
        print(f"📈 Average HRV score: {session.avg_stress:.2f}")
        print("=" * 60)

        return jsonify({
            "status": "ok",
            "session_id": session.id,
            "measurement_count": len(saved_measurements),
            "avg_bpm": session.avg_bpm,
            "avg_spo2": session.avg_spo2,
            "avg_stress": session.avg_stress
        }), 201

    except Exception as error:
        db.session.rollback()

        print(f"❌ ERROR in /api/record: {error}")

        import traceback
        traceback.print_exc()

        return jsonify({
            "error": str(error)
        }), 500


# ============================================================
# ============== LEGACY REAL-TIME ENDPOINT ===================
# ============================================================

@app.route("/api/real-time-data", methods=["POST"])
def real_time_data():
    """
    Legacy endpoint for older ESP32 firmware.

    Expected payload:

    {
        "session_id": 0,
        "bpm": 72,
        "spo2": 97,
        "stress": 35.2
    }
    """

    try:
        data = request.get_json(silent=True)

        if not isinstance(data, dict):
            return jsonify({
                "error": "Request body must be a JSON object"
            }), 400

        session_id = data.get("session_id")
        bpm = data.get("bpm", 0)
        spo2 = data.get("spo2", 0)
        stress = data.get("stress", 0)

        if not valid_number(bpm):
            return jsonify({
                "error": "bpm must be a number"
            }), 400

        if not valid_number(spo2):
            return jsonify({
                "error": "spo2 must be a number"
            }), 400

        if not valid_number(stress):
            return jsonify({
                "error": "stress must be a number"
            }), 400

        bpm = int(bpm)
        spo2 = int(spo2)
        stress = float(stress)

        if bpm < 0 or bpm > 250:
            return jsonify({
                "error": "bpm must be between 0 and 250"
            }), 400

        if spo2 < 0 or spo2 > 100:
            return jsonify({
                "error": "spo2 must be between 0 and 100"
            }), 400

        if stress < 0 or stress > 100:
            return jsonify({
                "error": "stress must be between 0 and 100"
            }), 400

        print("=" * 60)
        print(
            "📊 RECEIVED REAL-TIME DATA: "
            f"session_id={session_id}, "
            f"bpm={bpm}, "
            f"spo2={spo2}, "
            f"stress={stress:.2f}"
        )
        print("=" * 60)

        # Create a new session when ESP32 sends session_id 0.
        if session_id is None or int(session_id) == 0:
            session = Session(
                avg_bpm=bpm,
                avg_spo2=spo2,
                avg_stress=stress
            )

            db.session.add(session)
            db.session.flush()

            measurement = Measurement(
                session_id=session.id,
                offset_seconds=0,
                bpm=bpm,
                spo2=spo2,
                stress=stress
            )

            db.session.add(measurement)
            db.session.commit()

            session_id = session.id

            active_sessions[session_id] = {
                "bpm_values": [bpm] if bpm > 0 else [],
                "spo2_values": [spo2] if spo2 > 0 else [],
                "stress_values": [stress] if stress > 0 else [],
                "count": 1
            }

            socketio.emit("new_data", {
                "session_id": session_id,
                "measurement_id": measurement.id,
                "offset_seconds": 0,
                "bpm": bpm,
                "spo2": spo2,
                "stress": stress
            })

            print(f"✨ NEW SESSION CREATED: {session_id}")

            return jsonify({
                "status": "ok",
                "session_id": session_id,
                "measurement_id": measurement.id
            }), 201

        # Continue an existing session.
        session = db.session.get(Session, int(session_id))

        if session is None:
            return jsonify({
                "error": "session not found"
            }), 404

        if session.id not in active_sessions:
            existing_measurement_count = Measurement.query.filter_by(
                session_id=session.id
            ).count()

            active_sessions[session.id] = {
                "bpm_values": [],
                "spo2_values": [],
                "stress_values": [],
                "count": existing_measurement_count
            }

        tracking = active_sessions[session.id]

        if bpm > 0:
            tracking["bpm_values"].append(bpm)

        if spo2 > 0:
            tracking["spo2_values"].append(spo2)

        if stress > 0:
            tracking["stress_values"].append(stress)

        tracking["count"] += 1

        measurement = Measurement(
            session_id=session.id,
            offset_seconds=tracking["count"] - 1,
            bpm=bpm,
            spo2=spo2,
            stress=stress
        )

        db.session.add(measurement)

        if tracking["bpm_values"]:
            session.avg_bpm = int(
                sum(tracking["bpm_values"]) /
                len(tracking["bpm_values"])
            )

        if tracking["spo2_values"]:
            session.avg_spo2 = int(
                sum(tracking["spo2_values"]) /
                len(tracking["spo2_values"])
            )

        if tracking["stress_values"]:
            session.avg_stress = (
                sum(tracking["stress_values"]) /
                len(tracking["stress_values"])
            )

        db.session.commit()

        socketio.emit("new_data", {
            "session_id": session.id,
            "measurement_id": measurement.id,
            "offset_seconds": measurement.offset_seconds,
            "bpm": measurement.bpm,
            "spo2": measurement.spo2,
            "stress": measurement.stress
        })

        print(
            f"✅ REAL-TIME MEASUREMENT SAVED: "
            f"session={session.id}, "
            f"measurement={measurement.id}"
        )

        return jsonify({
            "status": "ok",
            "session_id": session.id,
            "measurement_id": measurement.id
        }), 201

    except Exception as error:
        db.session.rollback()

        print(f"❌ ERROR in /api/real-time-data: {error}")

        import traceback
        traceback.print_exc()

        return jsonify({
            "error": str(error)
        }), 500


# ============================================================
# =================== END SESSION ENDPOINT ===================
# ============================================================

@app.route("/api/end-session", methods=["POST"])
def end_session():
    """Finalize an existing session and calculate its averages."""

    try:
        data = request.get_json(silent=True)

        if not isinstance(data, dict):
            return jsonify({
                "error": "Request body must be a JSON object"
            }), 400

        session_id = data.get("session_id")

        if session_id is None:
            return jsonify({
                "error": "session_id is required"
            }), 400

        session = db.session.get(Session, int(session_id))

        if session is None:
            return jsonify({
                "error": "session not found"
            }), 404

        measurements = Measurement.query.filter_by(
            session_id=session.id
        ).order_by(
            Measurement.offset_seconds.asc()
        ).all()

        (
            session.avg_bpm,
            session.avg_spo2,
            session.avg_stress
        ) = calculate_session_averages(measurements)

        db.session.commit()

        active_sessions.pop(session.id, None)

        socketio.emit("session_ended", {
            "session_id": session.id,
            "avg_bpm": session.avg_bpm,
            "avg_spo2": session.avg_spo2,
            "avg_stress": session.avg_stress
        })

        print(f"🛑 SESSION ENDED: {session.id}")
        print(f"   Average BPM: {session.avg_bpm}")
        print(f"   Average SpO2: {session.avg_spo2}")
        print(f"   Average HRV score: {session.avg_stress:.2f}")

        return jsonify({
            "status": "ok",
            "session_id": session.id,
            "avg_bpm": session.avg_bpm,
            "avg_spo2": session.avg_spo2,
            "avg_stress": session.avg_stress
        }), 200

    except Exception as error:
        db.session.rollback()

        print(f"❌ ERROR in /api/end-session: {error}")

        import traceback
        traceback.print_exc()

        return jsonify({
            "error": str(error)
        }), 500


# ============================================================
# ================= HISTORICAL DATA APIs =====================
# ============================================================

@app.route("/api/session/<int:session_id>", methods=["GET"])
def get_session_data(session_id):
    """Return all readings and summary data for one session."""

    session = db.session.get(Session, session_id)

    if session is None:
        return jsonify({
            "error": "session not found"
        }), 404

    measurements = Measurement.query.filter_by(
        session_id=session.id
    ).order_by(
        Measurement.offset_seconds.asc()
    ).all()

    return jsonify({
        "session_id": session.id,
        "timestamp": session.timestamp.isoformat(),
        "avg_bpm": session.avg_bpm,
        "avg_spo2": session.avg_spo2,
        "avg_stress": session.avg_stress,
        "measurement_count": len(measurements),
        "labels": [
            measurement.offset_seconds
            for measurement in measurements
        ],
        "bpm": [
            measurement.bpm
            for measurement in measurements
        ],
        "spo2": [
            measurement.spo2
            for measurement in measurements
        ],
        "stress": [
            measurement.stress
            for measurement in measurements
        ]
    })


@app.route("/api/sessions", methods=["GET"])
def get_all_sessions():
    """Return all saved sessions, newest first."""

    sessions = Session.query.order_by(
        Session.timestamp.desc()
    ).all()

    return jsonify([
        {
            "id": session.id,
            "timestamp": session.timestamp.isoformat(),
            "avg_bpm": session.avg_bpm,
            "avg_spo2": session.avg_spo2,
            "avg_stress": session.avg_stress
        }
        for session in sessions
    ])


# ============================================================
# ========================= STARTUP ==========================
# ============================================================

if __name__ == "__main__":
    print("\n" + "=" * 60)
    print("🚀 STRESS MONITOR SERVER STARTING")
    print("=" * 60)
    print("📡 Listening on http://0.0.0.0:5000")
    print("🌐 Dashboard: http://<SERVER-IP>:5000/")
    print("📊 Live dashboard: http://<SERVER-IP>:5000/api/live")
    print("📥 Batch endpoint: POST /api/record")
    print("🔌 WebSocket: Ready for real-time updates")
    print("=" * 60 + "\n")

    socketio.run(
        app,
        host="0.0.0.0",
        port=5000,
        debug=False,
        allow_unsafe_werkzeug=True
    )