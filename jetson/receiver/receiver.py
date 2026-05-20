from flask import Flask, request, jsonify
from pathlib import Path
from datetime import datetime
import os

app = Flask(__name__)

# Folder where incoming JPEGs are saved
INCOMING_DIR = Path("images/incoming")
INCOMING_DIR.mkdir(parents=True, exist_ok=True)

# Optional safety limit: 5 MB max upload
app.config["MAX_CONTENT_LENGTH"] = 5 * 1024 * 1024


@app.route("/", methods=["GET"])
def home():
    return "ESP32-CAM receiver is running", 200


@app.route("/health", methods=["GET"])
def health():
    return jsonify({
        "status": "ok",
        "message": "receiver running"
    }), 200


@app.route("/upload", methods=["POST"])
def upload_image():
    image_data = request.get_data()

    if not image_data:
        return jsonify({
            "status": "error",
            "message": "No image data received"
        }), 400

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")

    temp_path = INCOMING_DIR / f"{timestamp}.tmp"
    final_path = INCOMING_DIR / f"{timestamp}.jpg"

    try:
        # Write to temporary file first
        with open(temp_path, "wb") as f:
            f.write(image_data)

        # Atomic rename so AI code never sees half-written JPEGs
        os.replace(temp_path, final_path)

        print(f"Saved {final_path} ({len(image_data)} bytes)")

        return jsonify({
            "status": "ok",
            "filename": str(final_path),
            "bytes": len(image_data)
        }), 200

    except Exception as e:
        print(f"Failed to save image: {e}")

        if temp_path.exists():
            temp_path.unlink()

        return jsonify({
            "status": "error",
            "message": str(e)
        }), 500


if __name__ == "__main__":
    app.run(
        host="0.0.0.0",
        port=5000,
        debug=False
    )