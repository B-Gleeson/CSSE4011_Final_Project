import json
import threading
import time
import urllib.request
from collections import deque
from datetime import datetime

import serial
from flask import Flask, Response, jsonify, render_template_string, request

# ============================================================
# Configuration
# ============================================================

SERIAL_PORT = "/dev/ttyACM1"
BAUD_RATE   = 115200
MAX_EVENTS  = 50

JETSON_IP   = "192.168.1.54"
JETSON_PORT = 5000

# ============================================================
# App + shared state
# ============================================================

app = Flask(__name__)

latest_state = {
    "connected":        False,
    "last_seen":        None,
    "latest_result":    None,
    "latest_humidity":  None,
    "latest_image_url": None,
    "nodes":            {},
}

event_log  = deque(maxlen=MAX_EVENTS)
lock       = threading.Lock()
ser_handle = None          # module-level so the command route can write to it


def now_string():
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S")


# ============================================================
# Packet handler
# ============================================================

def handle_json_packet(packet):
    packet_type = packet.get("type", "unknown")
    node_id     = packet.get("node_id", "unknown")
    received_at = now_string()

    # The NUS humidity-node response does not necessarily include node_id.
    # Use the routed logical name when the packet type identifies that node.
    if packet_type in ("humidity_data", "humidity_error", "alarm_ack") and node_id == "unknown":
        node_id = "alarm_node"

    with lock:
        latest_state["connected"] = True
        latest_state["last_seen"] = received_at

        if packet_type == "ai_result":
            latest_state["latest_result"] = packet

            event_log.appendleft({
                "time":   received_at,
                "node":   node_id,
                "event":  "PPE detection",
                "result": "Compliant" if packet.get("ppe_detected") else "Non-compliant",
                "raw":    packet,
            })

        elif packet_type == "humidity_data":
            humidity_x100 = packet.get("humidity_x100")
            temperature_x100 = packet.get("temperature_c_x100")

            humidity_percent = (
                humidity_x100 / 100.0 if isinstance(humidity_x100, (int, float))
                else packet.get("humidity")
            )
            temperature_c = (
                temperature_x100 / 100.0 if isinstance(temperature_x100, (int, float))
                else packet.get("temperature_c")
            )

            reading = {
                **packet,
                "node_id": node_id,
                "humidity_percent": humidity_percent,
                "temperature_c": temperature_c,
                "last_seen": received_at,
            }
            latest_state["latest_humidity"] = reading

            existing_node = latest_state["nodes"].get(node_id, {})
            latest_state["nodes"][node_id] = {
                **existing_node,
                "node_id": node_id,
                "status": "humidity_received",
                "humidity_percent": humidity_percent,
                "temperature_c": temperature_c,
                "alarm": packet.get("alarm", existing_node.get("alarm", False)),
                "last_seen": received_at,
            }

            humidity_text = (
                f"{humidity_percent:.2f} %RH"
                if isinstance(humidity_percent, (int, float))
                else "unknown humidity"
            )
            temperature_text = (
                f"{temperature_c:.2f} C"
                if isinstance(temperature_c, (int, float))
                else "unknown temperature"
            )
            event_log.appendleft({
                "time":   received_at,
                "node":   node_id,
                "event":  "Humidity reading",
                "result": f"{humidity_text}, {temperature_text}",
                "raw":    packet,
            })

        elif packet_type == "humidity_error":
            event_log.appendleft({
                "time":   received_at,
                "node":   node_id,
                "event":  "Humidity sensor error",
                "result": f"Read failed: {packet.get('err', 'unknown')}",
                "raw":    packet,
            })

        elif packet_type == "alarm_ack":
            existing_node = latest_state["nodes"].get(node_id, {})
            alarm_on = bool(packet.get("alarm", False))
            latest_state["nodes"][node_id] = {
                **existing_node,
                "node_id": node_id,
                "status": "alarm_on" if alarm_on else "alarm_off",
                "alarm": alarm_on,
                "last_seen": received_at,
            }

            event_log.appendleft({
                "time":   received_at,
                "node":   node_id,
                "event":  "Alarm acknowledgement",
                "result": "ON" if alarm_on else "OFF",
                "raw":    packet,
            })

        elif packet_type == "status":
            latest_state["nodes"][node_id] = {
                **latest_state["nodes"].get(node_id, {}),
                **packet,
                "last_seen": received_at,
            }

            event_log.appendleft({
                "time":   received_at,
                "node":   node_id,
                "event":  "Status update",
                "result": packet.get("status", "unknown"),
                "raw":    packet,
            })

        elif packet_type == "alert":
            event_log.appendleft({
                "time":   received_at,
                "node":   node_id,
                "event":  "Alert",
                "result": packet.get("message", "Alert triggered"),
                "raw":    packet,
            })

        elif packet_type == "processed_image":
            latest_state["latest_image_url"] = packet.get("image_url")

            event_log.appendleft({
                "time":   received_at,
                "node":   node_id,
                "event":  "Image available",
                "result": f"frame {packet.get('frame_id')}",
                "raw":    packet,
            })

        else:
            event_log.appendleft({
                "time":   received_at,
                "node":   node_id,
                "event":  "Unknown packet",
                "result": packet_type,
                "raw":    packet,
            })


