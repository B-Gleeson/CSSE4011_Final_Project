#!/usr/bin/env python

import subprocess
import signal
import sys
import time
import os

# ============================================================
# Configuration
# ============================================================

RECEIVER_SCRIPT = "receiver/transmitter.py"

INFERENCE_BINARY = "./ppe_inference"

DEFAULT_ENGINE = "models/ppe.engine"

# ============================================================
# Process list
# ============================================================

processes = []

# ============================================================
# Cleanup
# ============================================================

def cleanup(signum=None, frame=None):

    print("\nStopping processes...")

    for p in processes:

        try:
            p.terminate()
        except:
            pass

    time.sleep(1)

    for p in processes:

        try:
            p.kill()
        except:
            pass

    sys.exit(0)

# ============================================================
# Main
# ============================================================

def main():

    signal.signal(signal.SIGINT, cleanup)

    signal.signal(signal.SIGTERM, cleanup)

    # ========================================================
    # Engine path argument
    # ========================================================

    if len(sys.argv) > 1:

        engine_path = sys.argv[1]

    else:

        engine_path = DEFAULT_ENGINE

    # ========================================================
    # Validate engine exists
    # ========================================================

    if not os.path.isfile(engine_path):

        print(
            "Engine not found: {0}".format(
                engine_path
            )
        )

        sys.exit(1)

    print(
        "Using engine: {0}".format(
            engine_path
        )
    )

    # ========================================================
    # Start receiver
    # ========================================================

    print("Starting receiver...")

    receiver_process = subprocess.Popen(
        ["python", RECEIVER_SCRIPT]
    )

    processes.append(receiver_process)

    time.sleep(2)

    # ========================================================
    # Start inference
    # ========================================================

    print("Starting inference...")

    inference_process = subprocess.Popen(
        [
            INFERENCE_BINARY,
            engine_path
        ]
    )

    processes.append(inference_process)

    print("System running.")

    # ========================================================
    # Monitor processes
    # ========================================================

    while True:

        time.sleep(1)

        # ====================================================
        # Receiver alive
        # ====================================================

        if receiver_process.poll() is not None:

            print(
                "Receiver process exited."
            )

            cleanup()

        # ====================================================
        # Inference alive
        # ====================================================

        if inference_process.poll() is not None:

            print(
                "Inference process exited."
            )

            cleanup()

# ============================================================
# Entry
# ============================================================

if __name__ == "__main__":

    main()