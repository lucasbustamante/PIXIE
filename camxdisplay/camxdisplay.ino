#include "esp_camera.h"
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

// --- Pinos do display ---
#define TFT_CS   13
#define TFT_DC   15
#define TFT_RST  2
#define TFT_MOSI 12
#define TFT_SCLK 14

// --- Botão e flash ---
#define BUTTON_PIN 16
#define FLASH_PIN  4

// ESP32-CAM AI Thinker
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

SPIClass vspi(VSPI);
Adafruit_ST7735 tft = Adafruit_ST7735(&vspi, TFT_CS, TFT_DC, TFT_RST);

uint16_t lineBuffer[80];

bool lastButtonState = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 250;

uint16_t swapRB(uint16_t color) {
  uint8_t r = (color >> 11) & 0x1F;
  uint8_t g = (color >> 5)  & 0x3F;
  uint8_t b = color & 0x1F;

  return (b << 11) | (g << 5) | r;
}

void mostrarFrame(camera_fb_t *fb) {
  const int cropLeft = 40;
  const int offsetY  = 20;

  // Correção: desenha as linhas de baixo para cima no display
  for (int y = 0; y < 120; y++) {
    int sourceY = 119 - y;

    uint16_t *src = (uint16_t *)(fb->buf + sourceY * 160 * 2);

    for (int x = 0; x < 80; x++) {
      int sourceX = x + cropLeft;
      lineBuffer[x] = swapRB(__builtin_bswap16(src[sourceX]));
    }

    tft.drawRGBBitmap(0, y + offsetY, lineBuffer, 80, 1);
  }
}

void tirarFotoComFlash() {
  digitalWrite(FLASH_PIN, HIGH);
  delay(150);

  camera_fb_t *fb = esp_camera_fb_get();

  if (fb) {
    mostrarFrame(fb);
    esp_camera_fb_return(fb);

    tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    tft.setCursor(5, 5);
    tft.print("FOTO!");
  }

  delay(300);
  digitalWrite(FLASH_PIN, LOW);
}

void setup() {
  Serial.begin(115200);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(FLASH_PIN, OUTPUT);
  digitalWrite(FLASH_PIN, LOW);

  Serial.printf("PSRAM detectada: %d bytes\n", ESP.getPsramSize());

  vspi.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);

  tft.initR(INITR_MINI160x80);
  tft.setSPISpeed(40000000);

  tft.setRotation(2);
  tft.fillScreen(ST77XX_BLACK);
  tft.invertDisplay(true);

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

  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;

  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_RGB565;
  config.frame_size   = FRAMESIZE_QQVGA;

  config.jpeg_quality = 12;
  config.fb_count     = 1;
  config.fb_location  = CAMERA_FB_IN_DRAM;
  config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;

  esp_err_t err = esp_camera_init(&config);

  if (err != ESP_OK) {
    Serial.printf("Erro câmera: 0x%x\n", err);

    tft.fillScreen(ST77XX_RED);
    tft.setCursor(5, 30);
    tft.setTextColor(ST77XX_WHITE);
    tft.print("Erro camera");

    return;
  }

  sensor_t *s = esp_camera_sensor_get();

  // Deixa sem inversão no sensor.
  // A correção foi feita no desenho do frame.
  s->set_vflip(s, 0);
  s->set_hmirror(s, 0);

  s->set_whitebal(s, 1);
  s->set_gain_ctrl(s, 1);
  s->set_exposure_ctrl(s, 1);
  s->set_brightness(s, 0);
  s->set_contrast(s, 0);

  tft.fillScreen(ST77XX_BLACK);
}

void loop() {
  bool buttonState = digitalRead(BUTTON_PIN);

  if (buttonState == LOW && lastButtonState == HIGH && millis() - lastDebounceTime > debounceDelay) {
    lastDebounceTime = millis();
    tirarFotoComFlash();
  }

  lastButtonState = buttonState;

  camera_fb_t *fb = esp_camera_fb_get();

  if (!fb) return;

  mostrarFrame(fb);

  esp_camera_fb_return(fb);
}