# ============================================================
# Serial reader thread
# ============================================================

def serial_reader():
    global ser_handle

    while True:
        try:
            print(f"Opening serial port {SERIAL_PORT}...")
            ser_handle = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
            print("Serial connected.")

            while True:
                line = ser_handle.readline().decode("utf-8", errors="ignore").strip()

                if not line:
                    continue

                try:
                    packet = json.loads(line)
                    handle_json_packet(packet)
                    print("[JSON]", packet)

                except json.JSONDecodeError:
                    print("[IGNORED NON-JSON]", line)

        except serial.SerialException as e:
            print("Serial error:", e)
            ser_handle = None

            with lock:
                latest_state["connected"] = False

            time.sleep(2)


# ============================================================
# Routes
# ============================================================

@app.route("/")
def index():
    return render_template_string(DASHBOARD_HTML)


@app.route("/api/state")
def api_state():
    with lock:
        return jsonify({
            "state":  latest_state,
            "events": list(event_log),
        })


@app.route("/api/command", methods=["POST"])
def api_command():
    cmd = request.get_json(force=True)
    if not cmd:
        return jsonify({"ok": False, "error": "no JSON body"}), 400

    line = json.dumps(cmd) + "\n"

    with lock:
        handle = ser_handle

    if handle and handle.is_open:
        try:
            handle.write(line.encode("utf-8"))
            print("[CMD SENT]", cmd)
            return jsonify({"ok": True})
        except serial.SerialException as e:
            return jsonify({"ok": False, "error": str(e)}), 503

    return jsonify({"ok": False, "error": "serial not open"}), 503


@app.route("/proxy/latest_image")
def proxy_image():
    """
    Fetches the latest processed image from the Jetson and relays it to the
    browser.  This means the browser never needs direct access to the Jetson IP.
    """
    url = f"http://{JETSON_IP}:{JETSON_PORT}/latest_image.jpg"
    try:
        with urllib.request.urlopen(url, timeout=3) as r:
            return Response(r.read(), mimetype="image/jpeg")
    except Exception as e:
        print(f"[IMAGE PROXY ERROR] {e}")
        return "", 503


# ============================================================
# Dashboard HTML
# ============================================================

