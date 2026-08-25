#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include <FS.h>
#include <SD_MMC.h>
#include <SPI.h>
#include <Preferences.h>
#include <TJpg_Decoder.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include "boot_animation.h"

// --- Projeto ---
#define PROJECT_NAME        "PixieCam"
#define PROJECT_VERSION     "1.2.0"
#define PROJECT_DEVELOPER   "Desenvolvedor: edite este texto"
#define DCIM_DIR            "/DCIM"
#define PHOTO_PREFIX        "PHOTO_"
#define PHOTO_EXTENSION     ".jpg"
#define FIRMWARE_BUILD_ID    __DATE__ " " __TIME__

// --- Pinos do display ST7735 0.96 80x160 ---
#define TFT_CS   13
#define TFT_DC   15
#define TFT_RST  16
#define TFT_MOSI 12
#define TFT_SCLK 14
#define TFT_BL   1

// --- Botoes e flash ---
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

// SD_MMC do slot interno do ESP32-CAM AI Thinker em modo 1-bit:
// CLK=14, CMD=15, D0=2. Esses pinos se sobrepoem ao TFT/botoes neste projeto,
// por isso o cartao e montado apenas durante operacoes de arquivo.
#define SD_MMC_CLK_PIN 14
#define SD_MMC_CMD_PIN 15
#define SD_MMC_D0_PIN   2

// Deixe 0 para abrir a camera imediatamente apos o boot. Em alguns hardwares,
// SD_MMC.begin() pode demorar ou travar quando o SD compartilha pinos com TFT.
#define CHECK_SD_ON_BOOT 0

#define TFT_APP_ROTATION 3
#define CAMERA_ROTATE_90_CW 1
#define BUTTON_CALIBRATION_VERSION 1
#define BUTTON_CALIBRATION_MIN_DELTA 80
#define BUTTON_CALIBRATION_SAMPLES 24
#define SCREEN_W 160
#define SCREEN_H 80
#define MAX_PHOTOS 120
#define PHOTO_PATH_LEN 48
#define CAPTURE_SRC_W 160
#define CAPTURE_SRC_H 120
#define PHOTO_W 80
#define PHOTO_H 120
#define PHOTO_CROP_LEFT 40
#define BMP_HEADER_SIZE 54
#define PREVIEW_AREA_Y 12
#define PREVIEW_AREA_H 68
#define GALLERY_INFO_Y 66
#define CAMERA_CAPTURE_JPEG_QUALITY 2
#define CAMERA_PREVIEW_JPEG_QUALITY 12
#define CAMERA_PREVIEW_FRAME_SIZE FRAMESIZE_QVGA
#define CAMERA_PREVIEW_FILL_SCREEN 1
#define CAPTURE_SENSOR_SETTLE_MS 900
#define PREVIEW_SENSOR_SETTLE_MS 35
#define CAPTURE_WARMUP_FRAMES 2
#define CAPTURE_TRY_ULTRA_RES 1
#define SD_IO_BUFFER_SIZE 4096
#define MAX_GALLERY_JPEG_BYTES 5000000

#define COLOR_BG       0x0000
#define COLOR_PANEL    0x1082
#define COLOR_PANEL_2  0x2104
#define COLOR_ACCENT   0x05FF
#define COLOR_ACCENT_2 0x07E0
#define COLOR_TEXT     0xFFFF
#define COLOR_MUTED    0x8410
#define COLOR_WARN     0xFD20
#define COLOR_BAD      0xF800
#define COLOR_OK       0x07E0

SPIClass vspi(VSPI);
Adafruit_ST7735 tft = Adafruit_ST7735(&vspi, TFT_CS, TFT_DC, TFT_RST);
Preferences preferences;

uint16_t lineBuffer[SCREEN_W];
DMA_ATTR uint8_t sdIoBuffer[SD_IO_BUFFER_SIZE];

enum AppState {
  STATE_BOOT,
  STATE_CAMERA,
  STATE_MAIN_MENU,
  STATE_SETTINGS,
  STATE_SD_INFO,
  STATE_SD_FORMAT_CONFIRM,
  STATE_FLASH_SETTINGS,
  STATE_ABOUT,
  STATE_GALLERY,
  STATE_POWER_CONFIRM,
  STATE_SHUTDOWN
};

enum ButtonType {
  BTN_NONE,
  BTN_FOTO,
  BTN_UP,
  BTN_DOWN
};

enum ButtonEventType {
  EVT_NONE,
  EVT_OK_SHORT,
  EVT_OK_LONG,
  EVT_POWER_HOLD,
  EVT_UP,
  EVT_DOWN
};

enum CameraMode {
  CAMERA_OFF,
  CAMERA_PREVIEW_JPEG,
  CAMERA_CAPTURE_JPEG
};

struct ButtonEvent {
  ButtonEventType type;
};

struct PhotoEntry {
  char path[PHOTO_PATH_LEN];
  uint16_t number;
};

AppState appState = STATE_BOOT;
CameraMode cameraMode = CAMERA_OFF;

bool sistemaLigado = true;
bool cameraLigada = false;
bool sdAvailable = false;
bool sdMounted = false;
bool flashEnabled = false;
bool psramAvailable = false;
bool uiDirty = true;
bool bootFinished = false;
bool buttonStuckWarning = false;
bool buttonCalibrationReady = false;

uint16_t buttonAdcIdle = 4095;
uint16_t buttonAdcUp = 2000;
uint16_t buttonAdcDown = 500;
uint16_t buttonAdcOk = 0;

uint16_t nextPhotoNumber = 1;
PhotoEntry photos[MAX_PHOTOS];
uint16_t photoCount = 0;
uint16_t galleryIndex = 0;

uint8_t mainMenuIndex = 0;
uint8_t previousMainMenuIndex = 0;
uint8_t settingsIndex = 0;
uint8_t previousSettingsIndex = 0;
uint8_t sdInfoIndex = 1;
uint8_t confirmIndex = 1;

unsigned long lastCameraFrameMs = 0;
unsigned long lastUiFrameMs = 0;
unsigned long menuAnimStartMs = 0;
unsigned long messageUntilMs = 0;
unsigned long lastButtonChangeMs = 0;
unsigned long buttonPressStartMs = 0;
unsigned long lastRepeatMs = 0;

ButtonType stableButton = BTN_NONE;
ButtonType lastRawButton = BTN_NONE;
bool longEventSent = false;
bool powerEventSent = false;

int16_t jpegViewportX = 0;
int16_t jpegViewportY = 0;
int16_t jpegViewportW = SCREEN_W;
int16_t jpegViewportH = SCREEN_H;
int16_t jpegDrawX = 0;
int16_t jpegDrawY = 0;
int16_t jpegDecodedW = 0;
int16_t jpegDecodedH = 0;
bool captureProfileCached = false;
framesize_t cachedCaptureFrameSize = FRAMESIZE_SVGA;
bool cachedCaptureUsesPsram = false;
uint8_t cachedCaptureFbCount = 1;
framesize_t activeSensorFrameSize = CAMERA_PREVIEW_FRAME_SIZE;

const unsigned long debounceDelay = 80;
const unsigned long repeatDelayMs = 650;
const unsigned long repeatIntervalMs = 240;
const unsigned long backHoldMs = 900;
const unsigned long tempoSegurarPower = 3000;
const unsigned long stuckButtonMs = 8500;
const unsigned long cameraFrameIntervalMs = 45;
const unsigned long menuAnimMs = 130;

const char *mainMenuItems[] = {"Camera", "Configuracoes", "Galeria", "Desligar"};
const char *settingsItems[] = {"Cartao SD", "Flash", "Sobre", "Voltar"};

// Prototipos principais
void initializeDisplay();
bool initializeCamera(CameraMode mode);
bool iniciarCamera();
void desligarCamera();
void initializeSDCard();
void prepareSharedPinsForSD();
void handleButtons();
void updateInterface();
void drawBootAnimation();
void mostrarAnimacaoInicial();
void drawCameraScreen();
void drawMainMenu();
void drawSettingsMenu();
void drawSDInfo();
void drawGallery();
void drawPowerConfirmation();
bool captureAndSavePhoto();
void loadPhotoList();
void showPhoto(int index);
void enterDeepSleep();
void loadOrCalibrateButtons();

uint16_t swapRB(uint16_t color) {
  uint8_t r = (color >> 11) & 0x1F;
  uint8_t g = (color >> 5) & 0x3F;
  uint8_t b = color & 0x1F;
  return (b << 11) | (g << 5) | r;
}

void copyText(char *dst, const char *src, size_t dstSize) {
  if (dstSize == 0) return;
  size_t i = 0;
  while (src[i] && i < dstSize - 1) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = 0;
}

bool equalsIgnoreCaseChar(char a, char b) {
  if (a >= 'A' && a <= 'Z') a = a - 'A' + 'a';
  if (b >= 'A' && b <= 'Z') b = b - 'A' + 'a';
  return a == b;
}

bool endsWithExtension(const char *name, const char *extension) {
  size_t len = strlen(name);
  size_t extLen = strlen(extension);
  if (len < extLen) return false;

  const char *tail = name + len - extLen;
  for (size_t i = 0; i < extLen; i++) {
    if (!equalsIgnoreCaseChar(tail[i], extension[i])) return false;
  }
  return true;
}

bool endsWithJpg(const char *name) {
  return endsWithExtension(name, ".jpg") || endsWithExtension(name, ".jpeg");
}

bool endsWithBmp(const char *name) {
  return endsWithExtension(name, ".bmp");
}

bool endsWithSupportedPhoto(const char *name) {
  return endsWithJpg(name) || endsWithBmp(name);
}

uint16_t parsePhotoNumber(const char *name) {
  const char *prefix = strstr(name, PHOTO_PREFIX);
  if (!prefix) return 0;
  prefix += strlen(PHOTO_PREFIX);

  uint16_t value = 0;
  uint8_t digits = 0;
  while (*prefix >= '0' && *prefix <= '9' && digits < 5) {
    value = (uint16_t)(value * 10 + (*prefix - '0'));
    prefix++;
    digits++;
  }
  return digits > 0 ? value : 0;
}

void buildPhotoPath(uint16_t number, char *out, size_t outSize) {
  snprintf(out, outSize, DCIM_DIR "/PHOTO_%04u%s", number, PHOTO_EXTENSION);
}

void normalizePhotoPath(const char *name, char *out, size_t outSize) {
  if (!name || !name[0]) {
    out[0] = 0;
    return;
  }

  if (name[0] == '/') {
    copyText(out, name, outSize);
    return;
  }

  const char *baseName = strrchr(name, '/');
  baseName = baseName ? baseName + 1 : name;
  snprintf(out, outSize, DCIM_DIR "/%s", baseName);
}

void logHeap(const char *label) {
  Serial.printf("[%s] heap=%u psram=%u\n", label, ESP.getFreeHeap(), ESP.getFreePsram());
}

void backlightOn() {
  gpio_hold_dis((gpio_num_t)TFT_BL);
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  gpio_set_level((gpio_num_t)TFT_BL, 1);
}

void backlightOff() {
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, LOW);
  gpio_set_level((gpio_num_t)TFT_BL, 0);
}

void restoreDisplayBus() {
  pinMode(TFT_CS, OUTPUT);
  pinMode(TFT_DC, OUTPUT);
  pinMode(TFT_RST, OUTPUT);
  digitalWrite(TFT_CS, HIGH);
  vspi.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.setSPISpeed(40000000);
  tft.setRotation(TFT_APP_ROTATION);
  tft.enableDisplay(true);
  pinMode(BUTTON_ADC_PIN, INPUT);
}

