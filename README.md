## Building the Jetson Inference Application

The PPE detection application is compiled directly on the Jetson Xavier NX using `g++`, TensorRT, CUDA, and OpenCV.

### Requirements

The following dependencies must be installed on the Jetson:

- NVIDIA TensorRT
- CUDA Toolkit
- OpenCV 4
- GNU g++

### Build Command

Run the following command from the project root directory:

```bash
g++ src/main.cpp src/detector.cpp \
-o ppe_inference \
-Iinclude \
-I/usr/include/opencv4 \
-I/usr/local/cuda/include \
-L/usr/local/cuda/lib64 \
-lnvinfer \
-lcudart \
-lopencv_core \
-lopencv_imgproc \
-lopencv_highgui \
-lopencv_imgcodecs \
-lopencv_dnn \
-lopencv_videoio \
-lpthread \
-lstdc++fs
```

### Running the Application

After compilation, run the inference application from the root directory using:

```bash
python run_system.py
```

### Runtime Behaviour

The application continuously monitors the `images/incoming` directory for newly uploaded images.

For each image:
1. The image is loaded into memory
2. TensorRT YOLO inference is performed
3. Bounding boxes and labels are drawn
4. The processed image is saved to `images/output`
5. AI telemetry is written to `latest_result.json`
6. The original input image is deleted

# Firmware Build and Flash Instructions

## M5Stack Core2 Base Station

The M5Stack Core2 firmware is built using Zephyr RTOS targeting the ESP32 processor core.

### Build

```bash
west build -p always -b m5stack_core2/esp32/procpu
```

### Flash

```bash
west flash --esp-device /dev/ttyACM0
```

---

# nRF52840 BLE Sensor Node

The BLE humidity and alarm node targets the Seeed XIAO nRF52840 Sense board using Zephyr RTOS.

### Build

```bash
west build -p -b xiao_ble/nrf52840/sense
```

### Flash

```bash
west flash
```

---

# Dashboard Application

The dashboard application is written in Python.

Navigate to the dashboard directory:

```bash
cd dashboard
```

Run using:

```bash
python3 main.py
```

or:

```bash
python main.py
```

---

# ESP32-CAM Firmware

The ESP32-CAM firmware is developed using PlatformIO.

### Build

Use the PlatformIO build command:

```bash
pio run
```

### Upload

```bash
pio run --target upload
```

### Serial Monitor

```bash
pio device monitor
```