DASHBOARD_HTML = """
<!DOCTYPE html>
<html>
<head>
    <title>Blue-Titan Safety Dashboard</title>
    <style>
        body {
            font-family: Arial, sans-serif;
            background: #111827;
            color: #f9fafb;
            margin: 0;
            padding: 24px;
        }

        h1 { margin-bottom: 8px; }

        .grid {
            display: grid;
            grid-template-columns: repeat(2, 1fr);
            gap: 16px;
            margin-top: 20px;
        }

        .card {
            background: #1f2937;
            border-radius: 12px;
            padding: 18px;
            box-shadow: 0 0 12px rgba(0,0,0,0.3);
        }

        .status-ok   { color: #22c55e; font-weight: bold; }
        .status-bad  { color: #ef4444; font-weight: bold; }
        .status-warn { color: #facc15; font-weight: bold; }

        table {
            width: 100%;
            border-collapse: collapse;
            margin-top: 8px;
        }

        th, td {
            text-align: left;
            padding: 8px;
            border-bottom: 1px solid #374151;
        }

        pre {
            background: #030712;
            padding: 12px;
            border-radius: 8px;
            overflow-x: auto;
            font-size: 12px;
        }

        button {
            background: #374151;
            color: #f9fafb;
            border: none;
            border-radius: 8px;
            padding: 10px 16px;
            margin: 4px;
            cursor: pointer;
            font-size: 14px;
            transition: background 0.2s;
        }

        button:hover { background: #4b5563; }

        button.danger       { background: #7f1d1d; }
        button.danger:hover { background: #991b1b; }

        #latest-image {
            width: 100%;
            border-radius: 8px;
            display: none;
            margin-top: 8px;
        }

        #no-image { color: #6b7280; }

        #cmd-status {
            margin-top: 8px;
            font-size: 13px;
            color: #6b7280;
        }
    </style>
</head>

<body>
    <h1>Blue-Titan PPE and Environmental Safety Dashboard</h1>
    <div id="connection">Connection: loading...</div>

    <div class="grid">

        <!-- Live PPE status -->
        <div class="card">
            <h2>Live PPE Status</h2>
            <div id="ppe-status">No result yet</div>
        </div>

        <!-- Humidity alarm node -->
        <div class="card">
            <h2>Humidity Sensor</h2>
            <div id="humidity-status">No humidity reading yet</div>
            <pre id="humidity-json">No humidity JSON received</pre>
        </div>

        <!-- Latest raw JSON -->
        <div class="card">
            <h2>Latest Detection</h2>
            <pre id="latest-json">No JSON received</pre>
        </div>

        <!-- Processed image -->
        <div class="card">
            <h2>Latest Processed Image</h2>
            <p id="no-image">No image received yet</p>
            <img id="latest-image" src="" alt="Processed frame" />
        </div>

        <!-- Controls -->
        <div class="card">
            <h2>Controls</h2>
            <button onclick="sendCommand('camera','take_photo')">📷 Take Photo</button>
            <button onclick="sendCommand('alarm_node','read_humidity')">💧 Read Humidity</button>
            <button onclick="sendCommand('alarm_node','alarm_on')" class="danger">🔔 Alarm ON</button>
            <button onclick="sendCommand('alarm_node','alarm_off')">🔕 Alarm OFF</button>
            <button onclick="sendCommand('base_node','status')">📡 Node Status</button>
            <div id="cmd-status"></div>
        </div>

        <!-- Node health -->
        <div class="card">
            <h2>Node Health</h2>
            <table>
                <thead>
                    <tr>
                        <th>Node</th><th>Status</th><th>WiFi</th><th>NUS</th><th>Humidity</th><th>Temp</th><th>Alarm</th><th>Last Seen</th>
                    </tr>
                </thead>
                <tbody id="node-table"></tbody>
            </table>
        </div>

        <!-- Event log -->
        <div class="card">
            <h2>Event Log</h2>
            <table>
                <thead>
                    <tr>
                        <th>Time</th><th>Node</th><th>Event</th><th>Result</th>
                    </tr>
                </thead>
                <tbody id="event-table"></tbody>
            </table>
        </div>

    </div>

<script>
async function sendCommand(target, command) {
    const statusEl = document.getElementById("cmd-status");
    statusEl.textContent = `Sending: ${command} → ${target}...`;

    try {
        const resp = await fetch("/api/command", {
            method: "POST",
            headers: {"Content-Type": "application/json"},
            body: JSON.stringify({type: "command", target: target, command: command})
        });
        const data = await resp.json();
        statusEl.textContent = data.ok
            ? `✓ Sent: ${command} → ${target}`
            : `✗ Failed: ${data.error}`;
    } catch (e) {
        statusEl.textContent = `✗ Error: ${e}`;
    }
}

async function updateDashboard() {
    let data;
    try {
        const response = await fetch("/api/state");
        data = await response.json();
    } catch (e) {
        return;
    }

    const state  = data.state;
    const events = data.events;

    // Connection banner
    const connection = document.getElementById("connection");
    connection.innerHTML = state.connected
        ? `<span class="status-ok">Serial connected</span> | Last seen: ${state.last_seen}`
        : `<span class="status-bad">Serial disconnected</span>`;

    // PPE status + raw JSON
    const ppeStatus = document.getElementById("ppe-status");
    const latestJson = document.getElementById("latest-json");

    if (state.latest_result) {
        const r = state.latest_result;
        if (r.ppe_detected) {
            ppeStatus.innerHTML = `
                <p class="status-ok">PPE compliant</p>
                <p>Node: ${r.node_id}</p>
                <p>Confidence: ${(r.confidence * 100).toFixed(1)}%</p>`;
        } else {
            ppeStatus.innerHTML = `
                <p class="status-bad">PPE non-compliant</p>
                <p>Node: ${r.node_id}</p>
                <p>Missing: ${(r.missing_items || []).join(", ")}</p>
                <p>Confidence: ${(r.confidence * 100).toFixed(1)}%</p>
                <p class="status-warn">Action: ${r.action || "alert"}</p>`;
        }
        latestJson.textContent = JSON.stringify(r, null, 2);
    }

    // Humidity/temperature alarm node
    const humidityStatus = document.getElementById("humidity-status");
    const humidityJson = document.getElementById("humidity-json");

    if (state.latest_humidity) {
        const h = state.latest_humidity;
        const humidity = (typeof h.humidity_percent === "number")
            ? `${h.humidity_percent.toFixed(2)} %RH`
            : "Unavailable";
        const temperature = (typeof h.temperature_c === "number")
            ? `${h.temperature_c.toFixed(2)} °C`
            : "Unavailable";
        const alarmText = h.alarm
            ? '<span class="status-warn">ON</span>'
            : '<span class="status-ok">OFF</span>';

        humidityStatus.innerHTML = `
            <p><strong>Humidity:</strong> ${humidity}</p>
            <p><strong>Temperature:</strong> ${temperature}</p>
            <p><strong>Alarm:</strong> ${alarmText}</p>
            <p><strong>Last reading:</strong> ${h.last_seen || "-"}</p>`;
        humidityJson.textContent = JSON.stringify(h, null, 2);
    }

    // Processed image — load via proxy so the browser doesn't
    // need direct access to the Jetson IP
    const img   = document.getElementById("latest-image");
    const noImg = document.getElementById("no-image");

    if (state.latest_image_url) {
        img.src         = "/proxy/latest_image?t=" + Date.now();
        img.style.display = "block";
        noImg.style.display = "none";
    }

    // Node health table
    const nodeTable = document.getElementById("node-table");
    nodeTable.innerHTML = "";
    for (const [nodeId, node] of Object.entries(state.nodes)) {
        const wifi = (typeof node.wifi === "boolean")
            ? (node.wifi ? '<span class="status-ok">✓</span>' : '<span class="status-bad">✗</span>')
            : '-';
        const nus = (typeof node.nus === "boolean")
            ? (node.nus ? '<span class="status-ok">✓</span>' : '<span class="status-bad">✗</span>')
            : '-';
        const humidity = (typeof node.humidity_percent === "number")
            ? `${node.humidity_percent.toFixed(2)} %RH`
            : '-';
        const temperature = (typeof node.temperature_c === "number")
            ? `${node.temperature_c.toFixed(2)} °C`
            : '-';
        const alarm = node.alarm ? '<span class="status-warn">ON</span>' : 'OFF';
        nodeTable.innerHTML += `
            <tr>
                <td>${nodeId}</td>
                <td>${node.status || "unknown"}</td>
                <td>${wifi}</td>
                <td>${nus}</td>
                <td>${humidity}</td>
                <td>${temperature}</td>
                <td>${alarm}</td>
                <td>${node.last_seen || "-"}</td>
            </tr>`;
    }

    // Event log
    const eventTable = document.getElementById("event-table");
    eventTable.innerHTML = "";
    for (const event of events) {
        eventTable.innerHTML += `
            <tr>
                <td>${event.time}</td>
                <td>${event.node}</td>
                <td>${event.event}</td>
                <td>${event.result}</td>
            </tr>`;
    }
}

setInterval(updateDashboard, 1000);
updateDashboard();
</script>
</body>
</html>
"""


# ============================================================
# Entry point
# ============================================================

if __name__ == "__main__":
    thread = threading.Thread(target=serial_reader, daemon=True)
    thread.start()

    app.run(host="0.0.0.0", port=8080, debug=True, use_reloader=False)
