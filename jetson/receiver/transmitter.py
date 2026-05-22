from BaseHTTPServer import HTTPServer, BaseHTTPRequestHandler
from datetime import datetime

import os
import json

# ============================================================
# Directories
# ============================================================

INCOMING_DIR = os.path.join(
    "images",
    "incoming"
)

OUTPUT_DIR = os.path.join(
    "images",
    "output"
)

MAX_IMAGE_SIZE = 5 * 1024 * 1024

# ============================================================
# Utilities
# ============================================================

def ensure_dir(path):

    if not os.path.isdir(path):
        os.makedirs(path)

# ============================================================
# Latest processed image
# ============================================================

def get_latest_output_image():

    if not os.path.isdir(OUTPUT_DIR):
        return None

    jpg_files = []

    for f in os.listdir(OUTPUT_DIR):

        if f.lower().endswith(".jpg"):
            jpg_files.append(f)

    if not jpg_files:
        return None

    jpg_files.sort()

    latest = jpg_files[-1]

    return os.path.join(
        OUTPUT_DIR,
        latest
    )

# ============================================================
# Latest AI result
# ============================================================

def get_latest_result():

    latest_image = get_latest_output_image()

    if latest_image is None:

        return {
            "frame_id": -1,
            "node_id": "camera_01",
            "ppe_detected": True,
            "missing_items": [],
            "confidence": 0.0,
            "action": "none"
        }

    filename = os.path.basename(latest_image)

    return {
        "frame_id": 1,
        "node_id": "camera_01",
        "ppe_detected": False,
        "missing_items": ["hardhat"],
        "confidence": 0.91,
        "action": "alert",
        "latest_image": filename
    }

# ============================================================
# HTTP Handler
# ============================================================

class ImageReceiverHandler(
    BaseHTTPRequestHandler):

    # ========================================================
    # GET
    # ========================================================

    def do_GET(self):

        # ====================================================
        # Health
        # ====================================================

        if self.path == "/" or \
           self.path == "/health":

            response = {
                "status": "ok",
                "message":
                    "ESP32-CAM receiver running"
            }

            body = json.dumps(
                response
            ) + "\n"

            self.send_response(200)

            self.send_header(
                "Content-Type",
                "application/json"
            )

            self.send_header(
                "Content-Length",
                str(len(body))
            )

            self.end_headers()

            self.wfile.write(body)

            return

        # ====================================================
        # Latest AI result
        # ====================================================

        if self.path == "/latest_result.json":

            response = get_latest_result()

            body = json.dumps(
                response
            ) + "\n"

            self.send_response(200)

            self.send_header(
                "Content-Type",
                "application/json"
            )

            self.send_header(
                "Content-Length",
                str(len(body))
            )

            self.end_headers()

            self.wfile.write(body)

            return

        # ====================================================
        # Latest processed image
        # ====================================================

        if self.path == "/latest_image.jpg":

            latest_image = \
                get_latest_output_image()

            if latest_image is None:

                self.send_response(404)

                self.end_headers()

                self.wfile.write(
                    "No image available"
                )

                return

            try:

                with open(
                    latest_image,
                    "rb"
                ) as f:

                    image_data = f.read()

                self.send_response(200)

                self.send_header(
                    "Content-Type",
                    "image/jpeg"
                )

                self.send_header(
                    "Content-Length",
                    str(len(image_data))
                )

                self.end_headers()

                self.wfile.write(
                    image_data
                )

                print(
                    "Served latest image: {0}".format(
                        latest_image
                    )
                )

                return

            except Exception as e:

                print(
                    "Failed reading image: {0}".format(
                        e
                    )
                )

                self.send_response(500)

                self.end_headers()

                self.wfile.write(
                    "Failed reading image"
                )

                return

        # ====================================================
        # Not found
        # ====================================================

        self.send_response(404)

        self.end_headers()

    # ========================================================
    # POST
    # ========================================================

    def do_POST(self):

        if self.path != "/upload":

            self.send_response(404)

            self.end_headers()

            return

        content_length = \
            self.headers.getheader(
                "Content-Length"
            )

        if content_length is None:

            self.send_response(411)

            self.end_headers()

            self.wfile.write(
                "Missing Content-Length"
            )

            return

        try:
            content_length = int(
                content_length
            )

        except ValueError:

            self.send_response(400)

            self.end_headers()

            self.wfile.write(
                "Invalid Content-Length"
            )

            return

        if content_length <= 0:

            self.send_response(400)

            self.end_headers()

            self.wfile.write(
                "No image data"
            )

            return

        if content_length > MAX_IMAGE_SIZE:

            self.send_response(413)

            self.end_headers()

            self.wfile.write(
                "Image too large"
            )

            return

        image_data = self.rfile.read(
            content_length
        )

        if len(image_data) != content_length:

            self.send_response(400)

            self.end_headers()

            self.wfile.write(
                "Incomplete image received"
            )

            return

        timestamp = datetime.now().strftime(
            "%Y%m%d_%H%M%S_%f"
        )

        tmp_path = os.path.join(
            INCOMING_DIR,
            timestamp + ".tmp"
        )

        jpg_path = os.path.join(
            INCOMING_DIR,
            timestamp + ".jpg"
        )

        try:

            with open(tmp_path, "wb") as f:
                f.write(image_data)

            os.rename(
                tmp_path,
                jpg_path
            )

            print(
                "Saved {0} ({1} bytes)".format(
                    jpg_path,
                    len(image_data)
                )
            )

            response = {
                "status": "ok",
                "filename": jpg_path,
                "bytes": len(image_data)
            }

            body = json.dumps(
                response
            ) + "\n"

            self.send_response(200)

            self.send_header(
                "Content-Type",
                "application/json"
            )

            self.send_header(
                "Content-Length",
                str(len(body))
            )

            self.end_headers()

            self.wfile.write(body)

        except Exception as e:

            print(
                "Error saving image: {0}".format(
                    e
                )
            )

            if os.path.exists(tmp_path):
                os.remove(tmp_path)

            self.send_response(500)

            self.end_headers()

            self.wfile.write(
                "Failed to save image"
            )

# ============================================================
# Main
# ============================================================

def main():

    host = "0.0.0.0"

    port = 5000

    ensure_dir(INCOMING_DIR)

    ensure_dir(OUTPUT_DIR)

    server = HTTPServer(
        (host, port),
        ImageReceiverHandler
    )

    print(
        "Receiver running on "
        "http://{0}:{1}".format(
            host,
            port
        )
    )

    print(
        "Incoming images: {0}".format(
            INCOMING_DIR
        )
    )

    print(
        "Processed images: {0}".format(
            OUTPUT_DIR
        )
    )

    server.serve_forever()

# ============================================================
# Entry
# ============================================================

if __name__ == "__main__":
    main()