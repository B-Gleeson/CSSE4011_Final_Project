import json
import threading
import time
from collections import deque
from datetime import datetime

import serial
from flask import Flask, jsonify, render_template_string

SERIAL_PORT = "/dev/ttyACM0"   # Windows example: "COM5"
BAUD_RATE = 115200
MAX_EVENTS = 50

app = Flask(__name__)

latest_state = {
    "connected": False,
    "last_seen": None,
    "latest_result": None,
    "nodes": {},
}

event_log = deque(maxlen=MAX_EVENTS)
lock = threading.Lock()


def now_string():
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def handle_json_packet(packet):
    """
    Expected packets can be like:

    {
      "type": "ai_result",
      "node_id": "camera_01",
      "ppe_detected": false,
      "missing_items": ["hardhat"],
      "confidence": 0.91,
      "action": "alert"
    }

    or:

    {
      "type": "status",
      "node_id": "base_node",
      "status": "online",
      "wifi_rssi": -55
    }
    """

    packet_type = packet.get("type", "unknown")
    node_id = packet.get("node_id", "unknown")

    with lock:
        latest_state["connected"] = True
        latest_state["last_seen"] = now_string()

        if packet_type == "ai_result":
            latest_state["latest_result"] = packet

            event_log.appendleft({
                "time": now_string(),
                "node": node_id,
                "event": "PPE detection",
                "result": "Compliant" if packet.get("ppe_detected") else "Non-compliant",
                "raw": packet,
            })

        elif packet_type == "status":
            latest_state["nodes"][node_id] = {
                **packet,
                "last_seen": now_string(),
            }

            event_log.appendleft({
                "time": now_string(),
                "node": node_id,
                "event": "Status update",
                "result": packet.get("status", "unknown"),
                "raw": packet,
            })

        elif packet_type == "alert":
            event_log.appendleft({
                "time": now_string(),
                "node": node_id,
                "event": "Alert",
                "result": packet.get("message", "Alert triggered"),
                "raw": packet,
            })

        else:
            event_log.appendleft({
                "time": now_string(),
                "node": node_id,
                "event": "Unknown packet",
                "result": packet_type,
                "raw": packet,
            })


def serial_reader():
    while True:
        try:
            print(f"Opening serial port {SERIAL_PORT}...")
            ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
            print("Serial connected.")

            while True:
                line = ser.readline().decode("utf-8", errors="ignore").strip()

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

            with lock:
                latest_state["connected"] = False

            time.sleep(2)


@app.route("/")
def index():
    return render_template_string(DASHBOARD_HTML)


@app.route("/api/state")
def api_state():
    with lock:
        return jsonify({
            "state": latest_state,
            "events": list(event_log),
        })


DASHBOARD_HTML = """
<!DOCTYPE html>
<html>
<head>
    <title>Blue-Titan Dashboard</title>
    <style>
        body {
            font-family: Arial, sans-serif;
            background: #111827;
            color: #f9fafb;
            margin: 0;
            padding: 24px;
        }

        h1 {
            margin-bottom: 8px;
        }

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

        .status-ok {
            color: #22c55e;
            font-weight: bold;
        }

        .status-bad {
            color: #ef4444;
            font-weight: bold;
        }

        .status-warn {
            color: #facc15;
            font-weight: bold;
        }

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
        }
    </style>
</head>

<body>
    <h1>Blue-Titan PPE Monitoring Dashboard</h1>
    <div id="connection">Connection: loading...</div>

    <div class="grid">
        <div class="card">
            <h2>Live PPE Status</h2>
            <div id="ppe-status">No result yet</div>
        </div>

        <div class="card">
            <h2>Latest Detection</h2>
            <pre id="latest-json">No JSON received</pre>
        </div>

        <div class="card">
            <h2>Node Health</h2>
            <table>
                <thead>
                    <tr>
                        <th>Node</th>
                        <th>Status</th>
                        <th>Last Seen</th>
                    </tr>
                </thead>
                <tbody id="node-table"></tbody>
            </table>
        </div>

        <div class="card">
            <h2>Event Log</h2>
            <table>
                <thead>
                    <tr>
                        <th>Time</th>
                        <th>Node</th>
                        <th>Event</th>
                        <th>Result</th>
                    </tr>
                </thead>
                <tbody id="event-table"></tbody>
            </table>
        </div>
    </div>

<script>
async function updateDashboard() {
    const response = await fetch("/api/state");
    const data = await response.json();

    const state = data.state;
    const events = data.events;

    const connection = document.getElementById("connection");
    connection.innerHTML = state.connected
        ? `<span class="status-ok">Serial connected</span> | Last seen: ${state.last_seen}`
        : `<span class="status-bad">Serial disconnected</span>`;

    const ppeStatus = document.getElementById("ppe-status");
    const latestJson = document.getElementById("latest-json");

    if (state.latest_result) {
        const result = state.latest_result;

        if (result.ppe_detected) {
            ppeStatus.innerHTML = `
                <p class="status-ok">PPE compliant</p>
                <p>Node: ${result.node_id}</p>
                <p>Confidence: ${(result.confidence * 100).toFixed(1)}%</p>
            `;
        } else {
            ppeStatus.innerHTML = `
                <p class="status-bad">PPE non-compliant</p>
                <p>Node: ${result.node_id}</p>
                <p>Missing: ${(result.missing_items || []).join(", ")}</p>
                <p>Confidence: ${(result.confidence * 100).toFixed(1)}%</p>
                <p class="status-warn">Action: ${result.action || "alert"}</p>
            `;
        }

        latestJson.textContent = JSON.stringify(result, null, 2);
    }

    const nodeTable = document.getElementById("node-table");
    nodeTable.innerHTML = "";

    for (const [nodeId, node] of Object.entries(state.nodes)) {
        nodeTable.innerHTML += `
            <tr>
                <td>${nodeId}</td>
                <td>${node.status || "unknown"}</td>
                <td>${node.last_seen || "-"}</td>
            </tr>
        `;
    }

    const eventTable = document.getElementById("event-table");
    eventTable.innerHTML = "";

    for (const event of events) {
        eventTable.innerHTML += `
            <tr>
                <td>${event.time}</td>
                <td>${event.node}</td>
                <td>${event.event}</td>
                <td>${event.result}</td>
            </tr>
        `;
    }
}

setInterval(updateDashboard, 1000);
updateDashboard();
</script>

</body>
</html>
"""


if __name__ == "__main__":
    thread = threading.Thread(target=serial_reader, daemon=True)
    thread.start()

    app.run(host="0.0.0.0", port=5000, debug=True)