void prepareSharedPinsForSD() {
  digitalWrite(TFT_CS, HIGH);
  vspi.end();

  // GPIO14/15/2 pertencem ao SD interno em modo 1-bit. Como tambem sao usados
  // pelo TFT/botoes, deixe-os livres antes de chamar SD_MMC.begin().
  pinMode(TFT_SCLK, INPUT_PULLUP);
  pinMode(TFT_DC, INPUT_PULLUP);
  pinMode(BUTTON_ADC_PIN, INPUT_PULLUP);
  delay(20);
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

void drawBootAnimation() {
  mostrarAnimacaoInicial();
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

  tft.setRotation(TFT_APP_ROTATION);
  tft.fillScreen(ST77XX_BLACK);
}

void centerText(const char *text, int16_t y, uint16_t color, uint8_t size) {
  int16_t x1, y1;
  uint16_t w, h;
  tft.setTextSize(size);
  tft.getTextBounds(text, 0, y, &x1, &y1, &w, &h);
  int16_t x = (tft.width() - w) / 2;
  if (x < 0) x = 0;
  tft.setCursor(x, y);
  tft.setTextColor(color);
  tft.print(text);
}

void drawHeader(const char *title) {
  tft.fillRect(0, 0, SCREEN_W, 18, COLOR_BG);
  tft.drawFastHLine(0, 18, SCREEN_W, COLOR_PANEL_2);
  centerText(title, 5, COLOR_TEXT, 1);
}

void showStatus(const char *msg, uint16_t color = COLOR_TEXT, unsigned long durationMs = 900) {
  tft.fillRect(0, 0, SCREEN_W, 18, COLOR_BG);
  tft.drawFastHLine(0, 18, SCREEN_W, COLOR_PANEL_2);
  centerText(msg, 5, color, 1);
  messageUntilMs = millis() + durationMs;
}

void mostrarMensagem(const char *msg) {
  showStatus(msg, COLOR_TEXT, 900);
}

void showCenteredMessage(const char *title, const char *subtitle, uint16_t color) {
  tft.fillScreen(COLOR_BG);
  drawHeader(PROJECT_NAME);
  centerText(title, 34, color, 1);
  if (subtitle && subtitle[0]) {
    centerText(subtitle, 50, COLOR_MUTED, 1);
  }
}

void mostrarContadorPower(const char *acao, int segundos) {
  tft.fillScreen(COLOR_BG);
  centerText(acao, 22, COLOR_TEXT, 1);
  char numberText[8];
  snprintf(numberText, sizeof(numberText), "%d", segundos);
  centerText(numberText, 42, COLOR_WARN, 3);
}

uint16_t adcDistance(uint16_t a, uint16_t b) {
  return a > b ? a - b : b - a;
}

uint16_t readButtonAdcAverage(uint8_t samples = 6) {
  uint32_t total = 0;
  if (samples == 0) samples = 1;

  for (uint8_t i = 0; i < samples; i++) {
    total += analogRead(BUTTON_ADC_PIN);
    delay(2);
  }

  return (uint16_t)(total / samples);
}

bool buttonCalibrationIsValid() {
  if (buttonAdcIdle > 4095 || buttonAdcUp > 4095 ||
      buttonAdcDown > 4095 || buttonAdcOk > 4095) {
    return false;
  }

  if (adcDistance(buttonAdcIdle, buttonAdcUp) < BUTTON_CALIBRATION_MIN_DELTA ||
      adcDistance(buttonAdcIdle, buttonAdcDown) < BUTTON_CALIBRATION_MIN_DELTA ||
      adcDistance(buttonAdcIdle, buttonAdcOk) < BUTTON_CALIBRATION_MIN_DELTA) {
    return false;
  }

  if (adcDistance(buttonAdcUp, buttonAdcDown) < BUTTON_CALIBRATION_MIN_DELTA ||
      adcDistance(buttonAdcUp, buttonAdcOk) < BUTTON_CALIBRATION_MIN_DELTA ||
      adcDistance(buttonAdcDown, buttonAdcOk) < BUTTON_CALIBRATION_MIN_DELTA) {
    return false;
  }

  return true;
}

uint16_t buttonTolerance(uint16_t center, uint16_t otherA, uint16_t otherB) {
  uint16_t nearest = adcDistance(center, buttonAdcIdle);
  uint16_t distanceA = adcDistance(center, otherA);
  uint16_t distanceB = adcDistance(center, otherB);
  if (distanceA < nearest) nearest = distanceA;
  if (distanceB < nearest) nearest = distanceB;

  uint16_t tolerance = (nearest * 45U) / 100U;
  if (tolerance < 35) tolerance = 35;
  if (tolerance > 600) tolerance = 600;
  return tolerance;
}

ButtonType lerBotao() {
  uint16_t value = analogRead(BUTTON_ADC_PIN);

  if (!buttonCalibrationReady) {
    if (value < 100) return BTN_FOTO;
    if (value > 200 && value < 900) return BTN_DOWN;
    if (value > 900 && value < 3900) return BTN_UP;
    return BTN_NONE;
  }

  uint16_t upDistance = adcDistance(value, buttonAdcUp);
  uint16_t downDistance = adcDistance(value, buttonAdcDown);
  uint16_t okDistance = adcDistance(value, buttonAdcOk);

  ButtonType closestButton = BTN_UP;
  uint16_t closestDistance = upDistance;
  uint16_t acceptedTolerance = buttonTolerance(buttonAdcUp, buttonAdcDown, buttonAdcOk);

  if (downDistance < closestDistance) {
    closestButton = BTN_DOWN;
    closestDistance = downDistance;
    acceptedTolerance = buttonTolerance(buttonAdcDown, buttonAdcUp, buttonAdcOk);
  }

  if (okDistance < closestDistance) {
    closestButton = BTN_FOTO;
    closestDistance = okDistance;
    acceptedTolerance = buttonTolerance(buttonAdcOk, buttonAdcUp, buttonAdcDown);
  }

  return closestDistance <= acceptedTolerance ? closestButton : BTN_NONE;
}

uint16_t captureButtonForCalibration(const char *buttonName) {
  tft.fillScreen(COLOR_BG);
  drawHeader("Mapear botoes");
  centerText("Pressione", 28, COLOR_TEXT, 1);
  centerText(buttonName, 46, COLOR_ACCENT, 2);

  uint16_t candidate = buttonAdcIdle;
  while (true) {
    candidate = readButtonAdcAverage(4);
    if (adcDistance(candidate, buttonAdcIdle) >= BUTTON_CALIBRATION_MIN_DELTA) {
      delay(90);
      uint16_t confirmed = readButtonAdcAverage(BUTTON_CALIBRATION_SAMPLES);
      if (adcDistance(confirmed, buttonAdcIdle) >= BUTTON_CALIBRATION_MIN_DELTA) {
        candidate = confirmed;
        break;
      }
    }
    delay(8);
  }

  char adcText[22];
  snprintf(adcText, sizeof(adcText), "ADC: %u", candidate);
  tft.fillScreen(COLOR_BG);
  drawHeader("Mapear botoes");
  centerText("Mapeado", 26, COLOR_OK, 1);
  centerText(adcText, 42, COLOR_TEXT, 1);
  centerText("Solte o botao", 60, COLOR_MUTED, 1);

  while (adcDistance(readButtonAdcAverage(4), buttonAdcIdle) >= BUTTON_CALIBRATION_MIN_DELTA / 2) {
    delay(10);
  }
  delay(180);
  return candidate;
}

void resetButtonEventState() {
  stableButton = BTN_NONE;
  lastRawButton = BTN_NONE;
  lastButtonChangeMs = millis();
  buttonPressStartMs = 0;
  lastRepeatMs = 0;
  longEventSent = false;
  powerEventSent = false;
  buttonStuckWarning = false;
}

void runButtonCalibration() {
  buttonCalibrationReady = false;

  while (true) {
    tft.fillScreen(COLOR_BG);
    drawHeader("Primeiro uso");
    centerText("Solte os botoes", 28, COLOR_TEXT, 1);
    centerText("Calibrando...", 48, COLOR_MUTED, 1);
    delay(1500);
    buttonAdcIdle = readButtonAdcAverage(BUTTON_CALIBRATION_SAMPLES);

    buttonAdcUp = captureButtonForCalibration("PARA CIMA");
    buttonAdcDown = captureButtonForCalibration("PARA BAIXO");
    buttonAdcOk = captureButtonForCalibration("OK / FOTO");

    if (buttonCalibrationIsValid()) break;

    Serial.println("Mapeamento invalido: valores ADC repetidos ou muito proximos.");
    showCenteredMessage("Mapeamento invalido", "Tente novamente", COLOR_BAD);
    delay(1400);
  }

  preferences.putUShort("btn_idle", buttonAdcIdle);
  preferences.putUShort("btn_up", buttonAdcUp);
  preferences.putUShort("btn_down", buttonAdcDown);
  preferences.putUShort("btn_ok", buttonAdcOk);
  preferences.putUChar("btn_ver", BUTTON_CALIBRATION_VERSION);
  preferences.putString("btn_build", FIRMWARE_BUILD_ID);
  buttonCalibrationReady = true;
  resetButtonEventState();

  Serial.printf("Botoes mapeados: idle=%u up=%u down=%u ok=%u\n",
                buttonAdcIdle, buttonAdcUp, buttonAdcDown, buttonAdcOk);
  showCenteredMessage("Botoes mapeados", "Configuracao salva", COLOR_OK);
  delay(1000);
}

void loadOrCalibrateButtons() {
  uint8_t savedVersion = preferences.getUChar("btn_ver", 0);
  String savedBuildId = preferences.getString("btn_build", "");
  buttonAdcIdle = preferences.getUShort("btn_idle", 4095);
  buttonAdcUp = preferences.getUShort("btn_up", 2000);
  buttonAdcDown = preferences.getUShort("btn_down", 500);
  buttonAdcOk = preferences.getUShort("btn_ok", 0);

  bool sameFirmwareBuild = savedBuildId == FIRMWARE_BUILD_ID;
  buttonCalibrationReady = savedVersion == BUTTON_CALIBRATION_VERSION &&
                           sameFirmwareBuild && buttonCalibrationIsValid();
  if (!buttonCalibrationReady) {
    Serial.printf("Nova compilacao detectada (%s); calibrando botoes.\n", FIRMWARE_BUILD_ID);
    runButtonCalibration();
    return;
  }

  resetButtonEventState();
  Serial.printf("Mapeamento carregado: idle=%u up=%u down=%u ok=%u\n",
                buttonAdcIdle, buttonAdcUp, buttonAdcDown, buttonAdcOk);
}

ButtonEvent readButtonEvent() {
  ButtonEvent event = {EVT_NONE};
  ButtonType rawButton = lerBotao();
  unsigned long now = millis();

  if (rawButton != lastRawButton) {
    lastRawButton = rawButton;
    lastButtonChangeMs = now;
  }

  if (now - lastButtonChangeMs < debounceDelay) {
    return event;
  }

  if (rawButton != stableButton) {
    ButtonType releasedButton = stableButton;
    bool wasPressed = releasedButton != BTN_NONE;

    stableButton = rawButton;
    buttonStuckWarning = false;

    if (stableButton != BTN_NONE) {
      buttonPressStartMs = now;
      lastRepeatMs = now;
      longEventSent = false;
      powerEventSent = false;
      return event;
    }

    if (wasPressed && !longEventSent && !powerEventSent) {
      if (releasedButton == BTN_FOTO) event.type = EVT_OK_SHORT;
      else if (releasedButton == BTN_UP) event.type = EVT_UP;
      else if (releasedButton == BTN_DOWN) event.type = EVT_DOWN;
    }

    return event;
  }

  if (stableButton == BTN_NONE) return event;

  unsigned long heldMs = now - buttonPressStartMs;

  if (heldMs > stuckButtonMs && !buttonStuckWarning) {
    Serial.println("Botao pressionado por tempo excessivo; ignorando repeticoes.");
    showStatus("Solte o botao", COLOR_WARN, 1200);
    buttonStuckWarning = true;
    return event;
  }

  if (stableButton == BTN_FOTO) {
    if (!powerEventSent && appState == STATE_CAMERA && heldMs >= tempoSegurarPower) {
      powerEventSent = true;
      longEventSent = true;
      event.type = EVT_POWER_HOLD;
      return event;
    }

    if (!longEventSent && appState != STATE_CAMERA && heldMs >= backHoldMs) {
      longEventSent = true;
      event.type = EVT_OK_LONG;
      return event;
    }
  } else if (heldMs >= repeatDelayMs && now - lastRepeatMs >= repeatIntervalMs) {
    lastRepeatMs = now;
    event.type = stableButton == BTN_UP ? EVT_UP : EVT_DOWN;
    return event;
  }

  return event;
}

camera_config_t makeCameraConfig(CameraMode mode, framesize_t frameSize, bool usePsramBuffer, uint8_t fbCount) {
  camera_config_t config = {};

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

  config.frame_size = frameSize;
  config.fb_count = fbCount;
  config.fb_location = usePsramBuffer ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  config.grab_mode = fbCount > 1 ? CAMERA_GRAB_LATEST : CAMERA_GRAB_WHEN_EMPTY;
  config.pixel_format = PIXFORMAT_JPEG;
  config.jpeg_quality = CAMERA_CAPTURE_JPEG_QUALITY;

  return config;
}

void applySensorDefaults() {
  sensor_t *s = esp_camera_sensor_get();
  if (!s) return;

  s->set_vflip(s, 0);
  s->set_hmirror(s, 0);
  s->set_colorbar(s, 0);
  s->set_special_effect(s, 0);
  s->set_whitebal(s, 1);
  s->set_awb_gain(s, 1);
  s->set_wb_mode(s, 0);
  s->set_gain_ctrl(s, 1);
  s->set_exposure_ctrl(s, 1);
  s->set_aec2(s, 1);
  s->set_ae_level(s, 0);
  s->set_gainceiling(s, GAINCEILING_8X);
  s->set_brightness(s, 0);
  s->set_contrast(s, 0);
  s->set_saturation(s, 0);
  s->set_dcw(s, 1);
  s->set_bpc(s, 1);
  s->set_wpc(s, 1);
  s->set_raw_gma(s, 1);
  s->set_lenc(s, 1);
}

const char *frameSizeName(framesize_t frameSize) {
  switch (frameSize) {
    case FRAMESIZE_UXGA: return "UXGA";
    case FRAMESIZE_SXGA: return "SXGA";
    case FRAMESIZE_XGA: return "XGA";
    case FRAMESIZE_SVGA: return "SVGA";
    case FRAMESIZE_VGA: return "VGA";
    case FRAMESIZE_QVGA: return "QVGA";
    case FRAMESIZE_QQVGA: return "QQVGA";
    default: return "OUTRO";
  }
}

uint16_t frameSizeWidth(framesize_t frameSize) {
  switch (frameSize) {
    case FRAMESIZE_UXGA: return 1600;
    case FRAMESIZE_SXGA: return 1280;
    case FRAMESIZE_XGA: return 1024;
    case FRAMESIZE_SVGA: return 800;
    case FRAMESIZE_VGA: return 640;
    case FRAMESIZE_QVGA: return 320;
    case FRAMESIZE_QQVGA: return 160;
    default: return 0;
  }
}

uint16_t frameSizeHeight(framesize_t frameSize) {
  switch (frameSize) {
    case FRAMESIZE_UXGA: return 1200;
    case FRAMESIZE_SXGA: return 1024;
    case FRAMESIZE_XGA: return 768;
    case FRAMESIZE_SVGA: return 600;
    case FRAMESIZE_VGA: return 480;
    case FRAMESIZE_QVGA: return 240;
    case FRAMESIZE_QQVGA: return 120;
    default: return 0;
  }
}

bool setCameraFrameProfile(framesize_t frameSize, uint8_t jpegQuality,
                           CameraMode mode, uint16_t settleMs) {
  sensor_t *s = esp_camera_sensor_get();
  if (!s) {
    Serial.println("Sensor da camera indisponivel.");
    return false;
  }

  int frameResult = s->set_framesize(s, frameSize);
  int qualityResult = s->set_quality(s, jpegQuality);

  if (frameResult != 0 || qualityResult != 0) {
    Serial.printf("Falha ao configurar sensor: frame=%s res=%d quality=%d res=%d\n",
                  frameSizeName(frameSize), frameResult, jpegQuality, qualityResult);
    return false;
  }

  esp_camera_return_all();
  activeSensorFrameSize = frameSize;
  cameraMode = mode;

  if (settleMs > 0) {
    delay(settleMs);
  }

  Serial.printf("Sensor configurado: %s qualidade=%u modo=%s\n",
                frameSizeName(frameSize), jpegQuality,
                mode == CAMERA_CAPTURE_JPEG ? "captura" : "preview");
  return true;
}

bool initializeCamera(CameraMode mode) {
  if (mode == CAMERA_OFF) {
    desligarCamera();
    return true;
  }

  if (cameraLigada) {
    if (mode == CAMERA_CAPTURE_JPEG && cameraMode == CAMERA_PREVIEW_JPEG && captureProfileCached) {
      if (setCameraFrameProfile(cachedCaptureFrameSize, CAMERA_CAPTURE_JPEG_QUALITY,
                                CAMERA_CAPTURE_JPEG, CAPTURE_SENSOR_SETTLE_MS)) {
        return true;
      }
      Serial.println("Perfil de captura memorizado falhou; reinicializando camera.");
      captureProfileCached = false;
    } else if (mode == CAMERA_PREVIEW_JPEG && cameraMode == CAMERA_CAPTURE_JPEG) {
      return setCameraFrameProfile(CAMERA_PREVIEW_FRAME_SIZE, CAMERA_PREVIEW_JPEG_QUALITY,
                                   CAMERA_PREVIEW_JPEG, PREVIEW_SENSOR_SETTLE_MS);
    } else if (cameraMode == mode) {
      return true;
    }

    esp_camera_deinit();
    cameraLigada = false;
    cameraMode = CAMERA_OFF;
    delay(20);
  }

  digitalWrite(PWDN_GPIO_NUM, LOW);
  delay(20);

  struct CameraAttempt {
    framesize_t frameSize;
    bool usePsram;
    uint8_t fbCount;
  };

  CameraAttempt captureAttempts[] = {
#if CAPTURE_TRY_ULTRA_RES
    {FRAMESIZE_UXGA, true, 2},
    {FRAMESIZE_UXGA, true, 1},
    {FRAMESIZE_SXGA, true, 2},
    {FRAMESIZE_SXGA, true, 1},
#endif
    {FRAMESIZE_XGA, true, 2},
    {FRAMESIZE_XGA, true, 1},
    {FRAMESIZE_SVGA, true, 2},
    {FRAMESIZE_SVGA, true, 1},
    {FRAMESIZE_VGA, true, 2},
    {FRAMESIZE_VGA, true, 1},
    {FRAMESIZE_SVGA, false, 1},
    {FRAMESIZE_VGA, false, 1},
    {FRAMESIZE_QVGA, true, 1},
    {FRAMESIZE_QVGA, false, 1},
    {FRAMESIZE_QQVGA, false, 1}
  };

  CameraAttempt cachedCaptureAttempt[] = {
    {cachedCaptureFrameSize, cachedCaptureUsesPsram, cachedCaptureFbCount}
  };

  bool tryCachedFirst = captureProfileCached;

  for (uint8_t pass = 0; pass < 2; pass++) {
    CameraAttempt *attempts = captureAttempts;
    uint8_t attemptCount = sizeof(captureAttempts) / sizeof(captureAttempts[0]);
    bool usingCachedAttempt = false;

    if (pass == 0 && tryCachedFirst) {
      attempts = cachedCaptureAttempt;
      attemptCount = sizeof(cachedCaptureAttempt) / sizeof(cachedCaptureAttempt[0]);
      usingCachedAttempt = true;
      Serial.printf("Usando perfil de captura memorizado: %s buffer=%s fb=%u\n",
                    frameSizeName(cachedCaptureFrameSize),
                    cachedCaptureUsesPsram ? "PSRAM" : "DRAM",
                    cachedCaptureFbCount);
    } else if (pass == 1 && !tryCachedFirst) {
      break;
    }

    for (uint8_t i = 0; i < attemptCount; i++) {
      if (attempts[i].usePsram && !psramAvailable) continue;

      camera_config_t config = makeCameraConfig(mode, attempts[i].frameSize,
                                                attempts[i].usePsram, attempts[i].fbCount);
      Serial.printf("Tentando camera JPEG buffer max=%s memoria=%s fb=%u\n",
                    frameSizeName(attempts[i].frameSize),
                    attempts[i].usePsram ? "PSRAM" : "DRAM",
                    attempts[i].fbCount);

      esp_err_t err = esp_camera_init(&config);

      if (err == ESP_OK) {
        applySensorDefaults();
        cameraLigada = true;
        captureProfileCached = true;
        cachedCaptureFrameSize = attempts[i].frameSize;
        cachedCaptureUsesPsram = attempts[i].usePsram;
        cachedCaptureFbCount = attempts[i].fbCount;

        bool profileOk = false;
        if (mode == CAMERA_PREVIEW_JPEG) {
          profileOk = setCameraFrameProfile(CAMERA_PREVIEW_FRAME_SIZE, CAMERA_PREVIEW_JPEG_QUALITY,
                                            CAMERA_PREVIEW_JPEG, PREVIEW_SENSOR_SETTLE_MS);
        } else {
          profileOk = setCameraFrameProfile(cachedCaptureFrameSize, CAMERA_CAPTURE_JPEG_QUALITY,
                                            CAMERA_CAPTURE_JPEG, CAPTURE_SENSOR_SETTLE_MS);
        }

        if (!profileOk) {
          esp_camera_deinit();
          cameraLigada = false;
          cameraMode = CAMERA_OFF;
          captureProfileCached = false;
          continue;
        }

        Serial.printf("Camera inicializada em JPEG. Buffer max=%s memoria=%s fb=%u\n",
                      frameSizeName(attempts[i].frameSize),
                      attempts[i].usePsram ? "PSRAM" : "DRAM",
                      attempts[i].fbCount);
        return true;
      }

      Serial.printf("Falha ao inicializar camera JPEG %s: 0x%x\n",
                    frameSizeName(attempts[i].frameSize), err);
      esp_camera_deinit();

      digitalWrite(PWDN_GPIO_NUM, HIGH);
      delay(80);
      digitalWrite(PWDN_GPIO_NUM, LOW);
      delay(80);
    }

    if (usingCachedAttempt) {
      captureProfileCached = false;
      Serial.println("Perfil memorizado falhou; tentando outras resolucoes agora.");
    }
  }

  cameraLigada = false;
  cameraMode = CAMERA_OFF;
  Serial.println("Falha ao inicializar camera em todas as resolucoes.");
  return false;
}

bool iniciarCamera() {
  return initializeCamera(CAMERA_PREVIEW_JPEG);
}

void desligarCamera() {
  if (cameraLigada) {
    esp_camera_deinit();
  }

  cameraLigada = false;
  cameraMode = CAMERA_OFF;
  digitalWrite(PWDN_GPIO_NUM, HIGH);
}

bool beginSDSession(bool quiet = false) {
  if (sdMounted) return true;

  prepareSharedPinsForSD();

  bool pinsOk = SD_MMC.setPins(SD_MMC_CLK_PIN, SD_MMC_CMD_PIN, SD_MMC_D0_PIN);
  if (!pinsOk) {
    if (!quiet) Serial.println("Falha ao configurar pinos SD_MMC.");
    sdAvailable = false;
    restoreDisplayBus();
    return false;
  }

  bool mounted = SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT, 5);
  if (!mounted) {
    if (!quiet) Serial.println("Cartao SD nao encontrado ou falha de montagem.");
    sdAvailable = false;
    SD_MMC.end();
    restoreDisplayBus();
    return false;
  }

  if (SD_MMC.cardType() == CARD_NONE) {
    if (!quiet) Serial.println("Nenhum cartao SD detectado.");
    sdAvailable = false;
    SD_MMC.end();
    restoreDisplayBus();
    return false;
  }

  sdMounted = true;
  sdAvailable = true;
  return true;
}

