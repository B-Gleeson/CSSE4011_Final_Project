from http.server import HTTPServer, BaseHTTPRequestHandler
from pathlib import Path
from datetime import datetime
import os
import json

INCOMING_DIR = Path("images/incoming")
INCOMING_DIR.mkdir(parents=True, exist_ok=True)

MAX_IMAGE_SIZE = 5 * 1024 * 1024  # 5 MB


class ImageReceiverHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/" or self.path == "/health":
            response = {
                "status": "ok",
                "message": "ESP32-CAM receiver running"
            }

            body = json.dumps(response).encode("utf-8")

            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        self.send_response(404)
        self.end_headers()

    def do_POST(self):
        if self.path != "/upload":
            self.send_response(404)
            self.end_headers()
            return

        content_length = self.headers.get("Content-Length")

        if content_length is None:
            self.send_response(411)
            self.end_headers()
            self.wfile.write(b"Missing Content-Length")
            return

        try:
            content_length = int(content_length)
        except ValueError:
            self.send_response(400)
            self.end_headers()
            self.wfile.write(b"Invalid Content-Length")
            return

        if content_length <= 0:
            self.send_response(400)
            self.end_headers()
            self.wfile.write(b"No image data")
            return

        if content_length > MAX_IMAGE_SIZE:
            self.send_response(413)
            self.end_headers()
            self.wfile.write(b"Image too large")
            return

        image_data = self.rfile.read(content_length)

        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")

        tmp_path = INCOMING_DIR / f"{timestamp}.tmp"
        jpg_path = INCOMING_DIR / f"{timestamp}.jpg"

        try:
            with open(tmp_path, "wb") as f:
                f.write(image_data)

            # Rename only after full file is written
            os.replace(tmp_path, jpg_path)

            print(f"Saved {jpg_path} ({len(image_data)} bytes)")

            response = {
                "status": "ok",
                "filename": str(jpg_path),
                "bytes": len(image_data)
            }

            body = json.dumps(response).encode("utf-8")

            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        except Exception as e:
            print(f"Error saving image: {e}")

            if tmp_path.exists():
                tmp_path.unlink()

            self.send_response(500)
            self.end_headers()
            self.wfile.write(b"Failed to save image")


def main():
    host = "0.0.0.0"
    port = 5000

    server = HTTPServer((host, port), ImageReceiverHandler)

    print(f"Receiver running on http://{host}:{port}")
    print(f"Saving images to: {INCOMING_DIR}")

    server.serve_forever()


if __name__ == "__main__":
    main()