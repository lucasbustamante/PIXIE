#include "esp_camera.h"
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include "boot_animation.h"

// --- Pinos do display ---
#define TFT_CS   13
#define TFT_DC   15
#define TFT_RST  16
#define TFT_MOSI 12
#define TFT_SCLK 14
#define TFT_BL   1

// --- Botões e flash ---
#define BUTTON_ADC_PIN 2
#define FLASH_PIN      4

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

uint16_t lineBuffer[160];

bool sistemaLigado = true;
bool cameraLigada = false;

unsigned long lastButtonTime = 0;
const unsigned long debounceDelay = 350;
const unsigned long tempoSegurarPower = 3000;

enum ButtonType {
  BTN_NONE,
  BTN_FOTO,
  BTN_UP,
  BTN_DOWN
};

uint16_t swapRB(uint16_t color) {
  uint8_t r = (color >> 11) & 0x1F;
  uint8_t g = (color >> 5)  & 0x3F;
  uint8_t b = color & 0x1F;
  return (b << 11) | (g << 5) | r;
}

ButtonType lerBotao() {
  int valor = analogRead(BUTTON_ADC_PIN);

  if (valor < 100) return BTN_FOTO;
  if (valor > 200 && valor < 900) return BTN_DOWN;
  if (valor > 900 && valor < 3900) return BTN_UP;

  return BTN_NONE;
}

void backlightOn() {
  digitalWrite(TFT_BL, HIGH);
}

void backlightOff() {
  digitalWrite(TFT_BL, LOW);
}

bool lerPixelBitmapPROGMEM(const unsigned char *bitmap, int x, int y, int w) {
  int byteIndex = y * ((w + 7) / 8) + (x / 8);
  uint8_t byteValue = pgm_read_byte(bitmap + byteIndex);
  return byteValue & (0x80 >> (x % 8));
}

void desenharFrameTelaToda(const unsigned char *frame) {
  int telaW = tft.width();
  int telaH = tft.height();

  for (int y = 0; y < telaH; y++) {
    int srcY = (y * BOOT_ANIM_HEIGHT) / telaH;

    for (int x = 0; x < telaW; x++) {
      int srcX = (x * BOOT_ANIM_WIDTH) / telaW;

      bool pixel = lerPixelBitmapPROGMEM(frame, srcX, srcY, BOOT_ANIM_WIDTH);
      lineBuffer[x] = pixel ? ST77XX_WHITE : ST77XX_BLACK;
    }

    tft.drawRGBBitmap(0, y, lineBuffer, telaW, 1);
  }
}

void mostrarAnimacaoInicial() {
  tft.setRotation(3);
  tft.fillScreen(ST77XX_BLACK);

  for (int i = 0; i < BOOT_ANIM_FRAME_COUNT; i++) {
    const unsigned char *frame =
      (const unsigned char *)pgm_read_ptr(&boot_anim_frames[i]);

    desenharFrameTelaToda(frame);

    delay(BOOT_ANIM_DELAY_MS);
  }

  delay(150);

  tft.setRotation(2);
  tft.fillScreen(ST77XX_BLACK);
}

void mostrarMensagem(const char *msg) {
  tft.fillRect(0, 0, 80, 20, ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setCursor(5, 6);
  tft.print(msg);
}

void mostrarContadorPower(const char *acao, int segundos) {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);

  tft.setCursor(8, 35);
  tft.print(acao);

  tft.setTextSize(3);
  tft.setCursor(30, 70);
  tft.print(segundos);
}

