// ESP32-CAM video stream + PIR motion sensor web dashboard
// Works with AI-Thinker ESP32-CAM module
//
// Wiring:
// PIR VCC -> 5V (VIN on board)   (or 3.3V if your PIR supports it)
// PIR GND -> GND
// PIR OUT -> GPIO13  (change PIR_PIN below if needed)

#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>

// --------- User settings ----------
const char* ssid = "SSID";
const char* password = "PASSWORD";

#define PIR_PIN 13          // PIR output pin (change if necessary)
#define STREAM_PORT 80
// ----------------------------------

// AI-Thinker camera pin mapping
// Leave as-is if using AI-Thinker module
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

WebServer server(STREAM_PORT);

volatile bool motionDetected = false;
unsigned long lastMotionMillis = 0;
const unsigned long MOTION_HOLD_MS = 5000; // keep "motion" state for this many ms after last trigger

// Interrupt handler (optional) - keep short
void IRAM_ATTR pirISR() {
  motionDetected = true;
  lastMotionMillis = millis();
}

String getContentType(String filename) {
  if (server.hasArg("download")) return "application/octet-stream";
  else if (filename.endsWith(".htm")) return "text/html";
  else if (filename.endsWith(".html")) return "text/html";
  else if (filename.endsWith(".css")) return "text/css";
  else if (filename.endsWith(".js")) return "application/javascript";
  else if (filename.endsWith(".png")) return "image/png";
  return "text/plain";
}

// streaming multipart content
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=frame";
static const char* _STREAM_BOUNDARY = "\r\n--frame\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

void handleStream() {
  WiFiClient client = server.client();

  String response = String("HTTP/1.1 200 OK\r\n") +
                    "Content-Type: " + String(_STREAM_CONTENT_TYPE) + "\r\n" +
                    "Connection: close\r\n\r\n";
  client.print(response);

  while (client.connected()) {
    camera_fb_t * fb = NULL;
    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed");
      delay(100);
      continue;
    }

    client.print(_STREAM_BOUNDARY);
    char partBuf[64];
    sprintf(partBuf, _STREAM_PART, fb->len);
    client.print(partBuf);
    client.write(fb->buf, fb->len);
    esp_camera_fb_return(fb);

    // small delay to allow client to receive
    if (!client.connected()) break;
    delay(10);
  }
}

// Main page: shows stream and status, uses AJAX to poll /status
void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                "<title>ESP32-CAM Motion Stream</title>"
                "<style>"
                "body{margin:0;font-family:Helvetica,Arial;color:#fff;background:linear-gradient(135deg,#0f2027,#203a43,#2c5364);display:flex;flex-direction:column;align-items:center;}"
                ".container{margin:20px;padding:20px;background:rgba(255,255,255,0.05);border-radius:16px;box-shadow:0 10px 30px rgba(0,0,0,0.4);transform:perspective(800px) translateZ(0);} "
                "h1{margin:0 0 12px 0;text-align:center;} "
                ".stream{border-radius:12px;overflow:hidden;box-shadow:0 8px 24px rgba(0,0,0,0.6);} "
                "img#cam{display:block;width:320px;max-width:96vw;height:auto;background:#000;} "
                ".status{margin-top:12px;padding:12px;border-radius:10px;text-align:center;font-weight:700;min-width:220px;}"
                ".motion{background:linear-gradient(90deg,#ff4d4d,#ff8474);color:#300;} "
                ".nomotion{background:linear-gradient(90deg,#7efc7e,#3bd26f);color:#023;} "
                ".controls{display:flex;gap:10px;margin-top:12px;justify-content:center;}"
                ".btn{padding:8px 12px;border-radius:8px;border:none;background:rgba(255,255,255,0.06);color:#fff;cursor:pointer;}"
                "@media(min-width:700px){img#cam{width:640px;}}"
                "</style>"
                "</head><body>"
                "<div class='container'>"
                "<h1>ESP32-CAM Motion Stream</h1>"
                "<div class='stream'><img id='cam' src='/stream' alt='Camera stream'></div>"
                "<div id='statusBox' class='status nomotion'>Loading...</div>"
                "<div class='controls'>"
                "<button class='btn' onclick='toggleAutoReload()'>Toggle Auto-Update</button>"
                "<button class='btn' onclick='snapshot()'>Snapshot (download)</button>"
                "</div>"
                "</div>"
                "<script>"
                "let auto = true;"
                "function fetchStatus(){"
                " fetch('/status').then(r=>r.json()).then(j=>{"
                "   const box=document.getElementById('statusBox');"
                "   if(j.motion){ box.className='status motion'; box.innerText='⚠ Motion Detected!';"
                "   } else { box.className='status nomotion'; box.innerText='✅ No Motion'; }"
                " });"
                " if(auto) setTimeout(fetchStatus,800);"
                "}"
                "function toggleAutoReload(){ auto = !auto; if(auto) fetchStatus(); }"
                "function snapshot(){ window.location.href='/capture'; }"
                "window.onload = function(){ fetchStatus(); };"
                "</script>"
                "</body></html>";
  server.send(200, "text/html", html);
}

