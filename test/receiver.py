from flask import Flask, request
from pathlib import Path
from datetime import datetime

app = Flask(__name__)

SAVE_DIR = Path("received_images")
SAVE_DIR.mkdir(exist_ok=True)

@app.route("/upload", methods=["POST"])
def upload():
    image_data = request.get_data()

    if not image_data:
        return "No image", 400

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")

    filename = SAVE_DIR / f"{timestamp}.jpg"

    with open(filename, "wb") as f:
        f.write(image_data)

    print(f"Saved {filename}")

    return "OK", 200

@app.route("/")
def home():
    return "ESP32-CAM server running"

app.run(host="0.0.0.0", port=5000)