void endSDSession() {
  if (sdMounted) {
    SD_MMC.end();
    sdMounted = false;
  }
  restoreDisplayBus();
}

bool ensureDCIMFolder() {
  if (!sdMounted) return false;

  if (!SD_MMC.exists(DCIM_DIR)) {
    Serial.println("Criando pasta /DCIM");
    if (!SD_MMC.mkdir(DCIM_DIR)) {
      Serial.println("Falha ao criar /DCIM");
      return false;
    }
  }

  return true;
}

void scanNextPhotoNumber() {
  if (!sdMounted) return;

  uint16_t highest = 0;
  fs::File dir = SD_MMC.open(DCIM_DIR);
  if (!dir || !dir.isDirectory()) {
    nextPhotoNumber = 1;
    return;
  }

  fs::File file = dir.openNextFile();
  while (file) {
    if (!file.isDirectory() && endsWithSupportedPhoto(file.name())) {
      uint16_t number = parsePhotoNumber(file.name());
      if (number > highest) highest = number;
    }
    file.close();
    file = dir.openNextFile();
  }

  dir.close();
  nextPhotoNumber = highest + 1;
  if (nextPhotoNumber == 0) nextPhotoNumber = 1;
  Serial.printf("Proxima foto: %u\n", nextPhotoNumber);
}

