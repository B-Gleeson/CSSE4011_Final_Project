from BaseHTTPServer import HTTPServer, BaseHTTPRequestHandler
from datetime import datetime
import os
import json

INCOMING_DIR = os.path.join("images", "incoming")
MAX_IMAGE_SIZE = 5 * 1024 * 1024  # 5 MB


def ensure_dir(path):
    if not os.path.isdir(path):
        os.makedirs(path)


class ImageReceiverHandler(BaseHTTPRequestHandler):

    def do_GET(self):
        if self.path == "/" or self.path == "/health":
            response = {
                "status": "ok",
                "message": "ESP32-CAM receiver running"
            }

            body = json.dumps(response) + "\n"

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

        content_length = self.headers.getheader("Content-Length")

        if content_length is None:
            self.send_response(411)
            self.end_headers()
            self.wfile.write("Missing Content-Length")
            return

        try:
            content_length = int(content_length)
        except ValueError:
            self.send_response(400)
            self.end_headers()
            self.wfile.write("Invalid Content-Length")
            return

        if content_length <= 0:
            self.send_response(400)
            self.end_headers()
            self.wfile.write("No image data")
            return

        if content_length > MAX_IMAGE_SIZE:
            self.send_response(413)
            self.end_headers()
            self.wfile.write("Image too large")
            return

        image_data = self.rfile.read(content_length)

        if len(image_data) != content_length:
            self.send_response(400)
            self.end_headers()
            self.wfile.write("Incomplete image received")
            return

        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")

        tmp_path = os.path.join(INCOMING_DIR, timestamp + ".tmp")
        jpg_path = os.path.join(INCOMING_DIR, timestamp + ".jpg")

        try:
            with open(tmp_path, "wb") as f:
                f.write(image_data)

            # Python 2 does not have os.replace().
            # On Linux, os.rename() is atomic if source/destination are on same filesystem.
            os.rename(tmp_path, jpg_path)

            print("Saved {0} ({1} bytes)".format(jpg_path, len(image_data)))

            response = {
                "status": "ok",
                "filename": jpg_path,
                "bytes": len(image_data)
            }

            body = json.dumps(response) + "\n"

            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        except Exception as e:
            print("Error saving image: {0}".format(e))

            if os.path.exists(tmp_path):
                os.remove(tmp_path)

            self.send_response(500)
            self.end_headers()
            self.wfile.write("Failed to save image")


def main():
    host = "0.0.0.0"
    port = 5000

    ensure_dir(INCOMING_DIR)

    server = HTTPServer((host, port), ImageReceiverHandler)

    print("Receiver running on http://{0}:{1}".format(host, port))
    print("Saving images to: {0}".format(INCOMING_DIR))

    server.serve_forever()


if __name__ == "__main__":
    main()