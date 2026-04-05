from flask import Flask, render_template, request, jsonify
from flask_sqlalchemy import SQLAlchemy
from datetime import datetime

app = Flask(__name__)
app.config['SQLALCHEMY_DATABASE_URI'] = 'sqlite:///health.db'
app.config['SQLALCHEMY_TRACK_MODIFICATIONS'] = False
db = SQLAlchemy(app)

# --- 2 Tables for Relational Integrity ---
class Session(db.Model):
    id = db.Column(db.Integer, primary_key=True)
    timestamp = db.Column(db.DateTime, default=datetime.now)
    avg_bpm = db.Column(db.Integer)
    avg_spo2 = db.Column(db.Integer)
    avg_stress = db.Column(db.Float)
    # Relationship to measurements
    measurements = db.relationship('Measurement', backref='session', lazy=True)

class Measurement(db.Model):
    id = db.Column(db.Integer, primary_key=True)
    session_id = db.Column(db.Integer, db.ForeignKey('session.id'), nullable=False)
    offset_seconds = db.Column(db.Integer) # 0, 1, 2, 3...
    bpm = db.Column(db.Integer)
    spo2 = db.Column(db.Integer)
    stress = db.Column(db.Float)

with app.app_context():
    db.create_all()

@app.route('/')
def dashboard():
    # Get latest sessions
    sessions = Session.query.order_by(Session.timestamp.desc()).limit(20).all()
    return render_template('dashboard.html', sessions=sessions)

@app.route('/api/record', methods=['POST'])
def record():
    data = request.json
    if not data or 'readings' not in data:
        return jsonify({'error': 'no data'}), 400

    # Create Summary
    new_session = Session(
        avg_bpm=data.get('avg_bpm', 0),
        avg_spo2=data.get('avg_spo2', 0),
        avg_stress=data.get('avg_stress', 0)
    )
    db.session.add(new_session)
    db.session.commit() # Commit to get the ID

    # Add Detailed Rows
    for r in data['readings']:
        m = Measurement(
            session_id=new_session.id,
            offset_seconds=r['o'],
            bpm=r['b'],
            spo2=r['s'],
            stress=r['h']
        )
        db.session.add(m)
    
    db.session.commit()
    return jsonify({'status': 'ok'}), 201

# API to fetch graph data for a specific ID
@app.route('/api/session/<int:id>')
def get_session_data(id):
    measurements = Measurement.query.filter_by(session_id=id).order_by(Measurement.offset_seconds).all()
    
    # Format for Chart.js
    response = {
        "labels": [m.offset_seconds for m in measurements],
        "bpm": [m.bpm for m in measurements],
        "spo2": [m.spo2 for m in measurements],
        "stress": [m.stress for m in measurements]
    }
    return jsonify(response)

if __name__ == '__main__':
    app.run(host='0.0.0.0', debug=True)