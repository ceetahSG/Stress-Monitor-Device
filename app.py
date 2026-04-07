from flask import Flask, render_template, request, jsonify
from flask_sqlalchemy import SQLAlchemy
from flask_socketio import SocketIO, emit, send
from datetime import datetime
import os

app = Flask(__name__)
app.config['SQLALCHEMY_DATABASE_URI'] = 'sqlite:///health.db'
app.config['SQLALCHEMY_TRACK_MODIFICATIONS'] = False
app.config['SECRET_KEY'] = 'stress-monitor-secret-key'

db = SQLAlchemy(app)
socketio = SocketIO(app, cors_allowed_origins="*")

# --- Database Models ---
class Session(db.Model):
    id = db.Column(db.Integer, primary_key=True)
    timestamp = db.Column(db.DateTime, default=datetime.now)
    avg_bpm = db.Column(db.Integer, default=0)
    avg_spo2 = db.Column(db.Integer, default=0)
    avg_stress = db.Column(db.Float, default=0.0)
    measurements = db.relationship('Measurement', backref='session', lazy=True, cascade='all, delete-orphan')

class Measurement(db.Model):
    id = db.Column(db.Integer, primary_key=True)
    session_id = db.Column(db.Integer, db.ForeignKey('session.id'), nullable=False)
    offset_seconds = db.Column(db.Integer, default=0)
    bpm = db.Column(db.Integer, default=0)
    spo2 = db.Column(db.Integer, default=0)
    stress = db.Column(db.Float, default=0.0)

# Initialize database
with app.app_context():
    db.create_all()

# Global session tracking
active_sessions = {}

# ============================================================
# ==================== WEBSOCKET EVENTS =====================
# ============================================================

@socketio.on('connect')
def handle_connect():
    print("🔌 Client connected to live dashboard")
    emit('status', {'data': 'Connected to server'})

@socketio.on('disconnect')
def handle_disconnect():
    print("🔌 Client disconnected from live dashboard")

# ============================================================
# ======================== ROUTES ==========================
# ============================================================

@app.route('/')
def dashboard():
    """Main dashboard - historical sessions"""
    sessions = Session.query.order_by(Session.timestamp.desc()).limit(20).all()
    return render_template('dashboard.html', sessions=sessions)

@app.route('/api/live')
def live_dashboard():
    """Live monitoring dashboard"""
    return render_template('live_dashboard.html')

# ============================================================
# =============== REAL-TIME DATA ENDPOINTS ==================
# ============================================================

@app.route('/api/real-time-data', methods=['POST'])
def real_time_data():
    """Receive real-time sensor data from ESP32"""
    try:
        data = request.json
        session_id = data.get('session_id')
        bpm = data.get('bpm', 0)
        spo2 = data.get('spo2', 0)
        stress = data.get('stress', 0)
        
        print(f"{'='*60}")
        print(f"📊 RECEIVED DATA: session_id={session_id}, bpm={bpm}, spo2={spo2}, stress={stress:.2f}")
        print(f"{'='*60}")
        
        # NEW SESSION: Create if not exists
        if session_id is None or session_id == 0:
            new_session = Session(avg_bpm=bpm, avg_spo2=spo2, avg_stress=stress)
            db.session.add(new_session)
            db.session.commit()
            
            session_id = new_session.id
            active_sessions[session_id] = {
                'bpm_values': [bpm],
                'spo2_values': [spo2],
                'stress_values': [stress],
                'count': 1
            }
            
            print(f"✨ NEW SESSION - ID: {session_id}")
            print(f"✅ SAVED - Measurement ID: 1")
            
            # Broadcast to all clients (FIXED SYNTAX)
            socketio.emit('new_data', {
                'bpm': bpm,
                'spo2': spo2,
                'stress': stress,
                'session_id': session_id,
                'measurement_id': 1
            }, to=None)
            
            return jsonify({
                'status': 'ok',
                'session_id': session_id,
                'measurement_id': 1
            }), 201
        
        # EXISTING SESSION: Update with new measurement
        else:
            session = Session.query.get(session_id)
            if not session:
                return jsonify({'error': 'session not found'}), 404
            
            # Initialize tracking if needed
            if session_id not in active_sessions:
                active_sessions[session_id] = {
                    'bpm_values': [],
                    'spo2_values': [],
                    'stress_values': [],
                    'count': 0
                }
            
            # Add new values
            active_sessions[session_id]['bpm_values'].append(bpm)
            active_sessions[session_id]['spo2_values'].append(spo2)
            active_sessions[session_id]['stress_values'].append(stress)
            active_sessions[session_id]['count'] += 1
            
            # Update session averages
            session.avg_bpm = int(sum(active_sessions[session_id]['bpm_values']) / len(active_sessions[session_id]['bpm_values']))
            session.avg_spo2 = int(sum(active_sessions[session_id]['spo2_values']) / len(active_sessions[session_id]['spo2_values']))
            session.avg_stress = sum(active_sessions[session_id]['stress_values']) / len(active_sessions[session_id]['stress_values'])
            
            # Create measurement record
            measurement = Measurement(
                session_id=session_id,
                offset_seconds=active_sessions[session_id]['count'] - 1,
                bpm=bpm,
                spo2=spo2,
                stress=stress
            )
            db.session.add(measurement)
            db.session.commit()
            
            print(f"✅ SAVED - Session ID: {session_id}, Measurement ID: {measurement.id}")
            
            # Broadcast to all clients (FIXED SYNTAX)
            socketio.emit('new_data', {
                'bpm': bpm,
                'spo2': spo2,
                'stress': stress,
                'session_id': session_id,
                'measurement_id': measurement.id
            }, to=None)
            
            return jsonify({
                'status': 'ok',
                'session_id': session_id,
                'measurement_id': measurement.id
            }), 201
    
    except Exception as e:
        print(f"❌ ERROR in real_time_data: {str(e)}")
        import traceback
        traceback.print_exc()
        return jsonify({'error': str(e)}), 500