void mostrarFrame(camera_fb_t *fb) {
  const int cropLeft = 40;
  const int offsetY  = 20;

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

bool iniciarCamera() {
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
    cameraLigada = false;
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();

  s->set_vflip(s, 0);
  s->set_hmirror(s, 0);
  s->set_whitebal(s, 1);
  s->set_gain_ctrl(s, 1);
  s->set_exposure_ctrl(s, 1);
  s->set_brightness(s, 0);
  s->set_contrast(s, 0);

  cameraLigada = true;
  return true;
}

void desligarCamera() {
  if (cameraLigada) {
    esp_camera_deinit();
    cameraLigada = false;
  }

  digitalWrite(PWDN_GPIO_NUM, HIGH);
}

void ligarSistema() {
  sistemaLigado = true;

  digitalWrite(PWDN_GPIO_NUM, LOW);
  delay(100);

  backlightOn();

  tft.initR(INITR_MINI160x80);
  tft.setSPISpeed(40000000);
  tft.invertDisplay(false);
  tft.enableDisplay(true);
  tft.fillScreen(ST77XX_BLACK);

  mostrarAnimacaoInicial();

  iniciarCamera();

  mostrarMensagem("LIGADO");
  delay(500);
}

void desligarSistema() {
  mostrarMensagem("DESLIGANDO");
  delay(500);

  digitalWrite(FLASH_PIN, LOW);

  desligarCamera();

  tft.fillScreen(ST77XX_BLACK);
  tft.enableDisplay(false);

  backlightOff();

  sistemaLigado = false;
}

void tirarFotoComFlash() {
  if (!sistemaLigado || !cameraLigada) return;

  digitalWrite(FLASH_PIN, HIGH);
  delay(150);

  camera_fb_t *fb = esp_camera_fb_get();

  if (fb) {
    mostrarFrame(fb);
    esp_camera_fb_return(fb);
    mostrarMensagem("FOTO!");
  }

  delay(250);
  digitalWrite(FLASH_PIN, LOW);
}

bool verificarPowerSegurado(bool ligar) {
  if (lerBotao() != BTN_FOTO) return false;

  unsigned long inicio = millis();
  int ultimoSegundo = -1;

  while (lerBotao() == BTN_FOTO) {
    unsigned long segurando = millis() - inicio;
    int restante = 3 - (segurando / 1000);

    if (restante != ultimoSegundo && restante > 0) {
      ultimoSegundo = restante;

      if (sistemaLigado) {
        mostrarContadorPower("Desligando", restante);
      }
    }

    if (segurando >= tempoSegurarPower) {
      if (ligar) {
        ligarSistema();
      } else {
        desligarSistema();
      }

      while (lerBotao() == BTN_FOTO) {
        delay(50);
      }

      return true;
    }

    delay(50);
  }

  return false;
}

void setup() {
  Serial.begin(115200);

  pinMode(FLASH_PIN, OUTPUT);
  digitalWrite(FLASH_PIN, LOW);

  pinMode(TFT_BL, OUTPUT);
  backlightOn();

  pinMode(PWDN_GPIO_NUM, OUTPUT);
  digitalWrite(PWDN_GPIO_NUM, LOW);

  pinMode(BUTTON_ADC_PIN, INPUT);

  vspi.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);

  tft.initR(INITR_MINI160x80);
  tft.setSPISpeed(40000000);
  tft.invertDisplay(false);
  tft.enableDisplay(true);

  mostrarAnimacaoInicial();

  iniciarCamera();

  tft.setRotation(2);
  tft.fillScreen(ST77XX_BLACK);
}

void loop() {
  ButtonType botao = lerBotao();

  if (!sistemaLigado) {
    verificarPowerSegurado(true);
    delay(100);
    return;
  }

  if (botao == BTN_FOTO) {
    bool desligou = verificarPowerSegurado(false);

    if (!desligou && millis() - lastButtonTime > debounceDelay) {
      lastButtonTime = millis();
      tirarFotoComFlash();
    }
  } 
  else if (botao != BTN_NONE && millis() - lastButtonTime > debounceDelay) {
    lastButtonTime = millis();

    if (botao == BTN_UP) {
      mostrarMensagem("UP");
    } 
    else if (botao == BTN_DOWN) {
      mostrarMensagem("DOWN");
    }
  }

  if (!cameraLigada) return;

  camera_fb_t *fb = esp_camera_fb_get();

  if (!fb) return;

  mostrarFrame(fb);
  esp_camera_fb_return(fb);
}