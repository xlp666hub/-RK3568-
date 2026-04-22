from flask import Flask, jsonify, render_template
import json
import os

app = Flask(__name__)

STATUS_FILE = "/tmp/alarm_status.json"

@app.route("/")
def index():
    return render_template("index.html")

@app.route("/api/status")
def api_status():
    if not os.path.exists(STATUS_FILE):
        return jsonify({
            "state": "UNKNOWN",
            "lux": 0,
            "dark": 0,
            "motion": 0,
            "motion_hits": 0,
            "accel_delta": 0,
            "gyro_delta": 0,
            "temp": 0.0,
            "timestamp": 0
        })

    try:
        with open(STATUS_FILE, "r", encoding="utf-8") as f:
            data = json.load(f)
        return jsonify(data)
    except Exception as e:
        return jsonify({
            "state": "ERROR",
            "error": str(e)
        }), 500

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000, debug=False)
