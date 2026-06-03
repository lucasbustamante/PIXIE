#include "esp_camera.h"
#include "FS.h"
#include "SD_MMC.h"
#include "BluetoothSerial.h"

#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error Bluetooth is not enabled. Enable Bluetooth in the ESP32 Arduino core configuration.
#endif

// AI Thinker ESP32-CAM pin map.
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27

#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

#define FLASH_LED_PIN 4

static const char *BT_DEVICE_NAME = "ESP32-CAM-BT";
static const uint32_t DEFAULT_STREAM_INTERVAL_MS = 1500;
static const uint32_t MIN_STREAM_INTERVAL_MS = 700;
static const uint32_t MAX_STREAM_INTERVAL_MS = 10000;
static const uint16_t FLASH_WARMUP_MS = 180;
static const size_t BT_CHUNK_SIZE = 1024;

BluetoothSerial SerialBT;

String rxLine;
bool sdReady = false;
bool wasConnected = false;
bool streaming = false;
uint32_t streamIntervalMs = DEFAULT_STREAM_INTERVAL_MS;
uint32_t lastFrameAt = 0;
uint32_t photoCounter = 0;

void sendMessage(const String &message) {
  Serial.println("[MSG] " + message);
  if (SerialBT.hasClient()) {
    SerialBT.print("MSG ");
    SerialBT.print(message);
    SerialBT.print('\n');
  }
}

bool sendImage(const char *kind, const uint8_t *data, size_t length) {
  if (!SerialBT.hasClient()) {
    return false;
  }

  SerialBT.printf("IMG %s %u\n", kind, (unsigned int)length);

  size_t offset = 0;
  while (offset < length && SerialBT.hasClient()) {
    size_t remaining = length - offset;
    size_t chunk = remaining > BT_CHUNK_SIZE ? BT_CHUNK_SIZE : remaining;
    size_t written = SerialBT.write(data + offset, chunk);

    if (written == 0) {
      delay(8);
      continue;
    }

    offset += written;
    delay(2);
  }

  SerialBT.flush();
  return offset == length;
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

  // QVGA keeps the JPEG small enough for stable Bluetooth transfer.
  config.frame_size = FRAMESIZE_QVGA;
  config.jpeg_quality = 14;
  config.fb_count = psramFound() ? 2 : 1;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    return false;
  }

  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor) {
    sensor->set_framesize(sensor, FRAMESIZE_QVGA);
    sensor->set_quality(sensor, 14);
    sensor->set_brightness(sensor, 0);
    sensor->set_saturation(sensor, 0);
  }

  return true;
}

bool initSdCard() {
  // Use 1-bit SD mode so GPIO4 stays available for the flash LED.
  if (!SD_MMC.begin("/sdcard", true, false)) {
    Serial.println("SD_MMC mount failed. Photos will not be saved.");
    return false;
  }

  uint8_t cardType = SD_MMC.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("No microSD card found.");
    return false;
  }

  Serial.printf("microSD ready. Size: %llu MB\n", SD_MMC.cardSize() / (1024ULL * 1024ULL));
  return true;
}

String nextPhotoPath() {
  for (uint32_t attempt = 0; attempt < 100000; attempt++) {
    photoCounter++;
    char path[32];
    snprintf(path, sizeof(path), "/photo_%06lu.jpg", (unsigned long)photoCounter);
    if (!SD_MMC.exists(path)) {
      return String(path);
    }
  }

  return "/photo_latest.jpg";
}

bool saveJpegToSd(const String &path, const uint8_t *data, size_t length) {
  if (!sdReady) {
    return false;
  }

  File file = SD_MMC.open(path, FILE_WRITE);
  if (!file) {
    return false;
  }

  size_t written = file.write(data, length);
  file.close();
  return written == length;
}

camera_fb_t *captureJpeg(bool useFlash) {
  if (useFlash) {
    digitalWrite(FLASH_LED_PIN, HIGH);
    delay(FLASH_WARMUP_MS);
  }

  camera_fb_t *fb = esp_camera_fb_get();

  if (useFlash) {
    digitalWrite(FLASH_LED_PIN, LOW);
  }

  if (!fb) {
    sendMessage("ERROR CAMERA_CAPTURE_FAILED");
  }

  return fb;
}