uint16_t countPhotosOnMountedSD() {
  if (!sdMounted) return 0;

  uint16_t count = 0;
  fs::File dir = SD_MMC.open(DCIM_DIR);
  if (!dir || !dir.isDirectory()) return 0;

  fs::File file = dir.openNextFile();
  while (file) {
    if (!file.isDirectory() && endsWithSupportedPhoto(file.name())) count++;
    file.close();
    file = dir.openNextFile();
    yield();
  }

  dir.close();
  return count;
}

void initializeSDCard() {
  sdAvailable = false;
  sdMounted = false;
  nextPhotoNumber = 1;

#if CHECK_SD_ON_BOOT
  showStatus("Verificando SD", COLOR_TEXT, 500);
  Serial.println("Inicializando cartao SD em modo SD_MMC 1-bit.");

  if (beginSDSession(true)) {
    uint64_t total = SD_MMC.totalBytes();
    uint64_t used = SD_MMC.usedBytes();
    Serial.printf("SD montado. Total=%llu Usado=%llu Tipo=%d\n", total, used, SD_MMC.cardType());

    if (ensureDCIMFolder()) {
      scanNextPhotoNumber();
      showStatus("SD pronto", COLOR_OK, 600);
    } else {
      sdAvailable = false;
      showStatus("Erro /DCIM", COLOR_BAD, 900);
    }

    endSDSession();
    return;
  }

  Serial.println("SD indisponivel no boot; a interface continuara funcionando.");
  showStatus("SD ausente", COLOR_WARN, 700);
#else
  Serial.println("Verificacao do SD adiada: a camera inicia sem montar o cartao.");
#endif
}

const char *cardTypeText(uint8_t cardType) {
  switch (cardType) {
    case CARD_MMC: return "MMC";
    case CARD_SD: return "SDSC";
    case CARD_SDHC: return "SDHC";
    default: return "N/D";
  }
}

void formatBytes(uint64_t bytes, char *out, size_t outSize) {
  if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
    float gb = (float)bytes / (1024.0f * 1024.0f * 1024.0f);
    snprintf(out, outSize, "%.2f GB", gb);
  } else if (bytes >= 1024ULL * 1024ULL) {
    float mb = (float)bytes / (1024.0f * 1024.0f);
    snprintf(out, outSize, "%.1f MB", mb);
  } else if (bytes >= 1024ULL) {
    float kb = (float)bytes / 1024.0f;
    snprintf(out, outSize, "%.0f KB", kb);
  } else {
    snprintf(out, outSize, "%llu B", bytes);
  }
}

bool deleteRecursive(const char *path, bool removeSelf) {
  fs::File root = SD_MMC.open(path);
  if (!root) {
    Serial.printf("Nao abriu para remover: %s\n", path);
    return false;
  }

  bool ok = true;

  if (!root.isDirectory()) {
    root.close();
    return SD_MMC.remove(path);
  }

  fs::File file = root.openNextFile();
  while (file) {
    char childPath[96];
    const char *name = file.name();
    if (name[0] == '/') {
      copyText(childPath, name, sizeof(childPath));
    } else {
      snprintf(childPath, sizeof(childPath), "%s/%s", path, name);
    }

    bool isDir = file.isDirectory();
    file.close();

    if (isDir) {
      if (!deleteRecursive(childPath, true)) ok = false;
    } else {
      Serial.printf("Removendo arquivo %s\n", childPath);
      if (!SD_MMC.remove(childPath)) ok = false;
    }

    file = root.openNextFile();
    yield();
  }

  root.close();

  if (removeSelf && strcmp(path, "/") != 0) {
    Serial.printf("Removendo pasta %s\n", path);
    if (!SD_MMC.rmdir(path)) ok = false;
  }

  return ok;
}

bool formatSDCard() {
  showCenteredMessage("Formatando...", "Aguarde", COLOR_WARN);
  Serial.println("Apagando conteudo do cartao SD.");

  if (!beginSDSession()) {
    showCenteredMessage("SD nao encontrado", "", COLOR_BAD);
    return false;
  }

  bool ok = deleteRecursive("/", false);
  if (!SD_MMC.exists(DCIM_DIR)) {
    ok = SD_MMC.mkdir(DCIM_DIR) && ok;
  }

  nextPhotoNumber = 1;
  photoCount = 0;

  endSDSession();
  showCenteredMessage(ok ? "SD formatado" : "Erro ao formatar", ok ? "/DCIM recriado" : "Veja o Serial", ok ? COLOR_OK : COLOR_BAD);
  delay(650);
  return ok;
}

void sortPhotos() {
  for (uint16_t i = 0; i < photoCount; i++) {
    for (uint16_t j = i + 1; j < photoCount; j++) {
      if (photos[j].number < photos[i].number) {
        PhotoEntry temp = photos[i];
        photos[i] = photos[j];
        photos[j] = temp;
      }
    }
  }
}

void loadPhotoList() {
  photoCount = 0;
  galleryIndex = 0;

  if (!beginSDSession(true)) {
    sdAvailable = false;
    Serial.println("Galeria: SD nao disponivel.");
    return;
  }

  if (!ensureDCIMFolder()) {
    endSDSession();
    sdAvailable = false;
    return;
  }

  fs::File dir = SD_MMC.open(DCIM_DIR);
  if (!dir || !dir.isDirectory()) {
    endSDSession();
    return;
  }

  fs::File file = dir.openNextFile();
  while (file && photoCount < MAX_PHOTOS) {
    if (!file.isDirectory() && endsWithSupportedPhoto(file.name())) {
      normalizePhotoPath(file.name(), photos[photoCount].path, PHOTO_PATH_LEN);

      photos[photoCount].number = parsePhotoNumber(file.name());
      if (photos[photoCount].number == 0) photos[photoCount].number = photoCount + 1;
      photoCount++;
    }
    file.close();
    file = dir.openNextFile();
  }

  dir.close();
  sortPhotos();
  scanNextPhotoNumber();
  endSDSession();

  Serial.printf("Galeria carregada: %u foto(s)\n", photoCount);
}

void *allocImageBuffer(size_t size) {
  void *ptr = nullptr;
  if (psramAvailable) {
    ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (!ptr) {
    ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
  }
  return ptr;
}

bool tftJpegOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
#if CAMERA_ROTATE_90_CW
  int16_t viewXEnd = jpegViewportX + jpegViewportW;
  int16_t viewYEnd = jpegViewportY + jpegViewportH;

  // Cada linha horizontal decodificada vira uma coluna vertical. A formula
  // (x, y) -> (altura - 1 - y, x) corresponde a 90 graus no sentido horario.
  for (uint16_t row = 0; row < h; row++) {
    int16_t sourceY = y + row;
    int16_t destinationX = jpegDrawX + jpegDecodedH - 1 - sourceY;
    int16_t destinationY = jpegDrawY + x;
    int16_t firstPixel = 0;
    int16_t pixelCount = w;

    if (destinationX < jpegViewportX || destinationX >= viewXEnd) continue;

    if (destinationY < jpegViewportY) {
      firstPixel = jpegViewportY - destinationY;
      pixelCount -= firstPixel;
      destinationY = jpegViewportY;
    }
    if (destinationY + pixelCount > viewYEnd) {
      pixelCount = viewYEnd - destinationY;
    }
    if (pixelCount <= 0) continue;

    uint16_t *src = bitmap + row * w + firstPixel;
    tft.drawRGBBitmap(destinationX, destinationY, src, 1, pixelCount);
  }

  return true;
#else
  int16_t clipX0 = x > jpegViewportX ? x : jpegViewportX;
  int16_t clipY0 = y > jpegViewportY ? y : jpegViewportY;
  int16_t xEnd = x + (int16_t)w;
  int16_t yEnd = y + (int16_t)h;
  int16_t viewXEnd = jpegViewportX + jpegViewportW;
  int16_t viewYEnd = jpegViewportY + jpegViewportH;
  int16_t clipX1 = xEnd < viewXEnd ? xEnd : viewXEnd;
  int16_t clipY1 = yEnd < viewYEnd ? yEnd : viewYEnd;

  if (clipX0 >= clipX1 || clipY0 >= clipY1) return true;

  for (int16_t row = clipY0; row < clipY1; row++) {
    uint16_t *src = bitmap + (row - y) * w + (clipX0 - x);
    uint16_t drawW = clipX1 - clipX0;

    for (uint16_t col = 0; col < drawW; col++) {
      lineBuffer[col] = src[col];
    }

    tft.drawRGBBitmap(clipX0, row, lineBuffer, drawW, 1);
  }

  return true;
#endif
}

