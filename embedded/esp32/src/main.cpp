#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>

// =======================
// Wi-Fi details
// =======================
const char* WIFI_SSID     = "csse4011";
const char* WIFI_PASSWORD = "csse4011wifi";

// Jetson receiver URL — camera still POSTs frames here
const char* JETSON_UPLOAD_URL = "http://192.168.1.54:5000/upload";

// Static IP for the ESP32-CAM so the M5 Core can always reach it.

IPAddress LOCAL_IP(192, 168, 1, 80);
IPAddress GATEWAY(192, 168, 1, 1);
IPAddress SUBNET(255, 255, 255, 0);

//IPAddress LOCAL_IP(172, 20, 10, 10);      // ESP32-CAM
//IPAddress GATEWAY(172, 20, 10, 1);        // iPhone hotspot
//IPAddress SUBNET(255, 255, 255, 240);     // /28 mask

// Periodic auto-capture (set to 0 to disable)
const unsigned long PHOTO_INTERVAL_MS = 30000;
unsigned long lastPhotoTime = 0;

// =======================
// AI Thinker ESP32-CAM pins
// =======================
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5

#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// =======================
// Web server
// =======================
WebServer server(80);

// Forward declarations
void connectToWiFi();
bool initCamera();
bool captureAndSendPhoto();
void handleCapture();
void handleStatus();

// =======================
// Setup
// =======================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("ESP32-CAM starting...");

  connectToWiFi();

  if (!initCamera()) {
    Serial.println("Camera init failed. Restarting in 5 seconds...");
    delay(5000);
    ESP.restart();
  }

  // Register HTTP routes
  server.on("/capture", HTTP_POST, handleCapture);
  server.on("/status",  HTTP_GET,  handleStatus);

  server.begin();
  Serial.println("HTTP server started on port 80.");
  Serial.print("ESP32-CAM IP: ");
  Serial.println(WiFi.localIP());
}

// =======================
// Loop
// =======================
void loop() {
  server.handleClient();

  unsigned long now = millis();

  if (PHOTO_INTERVAL_MS > 0 &&
      (lastPhotoTime == 0 || now - lastPhotoTime >= PHOTO_INTERVAL_MS)) {
    lastPhotoTime = now;
    captureAndSendPhoto();
  }

  delay(10);
}

// =======================
// HTTP handlers
// =======================

/*
 * POST /capture
 *
 * Called by the M5 Core base node when the dashboard sends a
 * "take_photo" command.  Captures a frame, POSTs it to the Jetson,
 * and returns a JSON result to the caller.
 */
void handleCapture() {
  Serial.println("[/capture] Triggered by remote command.");

  bool ok = captureAndSendPhoto();

  if (ok) {
    server.send(200, "application/json",
                "{\"status\":\"ok\",\"message\":\"photo sent to Jetson\"}");
  } else {
    server.send(500, "application/json",
                "{\"status\":\"error\",\"message\":\"capture or upload failed\"}");
  }
}

/*
 * GET /status
 *
 * Simple health-check so the M5 Core (or the dashboard) can confirm
 * the camera is alive and connected.
 */
void handleStatus() {
  String json = "{\"status\":\"ok\","
                "\"ip\":\"" + WiFi.localIP().toString() + "\","
                "\"rssi\":" + String(WiFi.RSSI()) + "}";
  server.send(200, "application/json", json);
}

// =======================
// Wi-Fi
// =======================
void connectToWiFi() {
  Serial.print("Connecting to Wi-Fi: ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);

  // Request static IP so the M5 Core always knows where to find us
  if (!WiFi.config(LOCAL_IP, GATEWAY, SUBNET)) {
    Serial.println("Static IP config failed — falling back to DHCP.");
  }

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    attempts++;

    if (attempts > 60) {
      Serial.println();
      Serial.println("Wi-Fi connection failed. Restarting...");
      ESP.restart();
    }
  }

  Serial.println();
  Serial.println("Wi-Fi connected.");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

// =======================
// Camera init
// =======================
bool initCamera() {
  camera_config_t config;

  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;

  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;

  config.pin_xclk    = XCLK_GPIO_NUM;
  config.pin_pclk    = PCLK_GPIO_NUM;
  config.pin_vsync   = VSYNC_GPIO_NUM;
  config.pin_href    = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn    = PWDN_GPIO_NUM;
  config.pin_reset   = RESET_GPIO_NUM;

  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    Serial.println("PSRAM found.");
    config.frame_size   = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count     = 2;
    config.grab_mode    = CAMERA_GRAB_LATEST;
  } else {
    Serial.println("No PSRAM. Using smaller frame size.");
    config.frame_size   = FRAMESIZE_VGA;
    config.jpeg_quality = 15;
    config.fb_count     = 1;
    config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
  }

  config.fb_location = CAMERA_FB_IN_PSRAM;

  esp_err_t err = esp_camera_init(&config);

  if (err != ESP_OK) {
    Serial.printf("esp_camera_init failed: 0x%x\n", err);
    return false;
  }

  Serial.println("Camera initialized.");
  return true;
}

// =======================
// Capture + upload
// =======================
bool captureAndSendPhoto() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi lost. Reconnecting...");
    connectToWiFi();
  }

  Serial.println("Capturing photo...");

  camera_fb_t* fb = esp_camera_fb_get();

  if (!fb) {
    Serial.println("Camera capture failed.");
    return false;
  }

  Serial.printf("Captured: %u bytes\n", fb->len);

  HTTPClient http;
  http.begin(JETSON_UPLOAD_URL);
  http.addHeader("Content-Type", "image/jpeg");
  http.addHeader("X-Device-ID",  "esp32cam-01");

  int code = http.POST(fb->buf, fb->len);

  bool success = (code > 0 && code < 400);

  if (success) {
    Serial.printf("Jetson responded: %d\n", code);
  } else {
    Serial.printf("Upload failed: %s\n",
                  http.errorToString(code).c_str());
  }

  http.end();
  esp_camera_fb_return(fb);

  return success;
}