void capturePhoto(bool useFlash) {
  bool resumeStream = streaming;
  streaming = false;

  sendMessage(useFlash ? "CAPTURING FLASH" : "CAPTURING NO_FLASH");

  camera_fb_t *fb = captureJpeg(useFlash);
  if (!fb) {
    streaming = resumeStream;
    return;
  }

  String path = nextPhotoPath();
  bool saved = saveJpegToSd(path, fb->buf, fb->len);

  if (saved) {
    sendMessage("SAVED " + path + " " + String(fb->len));
  } else {
    sendMessage("ERROR SD_SAVE_FAILED");
  }

  if (!sendImage("PHOTO", fb->buf, fb->len)) {
    Serial.println("Could not send photo over Bluetooth.");
  }

  esp_camera_fb_return(fb);
  streaming = resumeStream;
  lastFrameAt = millis();
}

void sendStreamFrame() {
  camera_fb_t *fb = captureJpeg(false);
  if (!fb) {
    return;
  }

  if (!sendImage("FRAME", fb->buf, fb->len)) {
    Serial.println("Could not send stream frame over Bluetooth.");
  }

  esp_camera_fb_return(fb);
}

void processCommand(String command) {
  command.trim();
  if (command.length() == 0) {
    return;
  }

  String upper = command;
  upper.toUpperCase();
  Serial.println("[CMD] " + upper);

  if (upper == "PING") {
    sendMessage("PONG");
    return;
  }

  if (upper == "START_STREAM") {
    streaming = true;
    lastFrameAt = 0;
    sendMessage("STREAM_STARTED");
    return;
  }

  if (upper == "STOP_STREAM") {
    streaming = false;
    sendMessage("STREAM_STOPPED");
    return;
  }

  if (upper.startsWith("SET_INTERVAL ")) {
    String value = upper.substring(String("SET_INTERVAL ").length());
    uint32_t requested = value.toInt();
    if (requested < MIN_STREAM_INTERVAL_MS) {
      requested = MIN_STREAM_INTERVAL_MS;
    }
    if (requested > MAX_STREAM_INTERVAL_MS) {
      requested = MAX_STREAM_INTERVAL_MS;
    }

    streamIntervalMs = requested;
    sendMessage("INTERVAL " + String(streamIntervalMs));
    return;
  }

  if (upper.startsWith("CAPTURE")) {
    String arg = "0";
    int spaceIndex = upper.indexOf(' ');
    if (spaceIndex >= 0) {
      arg = upper.substring(spaceIndex + 1);
      arg.trim();
    }

    bool useFlash = arg == "1" || arg == "FLASH" || arg == "WITH_FLASH";
    capturePhoto(useFlash);
    return;
  }

  sendMessage("ERROR UNKNOWN_COMMAND");
}

void handleBluetoothInput() {
  while (SerialBT.available()) {
    char c = (char)SerialBT.read();
    if (c == '\r') {
      continue;
    }

    if (c == '\n') {
      processCommand(rxLine);
      rxLine = "";
      continue;
    }

    if (rxLine.length() < 96) {
      rxLine += c;
    } else {
      rxLine = "";
      sendMessage("ERROR COMMAND_TOO_LONG");
    }
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(false);
  delay(500);

  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);

  Serial.println();
  Serial.println("Starting ESP32-CAM Bluetooth camera...");

  if (!initCamera()) {
    Serial.println("Restarting because camera initialization failed.");
    delay(3000);
    ESP.restart();
  }

  sdReady = initSdCard();

  if (!SerialBT.begin(BT_DEVICE_NAME)) {
    Serial.println("Bluetooth start failed. Restarting...");
    delay(3000);
    ESP.restart();
  }

  Serial.println("Bluetooth ready as: " + String(BT_DEVICE_NAME));
  Serial.println("Pair from Android, connect, then send START_STREAM or CAPTURE 0/1.");
}

void loop() {
  bool connected = SerialBT.hasClient();

  if (connected && !wasConnected) {
    wasConnected = true;
    rxLine = "";
    sendMessage("READY " + String(BT_DEVICE_NAME));
  }

  if (!connected && wasConnected) {
    wasConnected = false;
    streaming = false;
    rxLine = "";
    Serial.println("Bluetooth client disconnected.");
  }

  if (!connected) {
    delay(50);
    return;
  }

  handleBluetoothInput();

  if (streaming && millis() - lastFrameAt >= streamIntervalMs) {
    lastFrameAt = millis();
    sendStreamFrame();
  }

  delay(5);
}
