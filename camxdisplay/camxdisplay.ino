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

// Usa VSPI (hardware SPI) com velocidade máxima
SPIClass vspi(VSPI);
Adafruit_ST7735 tft = Adafruit_ST7735(&vspi, TFT_CS, TFT_DC, TFT_RST);

// Buffer de uma linha completa
uint16_t lineBuffer[160];

void setup() {
  Serial.begin(115200);

  // Inicia SPI em 40 MHz (seguro para a maioria dos módulos ST7735)
  vspi.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.initR(INITR_MINI160x80);
  tft.setSPISpeed(40000000);  // 40 MHz — tente 80000000 se quiser mais risco/ganho
  tft.setRotation(1);
  tft.fillScreen(ST77XX_BLACK);

  // FIX DE COR: inverte o display para corrigir R<->B trocado
  tft.invertDisplay(true);

  // --- Configuração da câmera ---
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
  config.frame_size   = FRAMESIZE_QQVGA; // 160x120
  config.jpeg_quality = 12;
  config.fb_count     = 2;               // 2 buffers = menos espera
  config.fb_location  = CAMERA_FB_IN_PSRAM;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Erro câmera: 0x%x\n", err);
    tft.fillScreen(ST77XX_RED);
    tft.setCursor(5, 30);
    tft.setTextColor(ST77XX_WHITE);
    tft.print("Erro camera");
    return;
  }

  // Sensor: ganho automático e balanço de branco
  sensor_t *s = esp_camera_sensor_get();
  s->set_whitebal(s, 1);
  s->set_gain_ctrl(s, 1);
  s->set_exposure_ctrl(s, 1);
  s->set_brightness(s, 0);
  s->set_contrast(s, 0);

  tft.fillScreen(ST77XX_BLACK);
}

void loop() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) return;

  const int cropTop = 20; // corta para 160x80 (centro da imagem 160x120)

  for (int y = 0; y < 80; y++) {
    int sourceY = y + cropTop;
    uint16_t *src = (uint16_t *)(fb->buf + sourceY * 160 * 2);

    // FIX DE COR: swap de bytes low/high de cada pixel
    for (int x = 0; x < 160; x++) {
      lineBuffer[x] = __builtin_bswap16(src[x]);
    }

    tft.drawRGBBitmap(0, y, lineBuffer, 160, 1);
  }

  esp_camera_fb_return(fb);
}