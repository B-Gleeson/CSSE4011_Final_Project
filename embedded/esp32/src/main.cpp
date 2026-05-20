#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <HTTPClient.h>

// =======================
// Wi-Fi details
// =======================
const char* WIFI_SSID = "Iphone 67";
const char* WIFI_PASSWORD = "sunasuna";

// Jetson receiver URL
const char* SERVER_URL = "http://172.20.10.9:5000/upload";

// Take one photo every 30 seconds
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

void connectToWiFi();
bool initCamera();
void captureAndSendPhoto();

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("ESP32-CAM HTTP photo sender starting...");

  connectToWiFi();

  if (!initCamera()) {
    Serial.println("Camera init failed. Restarting in 5 seconds...");
    delay(5000);
    ESP.restart();
  }

  Serial.println("Setup complete.");
}

void loop() {
  unsigned long now = millis();

  if (now - lastPhotoTime >= PHOTO_INTERVAL_MS || lastPhotoTime == 0) {
    lastPhotoTime = now;
    captureAndSendPhoto();
  }

  delay(100);
}

void connectToWiFi() {
  Serial.print("Connecting to Wi-Fi: ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
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
  Serial.print("ESP32 IP address: ");
  Serial.println(WiFi.localIP());
}

bool initCamera() {
  camera_config_t config;

  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;

  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;

  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;

  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;

  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;

  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  // For reliability, start moderate.
  // You can later try FRAMESIZE_SVGA, FRAMESIZE_XGA, etc.
  if (psramFound()) {
    Serial.println("PSRAM found.");
    config.frame_size = FRAMESIZE_SVGA;   // 800x600
    config.jpeg_quality = 12;             // lower number = better quality/larger file
    config.fb_count = 2;
    config.grab_mode = CAMERA_GRAB_LATEST;
  } else {
    Serial.println("No PSRAM found. Using smaller frame size.");
    config.frame_size = FRAMESIZE_VGA;    // 640x480
    config.jpeg_quality = 15;
    config.fb_count = 1;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  }

  config.fb_location = CAMERA_FB_IN_PSRAM;

  esp_err_t err = esp_camera_init(&config);

  if (err != ESP_OK) {
    Serial.printf("esp_camera_init failed with error 0x%x\n", err);
    return false;
  }

  Serial.println("Camera initialized.");
  return true;
}

void captureAndSendPhoto() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi disconnected. Reconnecting...");
    connectToWiFi();
  }

  Serial.println("Capturing photo...");

  camera_fb_t* fb = esp_camera_fb_get();

  if (!fb) {
    Serial.println("Camera capture failed.");
    return;
  }

  Serial.printf("Captured image: %u bytes\n", fb->len);

  HTTPClient http;

  Serial.print("Posting to: ");
  Serial.println(SERVER_URL);

  http.begin(SERVER_URL);
  http.addHeader("Content-Type", "image/jpeg");
  http.addHeader("X-Device-ID", "esp32cam-01");

  int httpResponseCode = http.POST(fb->buf, fb->len);

  if (httpResponseCode > 0) {
    Serial.printf("HTTP response code: %d\n", httpResponseCode);
    String response = http.getString();
    Serial.println("Server response:");
    Serial.println(response);
  } else {
    Serial.printf("HTTP POST failed. Error: %s\n",
                  http.errorToString(httpResponseCode).c_str());
  }

  http.end();

  // Important: return frame buffer after use
  esp_camera_fb_return(fb);

  Serial.println("Photo cycle complete.");
}