bool drawJpegBufferToViewport(const uint8_t *jpegBuffer, size_t fileSize,
                               int16_t areaX, int16_t areaY, int16_t areaW, int16_t areaH,
                               bool fillViewport, bool clearArea) {
  if (!jpegBuffer || fileSize == 0) return false;

  uint16_t jpgW = 0;
  uint16_t jpgH = 0;
  JRESULT sizeResult = TJpgDec.getJpgSize(&jpgW, &jpgH, jpegBuffer, fileSize);
  if (sizeResult != JDR_OK || jpgW == 0 || jpgH == 0) {
    Serial.println("Falha ao obter tamanho do JPEG.");
    return false;
  }

  uint8_t scale = 1;
  if (fillViewport) {
    // TJpg_Decoder aceita apenas escalas 1, 2, 4 e 8. Escolhe a maior
    // reducao que ainda cobre toda a area; o excedente e recortado no centro.
    while (scale < 8) {
      uint8_t nextScale = scale * 2;
#if CAMERA_ROTATE_90_CW
      int16_t nextDrawW = jpgH / nextScale;
      int16_t nextDrawH = jpgW / nextScale;
#else
      int16_t nextDrawW = jpgW / nextScale;
      int16_t nextDrawH = jpgH / nextScale;
#endif
      if (nextDrawW < areaW || nextDrawH < areaH) break;
      scale = nextScale;
    }
  } else {
#if CAMERA_ROTATE_90_CW
    while ((jpgH / scale > areaW || jpgW / scale > areaH) && scale < 8) {
      scale *= 2;
    }
#else
    while ((jpgW / scale > areaW || jpgH / scale > areaH * 2) && scale < 8) {
      scale *= 2;
    }
    while (jpgW / scale > areaW && scale < 8) {
      scale *= 2;
    }
#endif
  }

  jpegDecodedW = jpgW / scale;
  jpegDecodedH = jpgH / scale;
#if CAMERA_ROTATE_90_CW
  int16_t drawW = jpegDecodedH;
  int16_t drawH = jpegDecodedW;
#else
  int16_t drawW = jpegDecodedW;
  int16_t drawH = jpegDecodedH;
#endif
  jpegDrawX = areaX + (areaW - drawW) / 2;
  jpegDrawY = areaY + (areaH - drawH) / 2;

  jpegViewportX = areaX;
  jpegViewportY = areaY;
  jpegViewportW = areaW;
  jpegViewportH = areaH;

  if (clearArea) {
    tft.fillRect(areaX, areaY, areaW, areaH, COLOR_BG);
  }

  TJpgDec.setJpgScale(scale);
  TJpgDec.setSwapBytes(false);
  TJpgDec.setCallback(tftJpegOutput);

#if CAMERA_ROTATE_90_CW
  JRESULT drawResult = TJpgDec.drawJpg(0, 0, jpegBuffer, fileSize);
#else
  JRESULT drawResult = TJpgDec.drawJpg(jpegDrawX, jpegDrawY, jpegBuffer, fileSize);
#endif
  if (drawResult != JDR_OK) {
    Serial.printf("Falha ao desenhar JPEG: %d\n", drawResult);
    return false;
  }

  return true;
}

bool drawBmpFromBuffer(const uint8_t *bmpBuffer, size_t fileSize, const char *path, int index) {
  if (!bmpBuffer || fileSize < BMP_HEADER_SIZE || bmpBuffer[0] != 'B' || bmpBuffer[1] != 'M') {
    showCenteredMessage("BMP invalido", "", COLOR_BAD);
    return false;
  }

  uint32_t pixelOffset = bmpBuffer[10] | (bmpBuffer[11] << 8) | (bmpBuffer[12] << 16) | (bmpBuffer[13] << 24);
  int32_t bmpW = (int32_t)(bmpBuffer[18] | (bmpBuffer[19] << 8) | (bmpBuffer[20] << 16) | (bmpBuffer[21] << 24));
  int32_t bmpH = (int32_t)(bmpBuffer[22] | (bmpBuffer[23] << 8) | (bmpBuffer[24] << 16) | (bmpBuffer[25] << 24));
  uint16_t planes = bmpBuffer[26] | (bmpBuffer[27] << 8);
  uint16_t bpp = bmpBuffer[28] | (bmpBuffer[29] << 8);
  uint32_t compression = bmpBuffer[30] | (bmpBuffer[31] << 8) | (bmpBuffer[32] << 16) | (bmpBuffer[33] << 24);

  if (bmpW <= 0 || bmpH == 0 || planes != 1 || bpp != 24 || compression != 0) {
    Serial.printf("BMP nao suportado: w=%ld h=%ld bpp=%u comp=%lu\n", (long)bmpW, (long)bmpH, bpp, (unsigned long)compression);
    showCenteredMessage("BMP nao suport.", "", COLOR_BAD);
    return false;
  }

  bool topDown = bmpH < 0;
  int32_t absH = topDown ? -bmpH : bmpH;
  uint32_t rowSize = ((bmpW * 3 + 3) / 4) * 4;
  if (pixelOffset + rowSize * absH > fileSize) {
    showCenteredMessage("BMP cortado", "", COLOR_BAD);
    return false;
  }

  tft.fillScreen(COLOR_BG);
  drawHeader("Galeria");

  int16_t targetW = SCREEN_W;
  int16_t targetH = GALLERY_INFO_Y;
  int16_t drawH = absH < targetH ? absH : targetH;
  int16_t yOffset = (targetH - drawH) / 2;

  for (int16_t y = 0; y < drawH; y++) {
    int32_t srcY = (int32_t)y * absH / drawH;
    int32_t fileY = topDown ? srcY : (absH - 1 - srcY);
    const uint8_t *row = bmpBuffer + pixelOffset + fileY * rowSize;

    for (int16_t x = 0; x < targetW; x++) {
      int32_t srcX = (int32_t)x * bmpW / targetW;
      const uint8_t *px = row + srcX * 3;
      uint8_t b = px[0];
      uint8_t g = px[1];
      uint8_t r = px[2];
      lineBuffer[x] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    }

    tft.drawRGBBitmap(0, yOffset + y, lineBuffer, targetW, 1);
    yield();
  }

  tft.fillRect(0, GALLERY_INFO_Y, SCREEN_W, SCREEN_H - GALLERY_INFO_Y, COLOR_BG);
  tft.drawFastHLine(0, GALLERY_INFO_Y - 1, SCREEN_W, COLOR_PANEL_2);
  tft.setTextSize(1);
  tft.setTextColor(COLOR_TEXT);
  tft.setCursor(3, GALLERY_INFO_Y + 2);
  const char *baseName = strrchr(path, '/');
  tft.print(baseName ? baseName + 1 : path);

  char counter[20];
  snprintf(counter, sizeof(counter), "Foto %u de %u", index + 1, photoCount);
  tft.setCursor(93, GALLERY_INFO_Y + 2);
  tft.setTextColor(COLOR_MUTED);
  tft.print(counter);

  return true;
}

void showPhoto(int index) {
  if (!sdAvailable) {
    showCenteredMessage("SD nao encontrado", "", COLOR_WARN);
    return;
  }

  if (photoCount == 0) {
    showCenteredMessage("Nenhuma foto", "salva", COLOR_MUTED);
    return;
  }

  if (index < 0 || index >= photoCount) return;

  char path[PHOTO_PATH_LEN];
  copyText(path, photos[index].path, sizeof(path));

  if (!beginSDSession()) {
    showCenteredMessage("SD nao encontrado", "", COLOR_WARN);
    return;
  }

  fs::File file = SD_MMC.open(path, FILE_READ);
  if (!file) {
    endSDSession();
    showCenteredMessage("Erro ao abrir", "imagem", COLOR_BAD);
    return;
  }

  size_t fileSize = file.size();
  if (fileSize == 0 || fileSize > MAX_GALLERY_JPEG_BYTES) {
    file.close();
    endSDSession();
    showCenteredMessage("JPEG invalido", "", COLOR_BAD);
    return;
  }

  uint8_t *jpegBuffer = (uint8_t *)allocImageBuffer(fileSize);
  if (!jpegBuffer) {
    file.close();
    endSDSession();
    showCenteredMessage("Memoria baixa", "", COLOR_BAD);
    return;
  }

  size_t readBytes = file.read(jpegBuffer, fileSize);
  file.close();
  endSDSession();

  if (readBytes != fileSize) {
    heap_caps_free(jpegBuffer);
    showCenteredMessage("Falha leitura", "JPEG", COLOR_BAD);
    return;
  }

  if (endsWithBmp(path)) {
    drawBmpFromBuffer(jpegBuffer, fileSize, path, index);
    heap_caps_free(jpegBuffer);
    return;
  }

  tft.fillScreen(COLOR_BG);
  bool drawResult = drawJpegBufferToViewport(jpegBuffer, fileSize, 0, 0, SCREEN_W, SCREEN_H,
                                             false, false);
  heap_caps_free(jpegBuffer);

  if (!drawResult) {
    showCenteredMessage("Falha JPEG", "", COLOR_BAD);
    return;
  }

  tft.fillRect(0, GALLERY_INFO_Y, SCREEN_W, SCREEN_H - GALLERY_INFO_Y, COLOR_BG);
  tft.drawFastHLine(0, GALLERY_INFO_Y - 1, SCREEN_W, COLOR_PANEL_2);
  tft.setTextSize(1);
  tft.setTextColor(COLOR_TEXT);
  tft.setCursor(3, GALLERY_INFO_Y + 2);
  const char *baseName = strrchr(path, '/');
  tft.print(baseName ? baseName + 1 : path);

  char counter[20];
  snprintf(counter, sizeof(counter), "Foto %u de %u", index + 1, photoCount);
  tft.setCursor(93, GALLERY_INFO_Y + 2);
  tft.setTextColor(COLOR_MUTED);
  tft.print(counter);
}

void drawIcon(uint8_t icon, int16_t x, int16_t y, uint16_t color) {
  switch (icon) {
    case 0:
      tft.drawRoundRect(x, y + 3, 14, 10, 2, color);
      tft.drawCircle(x + 7, y + 8, 3, color);
      tft.drawFastHLine(x + 3, y + 1, 6, color);
      break;
    case 1:
      tft.drawCircle(x + 7, y + 7, 4, color);
      tft.drawPixel(x + 7, y, color);
      tft.drawPixel(x + 7, y + 14, color);
      tft.drawPixel(x, y + 7, color);
      tft.drawPixel(x + 14, y + 7, color);
      break;
    case 2:
      tft.drawRect(x + 2, y + 1, 11, 8, color);
      tft.drawRect(x, y + 5, 11, 8, color);
      tft.drawPixel(x + 3, y + 10, color);
      break;
    default:
      tft.drawCircle(x + 7, y + 7, 6, color);
      tft.drawFastVLine(x + 7, y + 1, 7, color);
      break;
  }
}

int16_t animatedRowY(uint8_t current, uint8_t previous) {
  int16_t targetY = 18 + current * 14;
  if (current == previous || millis() - menuAnimStartMs >= menuAnimMs) {
    return targetY;
  }

  int16_t startY = 18 + previous * 14;
  unsigned long elapsed = millis() - menuAnimStartMs;
  return startY + ((targetY - startY) * (int16_t)elapsed) / (int16_t)menuAnimMs;
}