@app.route('/api/end-session', methods=['POST'])
def end_session():
    """End active session"""
    try:
        data = request.json
        session_id = data.get('session_id')
        
        session = Session.query.get(session_id)
        if not session:
            return jsonify({'error': 'session not found'}), 404
        
        # Calculate final averages from all measurements
        measurements = Measurement.query.filter_by(session_id=session_id).all()
        
        if measurements:
            bpm_vals = [m.bpm for m in measurements if m.bpm > 0]
            spo2_vals = [m.spo2 for m in measurements if m.spo2 > 0]
            stress_vals = [m.stress for m in measurements if m.stress > 0]
            
            session.avg_bpm = int(sum(bpm_vals) / len(bpm_vals)) if bpm_vals else 0
            session.avg_spo2 = int(sum(spo2_vals) / len(spo2_vals)) if spo2_vals else 0
            session.avg_stress = sum(stress_vals) / len(stress_vals) if stress_vals else 0
            
            db.session.commit()
        
        # Clean up
        if session_id in active_sessions:
            del active_sessions[session_id]
        
        print(f"🛑 SESSION ENDED - ID: {session_id}")
        print(f"   Avg BPM: {session.avg_bpm}")
        print(f"   Avg SpO2: {session.avg_spo2}")
        print(f"   Avg Stress: {session.avg_stress:.2f}")
        
        # Notify clients (FIXED SYNTAX)
        socketio.emit('session_ended', {
            'session_id': session_id,
            'avg_bpm': session.avg_bpm,
            'avg_spo2': session.avg_spo2,
            'avg_stress': session.avg_stress
        }, to=None)
        
        return jsonify({
            'status': 'ok',
            'avg_bpm': session.avg_bpm,
            'avg_spo2': session.avg_spo2,
            'avg_stress': session.avg_stress
        }), 200
    
    except Exception as e:
        print(f"❌ ERROR in end_session: {str(e)}")
        return jsonify({'error': str(e)}), 500

# ============================================================
# ================ HISTORICAL DATA ENDPOINTS ================
# ============================================================

@app.route('/api/session/<int:id>')
def get_session_data(id):
    """Fetch measurements for a session"""
    measurements = Measurement.query.filter_by(session_id=id).order_by(Measurement.offset_seconds).all()
    
    return jsonify({
        "labels": [m.offset_seconds for m in measurements],
        "bpm": [m.bpm for m in measurements],
        "spo2": [m.spo2 for m in measurements],
        "stress": [m.stress for m in measurements]
    })

@app.route('/api/sessions')
def get_all_sessions():
    """Fetch all sessions"""
    sessions = Session.query.order_by(Session.timestamp.desc()).all()
    return jsonify([{
        'id': s.id,
        'timestamp': s.timestamp.isoformat(),
        'avg_bpm': s.avg_bpm,
        'avg_spo2': s.avg_spo2,
        'avg_stress': s.avg_stress
    } for s in sessions])

# ============================================================
# ========================= STARTUP ==========================
# ============================================================

if __name__ == '__main__':
    print("\n" + "="*60)
    print("🚀 STRESS MONITOR SERVER STARTING")
    print("="*60)
    print("📡 Listening on http://0.0.0.0:5000")
    print("🌐 Dashboard: http://192.168.137.1:5000")
    print("📊 Live: http://192.168.137.1:5000/api/live")
    print("🔌 WebSocket: Ready for real-time updates")
    print("="*60 + "\n")
    
    socketio.run(app, host='0.0.0.0', port=5000, debug=False, allow_unsafe_werkzeug=True)