// Return JSON status { motion: true/false }
void handleStatus() {
  // Use hold window to keep motion true for a short time after trigger
  bool m = motionDetected && (millis() - lastMotionMillis <= MOTION_HOLD_MS);
  // If older than hold, clear
  if (!m) {
    motionDetected = false;
  }
  String json = String("{\"motion\":") + (m ? "true" : "false") + "}";
  server.send(200, "application/json", json);
}

// Capture single frame and send as JPEG to download
void handleCapture() {
  camera_fb_t * fb = esp_camera_fb_get();
  if (!fb) {
    server.send(500, "text/plain", "Camera capture failed");
    return;
  }
  server.sendHeader("Content-Disposition", "inline; filename=capture.jpg");
  server.send_P(200, "image/jpeg", (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
}

// Setup camera
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
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  // init with high framesize for better quality (change to FRAMESIZE_UXGA or lower if memory issues)
  config.frame_size = FRAMESIZE_VGA; // 640x480 - lower memory usage
  config.jpeg_quality = 12; // 0-63 lower means better quality
  config.fb_count = 1;      // Only 1 frame buffer to save RAM

  Serial.println("Before camera init");
  esp_err_t err = esp_camera_init(&config);
  Serial.println("After camera init");
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return false;
  }
  return true;
}

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("Starting ESP32-CAM with PIR...");

  pinMode(PIR_PIN, INPUT);
  // optional: attachInterrupt(digitalPinToInterrupt(PIR_PIN), pirISR, RISING);
  // Using polling below (reliable for PIR)

  if (!initCamera()) {
    Serial.println("Camera init failed");
    while (1) { delay(1000); }
  }

  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
    if (millis() - start > 20000) break;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("Connected! IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println();
    Serial.println("WiFi connect failed - continuing (you can still connect to AP if implemented)");
  }

  // Routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/stream", HTTP_GET, [](){ handleStream(); });
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/capture", HTTP_GET, handleCapture);

  server.begin();
  Serial.println("HTTP server started");
}

unsigned long lastPoll = 0;
const unsigned long POLL_INTERVAL = 150; // ms

void loop() {
  server.handleClient();

  // Poll PIR (debounce-ish and track last motion time)
  if (millis() - lastPoll >= POLL_INTERVAL) {
    lastPoll = millis();
    int v = digitalRead(PIR_PIN);
    if (v == HIGH) {
      motionDetected = true;
      lastMotionMillis = millis();
      Serial.println("Motion detected!");
      // You can add code here to start recording / change stream resolution / send alert
    }
    // if PIR stays HIGH the hold window keeps it true; we clear after MOTION_HOLD_MS if no new trigger
    if (motionDetected && (millis() - lastMotionMillis > MOTION_HOLD_MS)) {
      motionDetected = false;
      Serial.println("Motion cleared");
    }
  }
}