void drawListMenu(const char *title, const char **items, uint8_t count, uint8_t selected, uint8_t previous, bool mainMenu) {
  tft.fillScreen(COLOR_BG);
  drawHeader(title);

  int16_t highlightY = animatedRowY(selected, previous);
  tft.fillRoundRect(4, highlightY - 2, 152, 13, 4, COLOR_PANEL_2);
  tft.drawRoundRect(4, highlightY - 2, 152, 13, 4, COLOR_ACCENT);

  for (uint8_t i = 0; i < count; i++) {
    int16_t y = 18 + i * 14;
    bool isSelected = i == selected;
    uint16_t color = isSelected ? COLOR_TEXT : COLOR_MUTED;

    if (mainMenu) {
      drawIcon(i, 10, y - 2, isSelected ? COLOR_ACCENT : COLOR_MUTED);
      tft.setCursor(32, y + 1);
    } else {
      tft.fillCircle(16, y + 4, isSelected ? 3 : 2, isSelected ? COLOR_ACCENT : COLOR_MUTED);
      tft.setCursor(28, y + 1);
    }

    tft.setTextSize(1);
    tft.setTextColor(color);
    tft.print(items[i]);
  }

  tft.fillRect(136, 20, 4, 48, COLOR_PANEL);
  int16_t posY = 20 + (selected * 48) / count;
  tft.fillRect(136, posY, 4, 10, COLOR_ACCENT);
}

void drawCameraScreen() {
  tft.fillScreen(COLOR_BG);
  tft.drawRoundRect(0, 0, SCREEN_W, SCREEN_H, 3, COLOR_PANEL_2);
  tft.setTextSize(1);
  tft.setTextColor(COLOR_TEXT, COLOR_BG);
  tft.setCursor(3, 3);
  tft.print("Camera");
  tft.setTextColor(COLOR_MUTED, COLOR_BG);
  tft.setCursor(92, 3);
  tft.print("UP/DOWN");
  tft.setCursor(3, 69);
  tft.print(flashEnabled ? "Flash ON" : "Flash OFF");
}

void drawMainMenu() {
  drawListMenu("Menu", mainMenuItems, 4, mainMenuIndex, previousMainMenuIndex, true);
}

void drawSettingsMenu() {
  drawListMenu("Configuracoes", settingsItems, 4, settingsIndex, previousSettingsIndex, false);
}

void drawSDInfo() {
  tft.fillScreen(COLOR_BG);
  drawHeader("Cartao SD");

  if (!beginSDSession(true)) {
    tft.setTextSize(1);
    centerText("SD nao encontrado", 38, COLOR_WARN, 1);
  } else {
    uint64_t total = SD_MMC.totalBytes();
    uint64_t used = SD_MMC.usedBytes();
    uint64_t freeBytes = total > used ? total - used : 0;
    uint8_t type = SD_MMC.cardType();
    uint16_t storedPhotos = countPhotosOnMountedSD();
    scanNextPhotoNumber();

    char totalText[18];
    char usedText[18];
    char freeText[18];
    formatBytes(total, totalText, sizeof(totalText));
    formatBytes(used, usedText, sizeof(usedText));
    formatBytes(freeBytes, freeText, sizeof(freeText));

    tft.setTextSize(1);
    tft.setTextColor(COLOR_TEXT);
    tft.setCursor(4, 22);
    tft.print("Status: OK");
    tft.setCursor(4, 34);
    tft.print("Tipo: ");
    tft.print(cardTypeText(type));
    tft.setCursor(4, 46);
    tft.print("Total: ");
    tft.print(totalText);
    tft.setCursor(86, 22);
    tft.print("Usado: ");
    tft.print(usedText);
    tft.setCursor(86, 34);
    tft.print("Livre: ");
    tft.print(freeText);
    tft.setCursor(86, 46);
    tft.print("Fotos: ");
    tft.print(storedPhotos);

    endSDSession();
  }

  const char *options[] = {"Formatar", "Voltar"};
  for (uint8_t i = 0; i < 2; i++) {
    int16_t x = i == 0 ? 18 : 92;
    bool selected = i == sdInfoIndex;
    if (selected) {
      tft.fillRoundRect(x, 60, 54, 15, 4, COLOR_PANEL_2);
      tft.drawRoundRect(x, 60, 54, 15, 4, i == 0 ? COLOR_WARN : COLOR_ACCENT);
    }
    tft.setTextSize(1);
    tft.setTextColor(selected ? COLOR_TEXT : COLOR_MUTED);
    tft.setCursor(x + 4, 64);
    tft.print(options[i]);
  }
}

void drawFlashSettings() {
  tft.fillScreen(COLOR_BG);
  drawHeader("Flash");
  centerText("Flash fisico", 24, COLOR_TEXT, 1);

  tft.drawRoundRect(48, 38, 64, 24, 8, flashEnabled ? COLOR_OK : COLOR_MUTED);
  if (flashEnabled) {
    tft.fillRoundRect(82, 42, 24, 16, 6, COLOR_OK);
    centerText("ON", 47, COLOR_TEXT, 1);
  } else {
    tft.fillRoundRect(54, 42, 24, 16, 6, COLOR_PANEL_2);
    centerText("OFF", 47, COLOR_MUTED, 1);
  }

  centerText("OK alterna | segure OK volta", 68, COLOR_MUTED, 1);
}

void drawAbout() {
  tft.fillScreen(COLOR_BG);
  drawHeader("Sobre");

  tft.setTextSize(1);
  tft.setTextColor(COLOR_TEXT);
  tft.setCursor(4, 22);
  tft.print(PROJECT_NAME);
  tft.setCursor(4, 34);
  tft.print("Versao ");
  tft.print(PROJECT_VERSION);
  tft.setCursor(4, 46);
  tft.print("ESP32-CAM");
  tft.setCursor(82, 22);
  tft.print("camera digital");
  tft.setCursor(82, 34);
  tft.print("display/botoes/SD");
  tft.setTextColor(COLOR_MUTED);
  tft.setCursor(4, 58);
  tft.print(PROJECT_DEVELOPER);
  centerText("OK volta", 70, COLOR_ACCENT, 1);
}

void drawGallery() {
  if (!sdAvailable) {
    showCenteredMessage("SD nao encontrado", "", COLOR_WARN);
    return;
  }

  if (photoCount == 0) {
    showCenteredMessage("Nenhuma foto", "salva", COLOR_MUTED);
    return;
  }

  showPhoto(galleryIndex);
}

void drawPowerConfirmation() {
  tft.fillScreen(COLOR_BG);
  drawHeader("Desligar");
  centerText("Deseja desligar?", 30, COLOR_TEXT, 1);

  const char *options[] = {"Sim", "Nao"};
  for (uint8_t i = 0; i < 2; i++) {
    int16_t x = i == 0 ? 44 : 86;
    bool selected = i == confirmIndex;
    tft.drawRoundRect(x, 48, 34, 20, 5, selected ? COLOR_ACCENT : COLOR_MUTED);
    if (selected) tft.fillRoundRect(x + 2, 50, 30, 16, 4, i == 0 ? COLOR_WARN : COLOR_PANEL_2);
    tft.setTextSize(1);
    tft.setTextColor(selected ? COLOR_TEXT : COLOR_MUTED);
    tft.setCursor(x + 8, 55);
    tft.print(options[i]);
  }
}

void drawFormatConfirmation() {
  tft.fillScreen(COLOR_BG);
  drawHeader("Formatar");
  centerText("Formatar SD? Apaga tudo", 30, COLOR_WARN, 1);

  const char *options[] = {"Sim", "Nao"};
  for (uint8_t i = 0; i < 2; i++) {
    int16_t x = i == 0 ? 44 : 86;
    bool selected = i == confirmIndex;
    tft.drawRoundRect(x, 48, 34, 20, 5, selected ? (i == 0 ? COLOR_WARN : COLOR_ACCENT) : COLOR_MUTED);
    if (selected) tft.fillRoundRect(x + 2, 50, 30, 16, 4, i == 0 ? COLOR_WARN : COLOR_PANEL_2);
    tft.setTextSize(1);
    tft.setTextColor(selected ? COLOR_TEXT : COLOR_MUTED);
    tft.setCursor(x + 8, 55);
    tft.print(options[i]);
  }
}

void showCaptureAnimation() {
  tft.drawRoundRect(8, 8, 144, 64, 4, COLOR_ACCENT);
  delay(12);
  tft.drawRoundRect(20, 16, 120, 48, 4, COLOR_ACCENT);
  delay(12);
  tft.fillScreen(ST77XX_WHITE);
  delay(18);
  drawCameraScreen();
}

void mostrarFrame(camera_fb_t *fb) {
  if (!fb || fb->format != PIXFORMAT_RGB565 || fb->width < SCREEN_W || fb->height < SCREEN_H) return;

  int cropTop = ((int)fb->height - SCREEN_H) / 2;
  if (cropTop < 0) cropTop = 0;

  for (int y = 0; y < SCREEN_H; y++) {
    int sourceY = y + cropTop;
    uint16_t *src = (uint16_t *)(fb->buf + sourceY * fb->width * 2);

    for (int x = 0; x < SCREEN_W; x++) {
      lineBuffer[x] = swapRB(__builtin_bswap16(src[x]));
    }

    tft.drawRGBBitmap(0, y, lineBuffer, SCREEN_W, 1);
  }
}

void updateCameraPreview() {
  if (appState != STATE_CAMERA) return;
  if (!cameraLigada || cameraMode != CAMERA_PREVIEW_JPEG) return;

  unsigned long now = millis();
  if (now - lastCameraFrameMs < cameraFrameIntervalMs) return;
  lastCameraFrameMs = now;

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Falha ao obter framebuffer da camera.");
    showStatus("Falha camera", COLOR_BAD, 1000);
    return;
  }

  if (fb->format == PIXFORMAT_JPEG && fb->len > 0) {
    drawJpegBufferToViewport(fb->buf, fb->len, 0, 0, SCREEN_W, SCREEN_H,
                             CAMERA_PREVIEW_FILL_SCREEN != 0, false);
  } else {
    mostrarFrame(fb);
  }
  esp_camera_fb_return(fb);

  if (messageUntilMs && now > messageUntilMs) {
    messageUntilMs = 0;
  }
}

bool getNextAvailablePhotoPath(char *path, size_t pathSize) {
  for (uint16_t attempt = 0; attempt < 10000; attempt++) {
    buildPhotoPath(nextPhotoNumber, path, pathSize);
    if (!SD_MMC.exists(path)) return true;
    nextPhotoNumber++;
    if (nextPhotoNumber == 0) nextPhotoNumber = 1;
  }
  return false;
}

void putLe16(uint8_t *buffer, uint16_t value) {
  buffer[0] = value & 0xFF;
  buffer[1] = (value >> 8) & 0xFF;
}

void putLe32(uint8_t *buffer, uint32_t value) {
  buffer[0] = value & 0xFF;
  buffer[1] = (value >> 8) & 0xFF;
  buffer[2] = (value >> 16) & 0xFF;
  buffer[3] = (value >> 24) & 0xFF;
}

bool writeBuffered(fs::File &file, const uint8_t *data, size_t len, size_t *totalWritten) {
  size_t offset = 0;

  while (offset < len) {
    size_t chunk = len - offset;
    if (chunk > SD_IO_BUFFER_SIZE) chunk = SD_IO_BUFFER_SIZE;

    memcpy(sdIoBuffer, data + offset, chunk);
    size_t written = file.write(sdIoBuffer, chunk);
    if (totalWritten) *totalWritten += written;

    if (written != chunk) {
      Serial.printf("Falha writeBuffered: offset=%u chunk=%u written=%u\n",
                    (unsigned)offset, (unsigned)chunk, (unsigned)written);
      return false;
    }

    offset += chunk;
    yield();
  }

  return true;
}

bool writeJpegToSD(const char *photoPath, const uint8_t *jpegData, size_t jpegLen, size_t *savedBytes) {
  if (!jpegData || jpegLen < 2 || jpegData[0] != 0xFF || jpegData[1] != 0xD8) return false;

  // EXIF Orientation=6 informa aos visualizadores que a foto deve ser exibida
  // 90 graus no sentido horario, sem recomprimir nem reduzir a qualidade JPEG.
  static const uint8_t exifRotate90Clockwise[] = {
    0xFF, 0xE1, 0x00, 0x22,
    'E', 'x', 'i', 'f', 0x00, 0x00,
    'I', 'I', 0x2A, 0x00, 0x08, 0x00, 0x00, 0x00,
    0x01, 0x00,
    0x12, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x06, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00
  };

  fs::File photoFile = SD_MMC.open(photoPath, FILE_WRITE, true);
  if (!photoFile) {
    Serial.printf("Falha ao abrir %s para escrita JPEG.\n", photoPath);
    return false;
  }

  size_t expectedSize = jpegLen;
  size_t writtenTotal = 0;
  bool ok = true;

#if CAMERA_ROTATE_90_CW
  expectedSize += sizeof(exifRotate90Clockwise);
  ok = writeBuffered(photoFile, jpegData, 2, &writtenTotal) &&
       writeBuffered(photoFile, exifRotate90Clockwise, sizeof(exifRotate90Clockwise), &writtenTotal) &&
       writeBuffered(photoFile, jpegData + 2, jpegLen - 2, &writtenTotal);
#else
  ok = writeBuffered(photoFile, jpegData, jpegLen, &writtenTotal);
#endif
  photoFile.flush();
  photoFile.close();

  if (savedBytes) *savedBytes = writtenTotal;

  fs::File verifyFile = SD_MMC.open(photoPath, FILE_READ);
  size_t verifySize = verifyFile ? verifyFile.size() : 0;
  if (verifyFile) verifyFile.close();

  Serial.printf("Verificacao JPEG %s: escrito=%u salvo=%u esperado=%u\n",
                photoPath, (unsigned)writtenTotal, (unsigned)verifySize, (unsigned)expectedSize);

  if (!ok || writtenTotal != expectedSize || verifySize != expectedSize) {
    SD_MMC.remove(photoPath);
    return false;
  }

  return true;
}

bool writeBmpFromPreviewFrame(const char *photoPath, const uint8_t *rgb565Frame, size_t frameLen, size_t *savedBytes) {
  if (!rgb565Frame || frameLen < CAPTURE_SRC_W * CAPTURE_SRC_H * 2) {
    Serial.println("Frame RGB565 insuficiente para salvar BMP.");
    return false;
  }

  const uint32_t rowSize = PHOTO_W * 3;
  const uint32_t imageSize = rowSize * PHOTO_H;
  const uint32_t fileSize = BMP_HEADER_SIZE + imageSize;

  fs::File photoFile = SD_MMC.open(photoPath, FILE_WRITE, true);
  if (!photoFile) {
    Serial.printf("Falha ao abrir %s para escrita BMP.\n", photoPath);
    return false;
  }

  uint8_t header[BMP_HEADER_SIZE];
  memset(header, 0, sizeof(header));
  header[0] = 'B';
  header[1] = 'M';
  putLe32(header + 2, fileSize);
  putLe32(header + 10, BMP_HEADER_SIZE);
  putLe32(header + 14, 40);
  putLe32(header + 18, PHOTO_W);
  putLe32(header + 22, PHOTO_H);
  putLe16(header + 26, 1);
  putLe16(header + 28, 24);
  putLe32(header + 34, imageSize);

  size_t writtenTotal = 0;
  bool ok = writeBuffered(photoFile, header, sizeof(header), &writtenTotal);

  for (int y = PHOTO_H - 1; ok && y >= 0; y--) {
    int sourceY = CAPTURE_SRC_H - 1 - y;
    const uint16_t *src = (const uint16_t *)(rgb565Frame + sourceY * CAPTURE_SRC_W * 2);

    uint8_t *row = sdIoBuffer;
    for (int x = 0; x < PHOTO_W; x++) {
      int sourceX = x + PHOTO_CROP_LEFT;
      uint16_t color = swapRB(__builtin_bswap16(src[sourceX]));

      uint8_t r = ((color >> 11) & 0x1F) << 3;
      uint8_t g = ((color >> 5) & 0x3F) << 2;
      uint8_t b = (color & 0x1F) << 3;

      row[x * 3 + 0] = b;
      row[x * 3 + 1] = g;
      row[x * 3 + 2] = r;
    }

    size_t rowWritten = photoFile.write(sdIoBuffer, rowSize);
    writtenTotal += rowWritten;
    if (rowWritten != rowSize) {
      Serial.printf("Falha ao gravar linha BMP y=%d written=%u/%u\n", y, (unsigned)rowWritten, (unsigned)rowSize);
      ok = false;
      break;
    }
    yield();
  }

  photoFile.flush();
  photoFile.close();

  if (savedBytes) *savedBytes = writtenTotal;

  if (!ok || writtenTotal != fileSize) {
    Serial.printf("BMP incompleto: escrito=%u esperado=%u\n", (unsigned)writtenTotal, (unsigned)fileSize);
    SD_MMC.remove(photoPath);
    return false;
  }

  fs::File verifyFile = SD_MMC.open(photoPath, FILE_READ);
  size_t verifySize = verifyFile ? verifyFile.size() : 0;
  if (verifyFile) verifyFile.close();

  Serial.printf("Verificacao BMP %s: salvo=%u esperado=%u\n", photoPath, (unsigned)verifySize, (unsigned)fileSize);

  if (verifySize != fileSize) {
    SD_MMC.remove(photoPath);
    return false;
  }

  return true;
}

bool isExpectedCaptureSize(camera_fb_t *fb) {
  if (!fb) return false;

  uint16_t expectedW = frameSizeWidth(activeSensorFrameSize);
  uint16_t expectedH = frameSizeHeight(activeSensorFrameSize);
  if (expectedW == 0 || expectedH == 0) return true;

  return fb->width >= expectedW && fb->height >= expectedH;
}

camera_fb_t *captureJpegFrameForPhoto() {
  camera_fb_t *fb = nullptr;

  for (uint8_t warmup = 0; warmup < CAPTURE_WARMUP_FRAMES; warmup++) {
    fb = esp_camera_fb_get();
    if (!fb) return nullptr;
    Serial.printf("Aquecimento captura: %ux%u formato=%d len=%u\n",
                  (unsigned)fb->width, (unsigned)fb->height, fb->format, (unsigned)fb->len);
    esp_camera_fb_return(fb);
    fb = nullptr;
    delay(30);
  }

  for (uint8_t attempt = 0; attempt < 5; attempt++) {
    fb = esp_camera_fb_get();
    if (!fb) {
      return nullptr;
    }

    if (fb->format == PIXFORMAT_JPEG && fb->len > 0 && isExpectedCaptureSize(fb)) {
      return fb;
    }

    Serial.printf("Descartando frame de captura invalido/pequeno: %ux%u formato=%d len=%u esperado=%s\n",
                  (unsigned)fb->width, (unsigned)fb->height, fb->format, (unsigned)fb->len,
                  frameSizeName(activeSensorFrameSize));
    esp_camera_fb_return(fb);
    fb = nullptr;
    delay(35);
  }

  return nullptr;
}

bool captureAndSavePhoto() {
  if (!sistemaLigado) return false;

  showStatus("Capturando", COLOR_TEXT, 500);
  showCaptureAnimation();
  logHeap("antes-captura");

  if (!initializeCamera(CAMERA_CAPTURE_JPEG)) {
    showStatus("Falha camera", COLOR_BAD, 1200);
    initializeCamera(CAMERA_PREVIEW_JPEG);
    drawCameraScreen();
    return false;
  }

  if (flashEnabled) {
    digitalWrite(FLASH_PIN, HIGH);
    delay(90);
  }

  camera_fb_t *fb = captureJpegFrameForPhoto();
  digitalWrite(FLASH_PIN, LOW);

  if (!fb) {
    Serial.println("Falha ao obter framebuffer JPEG para foto.");
    showStatus("Falha camera", COLOR_BAD, 1200);
    initializeCamera(CAMERA_PREVIEW_JPEG);
    drawCameraScreen();
    return false;
  }

  if (fb->format != PIXFORMAT_JPEG || fb->len == 0) {
    Serial.printf("Framebuffer invalido para foto. formato=%d len=%u\n", fb->format, (unsigned)fb->len);
    esp_camera_fb_return(fb);
    showStatus("JPEG invalido", COLOR_BAD, 1200);
    initializeCamera(CAMERA_PREVIEW_JPEG);
    drawCameraScreen();
    return false;
  }

  Serial.printf("JPEG capturado: %ux%u %u bytes\n", (unsigned)fb->width, (unsigned)fb->height, (unsigned)fb->len);
  showStatus("Salvando SD", COLOR_TEXT, 500);

  if (!beginSDSession()) {
    esp_camera_fb_return(fb);
    showStatus("SD nao encontrado", COLOR_WARN, 1400);
    initializeCamera(CAMERA_PREVIEW_JPEG);
    drawCameraScreen();
    return false;
  }

  bool ok = false;
  char photoPath[PHOTO_PATH_LEN];

  if (!ensureDCIMFolder()) {
    showStatus("Erro /DCIM", COLOR_BAD, 1400);
  } else {
    scanNextPhotoNumber();
    if (!getNextAvailablePhotoPath(photoPath, sizeof(photoPath))) {
      showStatus("Nome indispon.", COLOR_BAD, 1400);
    } else {
      uint64_t total = SD_MMC.totalBytes();
      uint64_t used = SD_MMC.usedBytes();
      uint64_t freeBytes = total > used ? total - used : 0;
      Serial.printf("SD pronto para escrita. Total=%llu Usado=%llu Livre=%llu Arquivo=%s Tamanho=%u\n",
                    total, used, freeBytes, photoPath, (unsigned)fb->len);

      if (freeBytes > 0 && freeBytes < fb->len + 4096) {
        Serial.println("Espaco insuficiente no SD.");
        showStatus("Espaco insuf.", COLOR_BAD, 1400);
      } else {
        size_t savedBytes = 0;
        if (writeJpegToSD(photoPath, fb->buf, fb->len, &savedBytes)) {
          Serial.printf("Foto JPEG salva: %s (%u bytes)\n", photoPath, (unsigned)savedBytes);
          nextPhotoNumber++;
          ok = true;
        } else {
          Serial.println("Falha ao salvar JPEG.");
          showStatus("Erro salvar", COLOR_BAD, 1400);
        }
      }
    }
  }

  endSDSession();
  esp_camera_fb_return(fb);
  initializeCamera(CAMERA_PREVIEW_JPEG);
  drawCameraScreen();

  if (ok) {
    char msg[18];
    snprintf(msg, sizeof(msg), "Salva %04u", nextPhotoNumber - 1);
    showStatus(msg, COLOR_OK, 1200);
  }

  logHeap("apos-captura");
  return ok;
}

void setAppState(AppState newState) {
  appState = newState;
  uiDirty = true;
  lastUiFrameMs = 0;

  switch (newState) {
    case STATE_CAMERA:
      initializeCamera(CAMERA_PREVIEW_JPEG);
      drawCameraScreen();
      break;
    case STATE_MAIN_MENU:
      previousMainMenuIndex = mainMenuIndex;
      menuAnimStartMs = millis();
      break;
    case STATE_SETTINGS:
      previousSettingsIndex = settingsIndex;
      menuAnimStartMs = millis();
      break;
    case STATE_SD_INFO:
      sdInfoIndex = 1;
      break;
    case STATE_SD_FORMAT_CONFIRM:
    case STATE_POWER_CONFIRM:
      confirmIndex = 1;
      break;
    case STATE_GALLERY:
      desligarCamera();
      showCenteredMessage("Carregando", "galeria", COLOR_TEXT);
      loadPhotoList();
      break;
    case STATE_SHUTDOWN:
      enterDeepSleep();
      break;
    default:
      break;
  }
}

void moveMainMenu(int8_t delta) {
  previousMainMenuIndex = mainMenuIndex;
  mainMenuIndex = (mainMenuIndex + 4 + delta) % 4;
  menuAnimStartMs = millis();
  uiDirty = true;
}

void moveSettingsMenu(int8_t delta) {
  previousSettingsIndex = settingsIndex;
  settingsIndex = (settingsIndex + 4 + delta) % 4;
  menuAnimStartMs = millis();
  uiDirty = true;
}

void selectMainMenu() {
  switch (mainMenuIndex) {
    case 0:
      setAppState(STATE_CAMERA);
      break;
    case 1:
      settingsIndex = 0;
      setAppState(STATE_SETTINGS);
      break;
    case 2:
      setAppState(STATE_GALLERY);
      break;
    default:
      setAppState(STATE_POWER_CONFIRM);
      break;
  }
}

void selectSettingsMenu() {
  switch (settingsIndex) {
    case 0:
      setAppState(STATE_SD_INFO);
      break;
    case 1:
      setAppState(STATE_FLASH_SETTINGS);
      break;
    case 2:
      setAppState(STATE_ABOUT);
      break;
    default:
      setAppState(STATE_MAIN_MENU);
      break;
  }
}

void saveFlashPreference() {
  preferences.putBool("flash", flashEnabled);
  Serial.printf("Flash %s salvo em Preferences.\n", flashEnabled ? "ON" : "OFF");
}

void handleButtonEvent(ButtonEvent event) {
  if (event.type == EVT_NONE) return;

  if (event.type == EVT_POWER_HOLD) {
    setAppState(STATE_POWER_CONFIRM);
    return;
  }

  switch (appState) {
    case STATE_CAMERA:
      if (event.type == EVT_UP || event.type == EVT_DOWN) {
        mainMenuIndex = 0;
        setAppState(STATE_MAIN_MENU);
      } else if (event.type == EVT_OK_SHORT) {
        captureAndSavePhoto();
      }
      break;

    case STATE_MAIN_MENU:
      if (event.type == EVT_UP) moveMainMenu(-1);
      else if (event.type == EVT_DOWN) moveMainMenu(1);
      else if (event.type == EVT_OK_SHORT) selectMainMenu();
      else if (event.type == EVT_OK_LONG) setAppState(STATE_CAMERA);
      break;

    case STATE_SETTINGS:
      if (event.type == EVT_UP) moveSettingsMenu(-1);
      else if (event.type == EVT_DOWN) moveSettingsMenu(1);
      else if (event.type == EVT_OK_SHORT) selectSettingsMenu();
      else if (event.type == EVT_OK_LONG) setAppState(STATE_MAIN_MENU);
      break;

    case STATE_SD_INFO:
      if (event.type == EVT_UP || event.type == EVT_DOWN) {
        sdInfoIndex = sdInfoIndex == 0 ? 1 : 0;
        uiDirty = true;
      } else if (event.type == EVT_OK_SHORT) {
        if (sdInfoIndex == 0) setAppState(STATE_SD_FORMAT_CONFIRM);
        else setAppState(STATE_SETTINGS);
      } else if (event.type == EVT_OK_LONG) {
        setAppState(STATE_SETTINGS);
      }
      break;

    case STATE_SD_FORMAT_CONFIRM:
      if (event.type == EVT_UP || event.type == EVT_DOWN) {
        confirmIndex = confirmIndex == 0 ? 1 : 0;
        uiDirty = true;
      } else if (event.type == EVT_OK_SHORT) {
        if (confirmIndex == 0) {
          formatSDCard();
        }
        setAppState(STATE_SD_INFO);
      } else if (event.type == EVT_OK_LONG) {
        setAppState(STATE_SD_INFO);
      }
      break;

    case STATE_FLASH_SETTINGS:
      if (event.type == EVT_UP || event.type == EVT_DOWN || event.type == EVT_OK_SHORT) {
        flashEnabled = !flashEnabled;
        digitalWrite(FLASH_PIN, LOW);
        saveFlashPreference();
        uiDirty = true;
      } else if (event.type == EVT_OK_LONG) {
        setAppState(STATE_SETTINGS);
      }
      break;

    case STATE_ABOUT:
      if (event.type == EVT_OK_SHORT || event.type == EVT_OK_LONG) {
        setAppState(STATE_SETTINGS);
      }
      break;

    case STATE_GALLERY:
      if (event.type == EVT_UP && photoCount > 0) {
        galleryIndex = galleryIndex == 0 ? photoCount - 1 : galleryIndex - 1;
        uiDirty = true;
      } else if (event.type == EVT_DOWN && photoCount > 0) {
        galleryIndex = (galleryIndex + 1) % photoCount;
        uiDirty = true;
      } else if (event.type == EVT_OK_SHORT || event.type == EVT_OK_LONG) {
        setAppState(STATE_MAIN_MENU);
      }
      break;

    case STATE_POWER_CONFIRM:
      if (event.type == EVT_UP || event.type == EVT_DOWN) {
        confirmIndex = confirmIndex == 0 ? 1 : 0;
        uiDirty = true;
      } else if (event.type == EVT_OK_SHORT) {
        if (confirmIndex == 0) setAppState(STATE_SHUTDOWN);
        else setAppState(STATE_MAIN_MENU);
      } else if (event.type == EVT_OK_LONG) {
        setAppState(STATE_MAIN_MENU);
      }
      break;

    default:
      break;
  }
}

void handleButtons() {
  ButtonEvent event = readButtonEvent();
  handleButtonEvent(event);
}

void updateInterface() {
  unsigned long now = millis();
  bool menuAnimating = (appState == STATE_MAIN_MENU || appState == STATE_SETTINGS) &&
                       (now - menuAnimStartMs < menuAnimMs);

  if (!uiDirty && !menuAnimating) return;
  if (now - lastUiFrameMs < 20) return;

  lastUiFrameMs = now;

  switch (appState) {
    case STATE_MAIN_MENU:
      drawMainMenu();
      break;
    case STATE_SETTINGS:
      drawSettingsMenu();
      break;
    case STATE_SD_INFO:
      drawSDInfo();
      break;
    case STATE_SD_FORMAT_CONFIRM:
      drawFormatConfirmation();
      break;
    case STATE_FLASH_SETTINGS:
      drawFlashSettings();
      break;
    case STATE_ABOUT:
      drawAbout();
      break;
    case STATE_GALLERY:
      drawGallery();
      break;
    case STATE_POWER_CONFIRM:
      drawPowerConfirmation();
      break;
    default:
      break;
  }

  uiDirty = menuAnimating;
}

bool verificarPowerSegurado(bool ligar) {
  (void)ligar;
  if (lerBotao() != BTN_FOTO) return false;
  setAppState(STATE_POWER_CONFIRM);
  return true;
}

void tirarFotoComFlash() {
  bool previousFlash = flashEnabled;
  flashEnabled = true;
  captureAndSavePhoto();
  flashEnabled = previousFlash;
}

void ligarSistema() {
  sistemaLigado = true;
  digitalWrite(PWDN_GPIO_NUM, LOW);
  backlightOn();
  initializeDisplay();
  mostrarAnimacaoInicial();
  initializeSDCard();
  iniciarCamera();
  setAppState(STATE_CAMERA);
}

void desligarSistema() {
  setAppState(STATE_SHUTDOWN);
}

void enterDeepSleep() {
  sistemaLigado = false;
  appState = STATE_SHUTDOWN;

  tft.fillScreen(COLOR_BG);
  centerText("Desligando...", 36, COLOR_WARN, 1);
  delay(250);

  Serial.println("Entrando em deep sleep. Acorda pelo GPIO2 em nivel baixo (botao OK/FOTO).");
  Serial.flush();
  Serial.end();
  delay(30);

  digitalWrite(FLASH_PIN, LOW);
  desligarCamera();

  if (sdMounted) {
    SD_MMC.end();
    sdMounted = false;
  }

  tft.fillScreen(COLOR_BG);
  tft.enableDisplay(false);
  backlightOff();
  gpio_hold_en((gpio_num_t)TFT_BL);
  gpio_deep_sleep_hold_en();

  rtc_gpio_pullup_en((gpio_num_t)BUTTON_ADC_PIN);
  rtc_gpio_pulldown_dis((gpio_num_t)BUTTON_ADC_PIN);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_ADC_PIN, 0);
  esp_deep_sleep_start();
}

void initializeDisplay() {
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis((gpio_num_t)TFT_BL);
  pinMode(TFT_BL, OUTPUT);
  backlightOn();

  vspi.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.initR(INITR_MINI160x80);
  tft.setSPISpeed(40000000);
  tft.invertDisplay(false);
  tft.enableDisplay(true);
  tft.setRotation(TFT_APP_ROTATION);
  tft.fillScreen(ST77XX_BLACK);
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println();
  Serial.println("PixieCam iniciando...");

  psramAvailable = psramFound();
  Serial.printf("PSRAM: %s\n", psramAvailable ? "disponivel" : "indisponivel");
  logHeap("boot");

  pinMode(FLASH_PIN, OUTPUT);
  digitalWrite(FLASH_PIN, LOW);

  pinMode(PWDN_GPIO_NUM, OUTPUT);
  digitalWrite(PWDN_GPIO_NUM, LOW);

  pinMode(BUTTON_ADC_PIN, INPUT);
  analogReadResolution(12);

  preferences.begin("pixiecam", false);
  flashEnabled = preferences.getBool("flash", false);
  Serial.printf("Preferencia flash: %s\n", flashEnabled ? "ON" : "OFF");

  initializeDisplay();
  drawBootAnimation();
  bootFinished = true;

  // Cada nova compilacao identifica e salva os valores ADC reais de PARA CIMA,
  // PARA BAIXO e OK/FOTO. Reinicios do mesmo build reutilizam o mapa salvo.
  loadOrCalibrateButtons();

  initializeSDCard();

  if (!initializeCamera(CAMERA_PREVIEW_JPEG)) {
    showCenteredMessage("Falha camera", "ver Serial", COLOR_BAD);
    delay(1000);
  }

  setAppState(STATE_CAMERA);
}

void loop() {
  handleButtons();
  updateInterface();
  updateCameraPreview();
  delay(1);
}
