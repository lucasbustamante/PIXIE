#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_vfs_fat.h"
#include "freertos/semphr.h"
#include "vfs_api.h"
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
#define PROJECT_VERSION     "1.0.0"
#define PROJECT_DEVELOPER   "Desenvolvedor: edite este texto"
#define DCIM_DIR            "/DCIM"
#define PHOTO_PREFIX        "PHOTO_"
#define PHOTO_EXTENSION     ".jpg"
#define ENABLE_SERIAL_DEBUG 0

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
#define TFT_INIT_PROFILE INITR_MINI160x80_PLUGIN
#define CAMERA_DISPLAY_ROTATE_LEFT 1
#define CAMERA_DISPLAY_MIRROR_HORIZONTAL 1
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
#define CAMERA_CAPTURE_JPEG_QUALITY 0
#define CAMERA_CAPTURE_FALLBACK_QUALITY 2
#define CAMERA_PREVIEW_JPEG_QUALITY 12
#define CAMERA_PREVIEW_FRAME_SIZE FRAMESIZE_QQVGA
#define CAPTURE_SENSOR_SETTLE_MS 0
#define PREVIEW_SENSOR_SETTLE_MS 0
#define CAPTURE_WARMUP_FRAMES 0
#define CAPTURE_FRAME_READY_TIMEOUT_MS 900
#define CAPTURE_TRY_ULTRA_RES 1
#define SD_IO_BUFFER_SIZE 4096
#define SD_MOUNT_FREQ_KHZ SDMMC_FREQ_DEFAULT
#define SD_MAX_OPEN_FILES 5
#define SD_FORMAT_ALLOCATION_UNIT 16384
#define SD_FORMAT_TASK_STACK_SIZE 8192
#define MAX_GALLERY_JPEG_BYTES 5000000
#define CAMERA_BOOT_GUARD_MAGIC 0x50495843UL

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

// A classe Arduino SDMMCFS nao expoe o handle exigido pela API oficial de
// formatacao do ESP-IDF. Esta subclasse usa exatamente a mesma implementacao,
// mas disponibiliza o handle protegido sem alterar a biblioteca instalada.
class PixieSDMMCFS : public fs::SDMMCFS {
public:
  explicit PixieSDMMCFS(fs::FSImplPtr impl) : fs::SDMMCFS(impl) {}
  sdmmc_card_t *cardHandle() { return _card; }
};

PixieSDMMCFS pixieSD(fs::FSImplPtr(new VFSImpl()));
#define SD_MMC pixieSD

uint16_t lineBuffer[SCREEN_W];
uint16_t previewFrameBuffer[SCREEN_W * SCREEN_H];
DMA_ATTR uint8_t sdIoBuffer[SD_IO_BUFFER_SIZE];

enum AppState {
  STATE_BOOT,
  STATE_CAMERA,
  STATE_MAIN_MENU,
  STATE_SETTINGS,
  STATE_SD_INFO,
  STATE_SD_FORMAT_CONFIRM,
  STATE_RESET_CONFIRM,
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
bool safeCameraBoot = false;
RTC_NOINIT_ATTR uint32_t cameraBootGuard;

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
uint16_t lastButtonAdcValue = 4095;
uint16_t buttonAdcIdle = 4095;
uint16_t buttonAdcUp = 0;
uint16_t buttonAdcDown = 0;
uint16_t buttonAdcOk = 0;
bool buttonCalibrationValid = false;
bool longEventSent = false;
bool powerEventSent = false;

int16_t jpegViewportX = 0;
int16_t jpegViewportY = 0;
int16_t jpegViewportW = SCREEN_W;
int16_t jpegViewportH = SCREEN_H;
int16_t jpegDrawX = 0;
int16_t jpegDrawY = 0;
uint16_t jpegDecodedW = 0;
uint16_t jpegDecodedH = 0;
uint16_t jpegCropX = 0;
uint16_t jpegCropY = 0;
uint16_t jpegCropW = 0;
uint16_t jpegCropH = 0;
bool jpegRotateLeft = false;
bool jpegMirrorHorizontal = false;
bool jpegStretchToViewport = false;
bool captureProfileCached = false;
framesize_t cachedCaptureFrameSize = FRAMESIZE_SVGA;
bool cachedCaptureUsesPsram = false;
uint8_t cachedCaptureFbCount = 1;
framesize_t activeSensorFrameSize = CAMERA_PREVIEW_FRAME_SIZE;

const unsigned long debounceDelay = 12;
const unsigned long repeatDelayMs = 420;
const unsigned long repeatIntervalMs = 150;
const unsigned long backHoldMs = 900;
const unsigned long tempoSegurarPower = 3000;
const unsigned long stuckButtonMs = 8500;
const unsigned long cameraFrameIntervalMs = 15;
const unsigned long menuAnimMs = 130;

const char *mainMenuItems[] = {"Camera", "Configuracoes", "Galeria", "Desligar"};
const char *settingsItems[] = {"Cartao SD", "Flash", "Sobre", "Reset config", "Voltar"};
const char firmwareBuildId[] = __DATE__ " " __TIME__;

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
void drawResetConfirmation();
bool captureAndSavePhoto();
void loadPhotoList();
void showPhoto(int index);
void enterDeepSleep();

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
  Serial.printf("[%s] heap=%lu psram=%lu\n", label,
                (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getFreePsram());
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
  pinMode(FLASH_PIN, OUTPUT);
  digitalWrite(FLASH_PIN, LOW);
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
  // No modo SD de 1 bit apenas CLK, CMD e DAT0 sao usados pelo barramento;
  // DAT3 deve permanecer alto. D1 e D2 nao participam da transferencia.
  // No AI Thinker, GPIO4/DAT1 tambem aciona o LED de flash: ele precisa
  // continuar como saida LOW para nao causar pico de corrente.
  digitalWrite(FLASH_PIN, LOW);
  digitalWrite(TFT_CS, HIGH);
  vspi.end();

  pinMode(TFT_CS, OUTPUT);
  digitalWrite(TFT_CS, HIGH);
  pinMode(FLASH_PIN, OUTPUT);
  digitalWrite(FLASH_PIN, LOW);
  gpio_set_level((gpio_num_t)FLASH_PIN, 0);

  // GPIO12 e o pino de strap da tensao do flash do ESP32 e nao e usado no
  // SD_MMC de 1 bit. Deixe-o em alta impedancia, sem forcar pull-up.
  pinMode(TFT_MOSI, INPUT);
  pinMode(TFT_SCLK, INPUT_PULLUP);
  pinMode(TFT_DC, INPUT_PULLUP);
  pinMode(BUTTON_ADC_PIN, INPUT_PULLUP);

  gpio_pulldown_dis((gpio_num_t)TFT_DC);
  gpio_pulldown_dis((gpio_num_t)BUTTON_ADC_PIN);
  gpio_pullup_en((gpio_num_t)TFT_DC);
  gpio_pullup_en((gpio_num_t)BUTTON_ADC_PIN);
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

  delay(20);

  tft.setRotation(TFT_APP_ROTATION);
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
  tft.fillRect(0, 0, SCREEN_W, 18, COLOR_PANEL);
  tft.fillRect(0, 0, 3, 18, COLOR_ACCENT);
  tft.drawFastHLine(3, 17, SCREEN_W - 3, COLOR_PANEL_2);

  tft.setTextSize(1);
  tft.setTextColor(COLOR_TEXT, COLOR_PANEL);
  tft.setCursor(8, 5);
  tft.print(title);

  tft.setTextColor(COLOR_MUTED, COLOR_PANEL);
  tft.setCursor(126, 5);
  tft.print("PIXIE");
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

uint16_t readButtonAdcMedian() {
  uint16_t samples[5];

  for (uint8_t i = 0; i < 5; i++) {
    samples[i] = analogRead(BUTTON_ADC_PIN);
  }

  for (uint8_t i = 1; i < 5; i++) {
    uint16_t value = samples[i];
    int8_t j = i - 1;
    while (j >= 0 && samples[j] > value) {
      samples[j + 1] = samples[j];
      j--;
    }
    samples[j + 1] = value;
  }

  return samples[2];
}

uint16_t adcDistance(uint16_t a, uint16_t b) {
  return a > b ? a - b : b - a;
}

uint16_t measureButtonAdcCenter(uint16_t idleReference) {
  const uint8_t sampleCount = 24;
  uint32_t sum = 0;
  uint8_t accepted = 0;

  while (accepted < sampleCount) {
    uint16_t value = readButtonAdcMedian();
    if (adcDistance(value, idleReference) >= 80) {
      sum += value;
      accepted++;
    } else {
      sum = 0;
      accepted = 0;
    }
    delay(2);
  }

  return sum / sampleCount;
}

void drawButtonCalibrationPrompt(const char *buttonName) {
  tft.fillScreen(COLOR_BG);
  drawHeader("Calibracao");
  centerText("Pressione", 25, COLOR_TEXT, 1);
  centerText(buttonName, 39, COLOR_ACCENT, 2);
  centerText("depois solte", 66, COLOR_MUTED, 1);
}

uint16_t calibrateButtonStep(const char *buttonName) {
  drawButtonCalibrationPrompt(buttonName);

  while (adcDistance(readButtonAdcMedian(), buttonAdcIdle) < 80) {
    delay(2);
  }

  delay(18);
  uint16_t center = measureButtonAdcCenter(buttonAdcIdle);

  tft.fillScreen(COLOR_BG);
  drawHeader("Calibracao");
  centerText("Registrado", 30, COLOR_OK, 1);
  centerText("Solte o botao", 49, COLOR_TEXT, 1);

  while (adcDistance(readButtonAdcMedian(), buttonAdcIdle) >= 50) {
    delay(2);
  }
  delay(35);

  return center;
}

bool calibratedButtonCentersAreValid() {
  const uint16_t minimumSeparation = 100;
  return adcDistance(buttonAdcUp, buttonAdcDown) >= minimumSeparation &&
         adcDistance(buttonAdcUp, buttonAdcOk) >= minimumSeparation &&
         adcDistance(buttonAdcDown, buttonAdcOk) >= minimumSeparation &&
         adcDistance(buttonAdcIdle, buttonAdcUp) >= minimumSeparation &&
         adcDistance(buttonAdcIdle, buttonAdcDown) >= minimumSeparation &&
         adcDistance(buttonAdcIdle, buttonAdcOk) >= minimumSeparation;
}

uint32_t firmwareBuildSignature() {
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; firmwareBuildId[i] != 0; i++) {
    hash ^= (uint8_t)firmwareBuildId[i];
    hash *= 16777619UL;
  }
  return hash;
}

void loadButtonCalibration() {
  uint32_t storedBuild = preferences.getUInt("btn_build", 0);
  if (storedBuild != firmwareBuildSignature()) {
    buttonCalibrationValid = false;
    return;
  }

  buttonCalibrationValid = preferences.getBool("btn_cal", false);
  if (!buttonCalibrationValid) return;

  buttonAdcIdle = preferences.getUShort("btn_idle", 4095);
  buttonAdcUp = preferences.getUShort("btn_up", 0);
  buttonAdcDown = preferences.getUShort("btn_down", 0);
  buttonAdcOk = preferences.getUShort("btn_ok", 0);
  buttonCalibrationValid = calibratedButtonCentersAreValid();
}

void runButtonCalibration() {
  while (true) {
    tft.fillScreen(COLOR_BG);
    drawHeader("Calibracao");
    centerText("Solte os botoes", 31, COLOR_TEXT, 1);
    centerText("Preparando...", 49, COLOR_MUTED, 1);

    while (readButtonAdcMedian() < 3800) {
      delay(3);
    }
    delay(120);

    uint32_t idleSum = 0;
    for (uint8_t i = 0; i < 24; i++) {
      idleSum += readButtonAdcMedian();
      delay(2);
    }
    buttonAdcIdle = idleSum / 24;

    buttonAdcUp = calibrateButtonStep("PARA CIMA");
    buttonAdcDown = calibrateButtonStep("PARA BAIXO");
    buttonAdcOk = calibrateButtonStep("OK");

    if (calibratedButtonCentersAreValid()) break;

    showCenteredMessage("Valores iguais", "Tente novamente", COLOR_WARN);
    delay(1200);
  }

  preferences.putUShort("btn_idle", buttonAdcIdle);
  preferences.putUShort("btn_up", buttonAdcUp);
  preferences.putUShort("btn_down", buttonAdcDown);
  preferences.putUShort("btn_ok", buttonAdcOk);
  preferences.putUInt("btn_build", firmwareBuildSignature());
  preferences.putBool("btn_cal", true);
  buttonCalibrationValid = true;

  showCenteredMessage("Botoes prontos", "Calibracao salva", COLOR_OK);
  delay(700);
}

ButtonType lerBotao() {
  lastButtonAdcValue = readButtonAdcMedian();

  if (!buttonCalibrationValid) return BTN_NONE;

  ButtonType nearestButton = BTN_NONE;
  uint16_t nearestDistance = adcDistance(lastButtonAdcValue, buttonAdcIdle);

  uint16_t distance = adcDistance(lastButtonAdcValue, buttonAdcUp);
  if (distance < nearestDistance) {
    nearestDistance = distance;
    nearestButton = BTN_UP;
  }

  distance = adcDistance(lastButtonAdcValue, buttonAdcDown);
  if (distance < nearestDistance) {
    nearestDistance = distance;
    nearestButton = BTN_DOWN;
  }

  distance = adcDistance(lastButtonAdcValue, buttonAdcOk);
  if (distance < nearestDistance) {
    nearestButton = BTN_FOTO;
  }

  return nearestButton;
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
#if ENABLE_SERIAL_DEBUG
      Serial.printf("Botao ADC=%u tipo=%u\n", lastButtonAdcValue, (unsigned)stableButton);
#endif
      buttonPressStartMs = now;
      lastRepeatMs = now;
      longEventSent = false;
      powerEventSent = false;

      // Navegacao responde ao pressionar. OK continua sendo confirmado ao
      // soltar para distinguir clique curto dos atalhos por pressionamento.
      if (stableButton == BTN_UP) event.type = EVT_UP;
      else if (stableButton == BTN_DOWN) event.type = EVT_DOWN;
      return event;
    }

    if (wasPressed && !longEventSent && !powerEventSent) {
      if (releasedButton == BTN_FOTO) event.type = EVT_OK_SHORT;
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
  config.jpeg_quality = mode == CAMERA_PREVIEW_JPEG
                          ? CAMERA_PREVIEW_JPEG_QUALITY
                          : CAMERA_CAPTURE_JPEG_QUALITY;

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
  s->set_aec2(s, 0);
  s->set_ae_level(s, 1);
  s->set_gainceiling(s, GAINCEILING_8X);
  s->set_brightness(s, 1);
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

  // Reserva no boot os maiores buffers que couberem e depois baixa somente o
  // sensor para QQVGA. Assim o preview permanece leve, mas a foto pode mudar
  // para alta resolucao sem desligar e reinicializar toda a camera.
  if (mode == CAMERA_PREVIEW_JPEG) {
    CameraAttempt previewAttempts[] = {
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
      {FRAMESIZE_VGA, true, 1},
      {FRAMESIZE_SVGA, false, 1},
      {FRAMESIZE_VGA, false, 1},
      {FRAMESIZE_QVGA, false, 1},
      {CAMERA_PREVIEW_FRAME_SIZE, true, 2},
      {CAMERA_PREVIEW_FRAME_SIZE, false, 2},
      {CAMERA_PREVIEW_FRAME_SIZE, false, 1}
    };

    for (uint8_t i = 0; i < sizeof(previewAttempts) / sizeof(previewAttempts[0]); i++) {
      if (safeCameraBoot && previewAttempts[i].frameSize != CAMERA_PREVIEW_FRAME_SIZE) continue;
      if (previewAttempts[i].usePsram && !psramAvailable) continue;

      camera_config_t config = makeCameraConfig(CAMERA_PREVIEW_JPEG,
                                                previewAttempts[i].frameSize,
                                                previewAttempts[i].usePsram,
                                                previewAttempts[i].fbCount);
      Serial.printf("Tentando reservar buffer %s memoria=%s fb=%u\n",
                    frameSizeName(previewAttempts[i].frameSize),
                    previewAttempts[i].usePsram ? "PSRAM" : "DRAM",
                    previewAttempts[i].fbCount);

      esp_err_t err = esp_camera_init(&config);
      if (err == ESP_OK) {
        applySensorDefaults();
        cameraLigada = true;
        captureProfileCached = previewAttempts[i].frameSize != CAMERA_PREVIEW_FRAME_SIZE;
        if (captureProfileCached) {
          cachedCaptureFrameSize = previewAttempts[i].frameSize;
          cachedCaptureUsesPsram = previewAttempts[i].usePsram;
          cachedCaptureFbCount = previewAttempts[i].fbCount;
        }

        if (setCameraFrameProfile(CAMERA_PREVIEW_FRAME_SIZE,
                                  CAMERA_PREVIEW_JPEG_QUALITY,
                                  CAMERA_PREVIEW_JPEG,
                                  PREVIEW_SENSOR_SETTLE_MS)) {
          Serial.printf("Preview pronto; captura prealocada=%s memoria=%s fb=%u\n",
                        captureProfileCached ? frameSizeName(cachedCaptureFrameSize) : "nao",
                        previewAttempts[i].usePsram ? "PSRAM" : "DRAM",
                        previewAttempts[i].fbCount);
          return true;
        }
      }

      Serial.printf("Falha ao reservar perfil de camera: 0x%x\n", err);
      esp_camera_deinit();
      cameraLigada = false;
      cameraMode = CAMERA_OFF;
      digitalWrite(PWDN_GPIO_NUM, HIGH);
      delay(20);
      digitalWrite(PWDN_GPIO_NUM, LOW);
      delay(20);
    }

    Serial.println("Falha ao inicializar o preview.");
    return false;
  }

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
      if (safeCameraBoot && attempts[i].frameSize > FRAMESIZE_SVGA) continue;
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

  // O controlador da camera permanece produzindo quadros mesmo fora da tela
  // Camera. Pare-o antes de qualquer acesso ao SD para nao disputar DMA/RAM.
  if (cameraLigada) {
    desligarCamera();
    delay(20);
  }

  prepareSharedPinsForSD();

  // GPIO2 e simultaneamente DAT0 e a entrada do ladder de botoes. Se a linha
  // continuar baixa mesmo com pull-up, nao entregue o pino ao controlador:
  // isso indica botao preso ou pull-down permanente e evita travar o driver.
  if (digitalRead(BUTTON_ADC_PIN) == LOW) {
    if (!quiet) Serial.println("GPIO2/DAT0 esta baixo; solte os botoes e verifique o ladder.");
    sdAvailable = false;
    restoreDisplayBus();
    return false;
  }

  bool pinsOk = SD_MMC.setPins(SD_MMC_CLK_PIN, SD_MMC_CMD_PIN, SD_MMC_D0_PIN);
  if (!pinsOk) {
    if (!quiet) Serial.println("Falha ao configurar pinos SD_MMC.");
    sdAvailable = false;
    restoreDisplayBus();
    return false;
  }

  // Use a mesma inicializacao simples e estavel do firmware funcional do
  // repositorio: uma unica montagem, modo 1-bit e frequencia padrao de 20 MHz.
  // Evitar varias inicializacoes seguidas tambem evita estados incompletos do
  // host SDMMC depois de uma falha de comunicacao.
  Serial.printf("Montando SD_MMC 1-bit em %u kHz.\n", (unsigned)SD_MOUNT_FREQ_KHZ);
  bool mounted = SD_MMC.begin("/sdcard", true, false,
                              SD_MOUNT_FREQ_KHZ, SD_MAX_OPEN_FILES);

  // O driver de 1 bit nao usa DAT1; mantenha o flash apagado durante todo o IO.
  pinMode(FLASH_PIN, OUTPUT);
  digitalWrite(FLASH_PIN, LOW);
  gpio_set_level((gpio_num_t)FLASH_PIN, 0);

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

struct SDFormatJob {
  SemaphoreHandle_t finished;
  esp_err_t result;
};

void formatSDWorker(void *parameter) {
  SDFormatJob *job = static_cast<SDFormatJob *>(parameter);
  esp_vfs_fat_mount_config_t formatConfig = {};
  formatConfig.format_if_mount_failed = false;
  formatConfig.max_files = SD_MAX_OPEN_FILES;
  formatConfig.allocation_unit_size = SD_FORMAT_ALLOCATION_UNIT;
  formatConfig.disk_status_check_enable = false;
  formatConfig.use_one_fat = false;

  job->result = esp_vfs_fat_sdcard_format_cfg("/sdcard", SD_MMC.cardHandle(),
                                               &formatConfig);
  xSemaphoreGive(job->finished);
  vTaskDelete(nullptr);
}

bool formatMountedSDCard() {
  if (!sdMounted || !SD_MMC.cardHandle()) return false;

  SDFormatJob job = {};
  job.finished = xSemaphoreCreateBinary();
  job.result = ESP_FAIL;
  if (!job.finished) {
    Serial.println("Sem memoria para iniciar formatacao do SD.");
    return false;
  }

  TaskHandle_t formatTask = nullptr;
  BaseType_t created = xTaskCreatePinnedToCore(
    formatSDWorker, "pixie_sd_format", SD_FORMAT_TASK_STACK_SIZE,
    &job, 1, &formatTask, 0
  );

  if (created != pdPASS) {
    Serial.println("Falha ao criar tarefa de formatacao do SD.");
    vSemaphoreDelete(job.finished);
    return false;
  }

  // O TFT permanece intocado enquanto a tarefa usa os pinos compartilhados.
  // A espera curta devolve CPU ao sistema e evita watchdog durante cartoes
  // grandes ou lentos.
  while (xSemaphoreTake(job.finished, pdMS_TO_TICKS(50)) != pdTRUE) {
    delay(1);
  }

  vSemaphoreDelete(job.finished);
  Serial.printf("Resultado da formatacao FAT: %s (0x%x)\n",
                esp_err_to_name(job.result), (unsigned)job.result);
  return job.result == ESP_OK;
}

bool formatSDCard() {
  showCenteredMessage("Formatando...", "Aguarde", COLOR_WARN);
  Serial.println("Apagando conteudo do cartao SD.");

  // Monte primeiro sem formatacao automatica. O caminho antigo executava uma
  // operacao longa dentro de SD_MMC.begin() e podia reiniciar o dispositivo.
  if (!beginSDSession(false)) {
    showCenteredMessage("Use SD FAT32", "Nao foi montado", COLOR_BAD);
    return false;
  }

  bool ok = formatMountedSDCard();
  if (ok && !SD_MMC.exists(DCIM_DIR)) {
    ok = SD_MMC.mkdir(DCIM_DIR);
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
  if (jpegStretchToViewport) {
    if (x < 0 || y < 0 || x >= jpegDecodedW || y >= jpegDecodedH) return true;

    uint16_t sourceW = x + w > jpegDecodedW ? jpegDecodedW - x : w;
    uint16_t sourceH = y + h > jpegDecodedH ? jpegDecodedH - y : h;
    uint16_t orientedW = jpegRotateLeft ? jpegDecodedH : jpegDecodedW;

    for (uint16_t sourceRow = 0; sourceRow < sourceH; sourceRow++) {
      uint16_t sourceY = y + sourceRow;

      for (uint16_t sourceCol = 0; sourceCol < sourceW; sourceCol++) {
        uint16_t sourceX = x + sourceCol;
        uint16_t rotatedX = jpegRotateLeft ? sourceY : sourceX;
        uint16_t rotatedY = jpegRotateLeft ? jpegDecodedW - 1 - sourceX : sourceY;

        if (jpegMirrorHorizontal) {
          rotatedX = orientedW - 1 - rotatedX;
        }

        if (rotatedX < jpegCropX || rotatedX >= jpegCropX + jpegCropW ||
            rotatedY < jpegCropY || rotatedY >= jpegCropY + jpegCropH) {
          continue;
        }

        uint16_t croppedX = rotatedX - jpegCropX;
        uint16_t croppedY = rotatedY - jpegCropY;
        uint16_t destX0 = ((uint32_t)croppedX * jpegViewportW) / jpegCropW;
        uint16_t destX1 = ((uint32_t)(croppedX + 1) * jpegViewportW) / jpegCropW;
        uint16_t destY0 = ((uint32_t)croppedY * jpegViewportH) / jpegCropH;
        uint16_t destY1 = ((uint32_t)(croppedY + 1) * jpegViewportH) / jpegCropH;

        if (destX1 <= destX0 || destY1 <= destY0) continue;

        uint16_t color = bitmap[sourceRow * w + sourceCol];
        for (uint16_t destY = destY0; destY < destY1; destY++) {
          uint16_t *dest = previewFrameBuffer + destY * jpegViewportW + destX0;
          for (uint16_t destX = destX0; destX < destX1; destX++) {
            *dest++ = color;
          }
        }
      }
    }

    return true;
  }

  if (jpegRotateLeft) {
    if (x >= jpegDecodedW || y >= jpegDecodedH) return true;

    uint16_t sourceW = x + w > jpegDecodedW ? jpegDecodedW - x : w;
    uint16_t sourceH = y + h > jpegDecodedH ? jpegDecodedH - y : h;
    int16_t blockX = jpegDrawX + y;
    int16_t blockY = jpegDrawY + jpegDecodedW - (x + sourceW);
    int16_t viewXEnd = jpegViewportX + jpegViewportW;
    int16_t viewYEnd = jpegViewportY + jpegViewportH;
    int16_t clipX0 = blockX > jpegViewportX ? blockX : jpegViewportX;
    int16_t clipX1 = blockX + sourceH < viewXEnd ? blockX + sourceH : viewXEnd;

    if (clipX0 >= clipX1) return true;

    for (uint16_t destRow = 0; destRow < sourceW; destRow++) {
      int16_t screenY = blockY + destRow;
      if (screenY < jpegViewportY || screenY >= viewYEnd) continue;

      uint16_t sourceCol = sourceW - 1 - destRow;
      uint16_t drawW = clipX1 - clipX0;
      uint16_t firstSourceRow = clipX0 - blockX;

      for (uint16_t col = 0; col < drawW; col++) {
        uint16_t sourceRow = firstSourceRow + col;
        lineBuffer[col] = bitmap[sourceRow * w + sourceCol];
      }

      tft.drawRGBBitmap(clipX0, screenY, lineBuffer, drawW, 1);
    }

    return true;
  }

  int16_t screenX = jpegDrawX + x;
  int16_t screenY = jpegDrawY + y;
  int16_t clipX0 = screenX > jpegViewportX ? screenX : jpegViewportX;
  int16_t clipY0 = screenY > jpegViewportY ? screenY : jpegViewportY;
  int16_t xEnd = screenX + (int16_t)w;
  int16_t yEnd = screenY + (int16_t)h;
  int16_t viewXEnd = jpegViewportX + jpegViewportW;
  int16_t viewYEnd = jpegViewportY + jpegViewportH;
  int16_t clipX1 = xEnd < viewXEnd ? xEnd : viewXEnd;
  int16_t clipY1 = yEnd < viewYEnd ? yEnd : viewYEnd;

  if (clipX0 >= clipX1 || clipY0 >= clipY1) return true;

  for (int16_t row = clipY0; row < clipY1; row++) {
    uint16_t *src = bitmap + (row - screenY) * w + (clipX0 - screenX);
    uint16_t drawW = clipX1 - clipX0;

    for (uint16_t col = 0; col < drawW; col++) {
      lineBuffer[col] = src[col];
    }

    tft.drawRGBBitmap(clipX0, row, lineBuffer, drawW, 1);
  }

  return true;
}

bool drawJpegBufferToViewport(const uint8_t *jpegBuffer, size_t fileSize,
                              int16_t areaX, int16_t areaY, int16_t areaW, int16_t areaH,
                              bool clearArea, bool rotateLeft, bool mirrorHorizontal,
                              bool stretchToViewport) {
  if (!jpegBuffer || fileSize == 0) return false;

  uint16_t jpgW = 0;
  uint16_t jpgH = 0;
  JRESULT sizeResult = TJpgDec.getJpgSize(&jpgW, &jpgH, jpegBuffer, fileSize);
  if (sizeResult != JDR_OK || jpgW == 0 || jpgH == 0) {
    Serial.println("Falha ao obter tamanho do JPEG.");
    return false;
  }

  uint8_t scale = 1;
  uint16_t orientedW = rotateLeft ? jpgH : jpgW;
  uint16_t orientedH = rotateLeft ? jpgW : jpgH;
  bool isLivePreviewSize = jpgW == 160 && jpgH == 120;
  if (!stretchToViewport || !isLivePreviewSize) {
    while ((orientedW / scale > areaW || orientedH / scale > areaH) && scale < 8) {
      scale *= 2;
    }
  }

  jpegDecodedW = jpgW / scale;
  jpegDecodedH = jpgH / scale;
  uint16_t decodedOrientedW = rotateLeft ? jpegDecodedH : jpegDecodedW;
  uint16_t decodedOrientedH = rotateLeft ? jpegDecodedW : jpegDecodedH;
  int16_t drawW = decodedOrientedW;
  int16_t drawH = decodedOrientedH;
  int16_t drawX = areaX + (areaW - drawW) / 2;
  int16_t drawY = areaY + (areaH - drawH) / 2;

  jpegViewportX = areaX;
  jpegViewportY = areaY;
  jpegViewportW = areaW;
  jpegViewportH = areaH;
  jpegDrawX = drawX;
  jpegDrawY = drawY;
  jpegRotateLeft = rotateLeft;
  jpegMirrorHorizontal = mirrorHorizontal;
  jpegStretchToViewport = stretchToViewport;
  jpegCropX = 0;
  jpegCropY = 0;
  jpegCropW = decodedOrientedW;
  jpegCropH = decodedOrientedH;

  if (stretchToViewport) {
    if ((uint32_t)decodedOrientedW * areaH > (uint32_t)decodedOrientedH * areaW) {
      jpegCropW = ((uint32_t)decodedOrientedH * areaW) / areaH;
      jpegCropX = (decodedOrientedW - jpegCropW) / 2;
    } else {
      jpegCropH = ((uint32_t)decodedOrientedW * areaH) / areaW;
      jpegCropY = (decodedOrientedH - jpegCropH) / 2;
    }
  }

  if (stretchToViewport) {
    uint32_t pixelCount = (uint32_t)areaW * areaH;
    for (uint32_t i = 0; i < pixelCount; i++) {
      previewFrameBuffer[i] = COLOR_BG;
    }
  } else if (clearArea) {
    tft.fillRect(areaX, areaY, areaW, areaH, COLOR_BG);
  }

  TJpgDec.setJpgScale(scale);
  TJpgDec.setSwapBytes(false);
  TJpgDec.setCallback(tftJpegOutput);

  JRESULT drawResult = TJpgDec.drawJpg(0, 0, jpegBuffer, fileSize);
  if (drawResult != JDR_OK) {
    Serial.printf("Falha ao desenhar JPEG: %d\n", drawResult);
    return false;
  }

  if (stretchToViewport) {
    tft.drawRGBBitmap(areaX, areaY, previewFrameBuffer, areaW, areaH);
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
                                             false, CAMERA_DISPLAY_ROTATE_LEFT,
                                             CAMERA_DISPLAY_MIRROR_HORIZONTAL, true);
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

int16_t animatedRowY(uint8_t current, uint8_t previous, uint8_t rowHeight) {
  int16_t targetY = 18 + current * rowHeight;
  if (current == previous || millis() - menuAnimStartMs >= menuAnimMs) {
    return targetY;
  }

  int16_t startY = 18 + previous * rowHeight;
  unsigned long elapsed = millis() - menuAnimStartMs;
  return startY + ((targetY - startY) * (int16_t)elapsed) / (int16_t)menuAnimMs;
}

void drawListMenu(const char *title, const char **items, uint8_t count, uint8_t selected, uint8_t previous, bool mainMenu) {
  tft.fillScreen(COLOR_BG);
  drawHeader(title);

  uint8_t rowHeight = mainMenu ? 15 : 12;
  int16_t highlightY = animatedRowY(selected, previous, rowHeight);
  int8_t highlightOffset = mainMenu ? 0 : 0;
  uint8_t highlightHeight = mainMenu ? 14 : 11;
  tft.fillRoundRect(4, highlightY + highlightOffset, 148, highlightHeight, 4, COLOR_PANEL_2);
  tft.fillRoundRect(4, highlightY + highlightOffset, 4, highlightHeight, 2, COLOR_ACCENT);
  tft.drawFastVLine(150, highlightY + 3, highlightHeight - 6, COLOR_ACCENT);

  for (uint8_t i = 0; i < count; i++) {
    int16_t y = 18 + i * rowHeight;
    bool isSelected = i == selected;
    uint16_t color = isSelected ? COLOR_TEXT : COLOR_MUTED;

    if (mainMenu) {
      if (isSelected) {
        tft.fillRoundRect(10, y + 1, 18, 12, 3, COLOR_PANEL);
      }
      drawIcon(i, 12, y, isSelected ? COLOR_ACCENT : COLOR_MUTED);
      tft.setCursor(36, y + 4);
    } else {
      tft.fillCircle(15, y + 5, isSelected ? 3 : 2, isSelected ? COLOR_ACCENT : COLOR_PANEL_2);
      if (isSelected) tft.drawCircle(15, y + 5, 4, COLOR_ACCENT);
      tft.setCursor(26, y + 2);
    }

    tft.setTextSize(1);
    tft.setTextColor(color);
    tft.print(items[i]);

    if (!isSelected && i + 1 < count) {
      tft.drawFastHLine(mainMenu ? 36 : 26, y + rowHeight - 1,
                        mainMenu ? 108 : 118, COLOR_PANEL);
    }
  }

  for (uint8_t i = 0; i < count; i++) {
    uint16_t dotColor = i == selected ? COLOR_ACCENT : COLOR_PANEL_2;
    int16_t dotY = 18 + i * rowHeight + rowHeight / 2;
    tft.fillCircle(156, dotY, i == selected ? 2 : 1, dotColor);
  }
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
  drawListMenu("Configuracoes", settingsItems, 5, settingsIndex, previousSettingsIndex, false);
}

void drawSDInfo() {
  bool cardReady = false;
  uint8_t type = CARD_NONE;
  uint16_t storedPhotos = 0;
  char totalText[18] = "-";
  char usedText[18] = "-";
  char freeText[18] = "-";

  if (beginSDSession(true)) {
    uint64_t total = SD_MMC.totalBytes();
    uint64_t used = SD_MMC.usedBytes();
    uint64_t freeBytes = total > used ? total - used : 0;
    type = SD_MMC.cardType();
    storedPhotos = countPhotosOnMountedSD();
    scanNextPhotoNumber();
    formatBytes(total, totalText, sizeof(totalText));
    formatBytes(used, usedText, sizeof(usedText));
    formatBytes(freeBytes, freeText, sizeof(freeText));
    cardReady = true;
    endSDSession();
  }

  // Desenhe somente depois de devolver GPIO14/15/2 ao TFT/botoes.
  tft.fillScreen(COLOR_BG);
  drawHeader("Cartao SD");

  if (!cardReady) {
    tft.setTextSize(1);
    centerText("SD nao encontrado", 38, COLOR_WARN, 1);
  } else {
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

void drawResetConfirmation() {
  tft.fillScreen(COLOR_BG);
  drawHeader("Reset config");
  centerText("Apagar configuracoes?", 30, COLOR_WARN, 1);

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
  tft.drawRoundRect(3, 3, SCREEN_W - 6, SCREEN_H - 6, 5, COLOR_ACCENT);
  tft.fillScreen(ST77XX_WHITE);
  delay(6);
  drawCameraScreen();
}

void mostrarFrame(camera_fb_t *fb) {
  if (!fb || fb->format != PIXFORMAT_RGB565 || fb->width < SCREEN_W || fb->height < SCREEN_H) return;

  uint16_t *src = (uint16_t *)fb->buf;
#if CAMERA_DISPLAY_ROTATE_LEFT
  uint16_t orientedW = fb->height;
  uint16_t orientedH = fb->width;
#else
  uint16_t orientedW = fb->width;
  uint16_t orientedH = fb->height;
#endif
  uint16_t cropX = 0;
  uint16_t cropY = 0;
  uint16_t cropW = orientedW;
  uint16_t cropH = orientedH;

  if ((uint32_t)orientedW * SCREEN_H > (uint32_t)orientedH * SCREEN_W) {
    cropW = ((uint32_t)orientedH * SCREEN_W) / SCREEN_H;
    cropX = (orientedW - cropW) / 2;
  } else {
    cropH = ((uint32_t)orientedW * SCREEN_H) / SCREEN_W;
    cropY = (orientedH - cropH) / 2;
  }

  for (uint16_t y = 0; y < SCREEN_H; y++) {
    for (uint16_t x = 0; x < SCREEN_W; x++) {
      uint16_t orientedX = cropX + ((uint32_t)x * cropW) / SCREEN_W;
      uint16_t orientedY = cropY + ((uint32_t)y * cropH) / SCREEN_H;
#if CAMERA_DISPLAY_MIRROR_HORIZONTAL
      orientedX = orientedW - 1 - orientedX;
#endif
#if CAMERA_DISPLAY_ROTATE_LEFT
      uint16_t sourceX = fb->width - 1 - orientedY;
      uint16_t sourceY = orientedX;
#else
      uint16_t sourceX = orientedX;
      uint16_t sourceY = orientedY;
#endif
      previewFrameBuffer[y * SCREEN_W + x] =
        swapRB(__builtin_bswap16(src[sourceY * fb->width + sourceX]));
    }
  }

  tft.drawRGBBitmap(0, 0, previewFrameBuffer, SCREEN_W, SCREEN_H);
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

  bool previewDrawn = false;
  if (fb->format == PIXFORMAT_JPEG && fb->len > 0) {
    previewDrawn = drawJpegBufferToViewport(fb->buf, fb->len, 0, 0, SCREEN_W, SCREEN_H,
                                            false, CAMERA_DISPLAY_ROTATE_LEFT,
                                            CAMERA_DISPLAY_MIRROR_HORIZONTAL, true);
  } else {
    mostrarFrame(fb);
    previewDrawn = true;
  }
  esp_camera_fb_return(fb);

  if (previewDrawn && cameraBootGuard == CAMERA_BOOT_GUARD_MAGIC) {
    cameraBootGuard = 0;
  }

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
  if (!jpegData || jpegLen == 0) return false;

  for (uint8_t attempt = 0; attempt < 2; attempt++) {
    SD_MMC.remove(photoPath);
    fs::File photoFile = SD_MMC.open(photoPath, FILE_WRITE, true);
    if (!photoFile) {
      Serial.printf("Falha ao abrir %s para escrita JPEG (tentativa %u).\n",
                    photoPath, attempt + 1);
      delay(25);
      continue;
    }

    size_t writtenTotal = 0;
    bool ok = writeBuffered(photoFile, jpegData, jpegLen, &writtenTotal);
    photoFile.flush();
    photoFile.close();

    fs::File verifyFile = SD_MMC.open(photoPath, FILE_READ);
    size_t verifySize = verifyFile ? verifyFile.size() : 0;
    if (verifyFile) verifyFile.close();

    Serial.printf("Verificacao JPEG %s: escrito=%u salvo=%u esperado=%u tentativa=%u\n",
                  photoPath, (unsigned)writtenTotal, (unsigned)verifySize,
                  (unsigned)jpegLen, attempt + 1);

    if (ok && writtenTotal == jpegLen && verifySize == jpegLen) {
      if (savedBytes) *savedBytes = writtenTotal;
      return true;
    }

    SD_MMC.remove(photoPath);
    delay(35);
  }

  if (savedBytes) *savedBytes = 0;
  return false;
}

bool writeJpegWithSDRecovery(const char *photoPath, const uint8_t *jpegData,
                             size_t jpegLen, size_t *savedBytes) {
  if (writeJpegToSD(photoPath, jpegData, jpegLen, savedBytes)) return true;

  Serial.println("Reiniciando barramento SD para nova tentativa de gravacao.");
  endSDSession();
  delay(60);

  if (!beginSDSession() || !ensureDCIMFolder()) return false;
  return writeJpegToSD(photoPath, jpegData, jpegLen, savedBytes);
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

bool readJpegDimensions(const uint8_t *data, size_t length, uint16_t *width, uint16_t *height) {
  if (!data || length < 12 || data[0] != 0xFF || data[1] != 0xD8) return false;

  size_t offset = 2;
  while (offset + 8 < length) {
    while (offset < length && data[offset] != 0xFF) offset++;
    while (offset < length && data[offset] == 0xFF) offset++;
    if (offset >= length) break;

    uint8_t marker = data[offset++];
    if (marker == 0xD9 || marker == 0xDA) break;
    if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;
    if (offset + 1 >= length) return false;

    uint16_t segmentLength = ((uint16_t)data[offset] << 8) | data[offset + 1];
    if (segmentLength < 2 || offset + segmentLength > length) return false;

    bool isSof = marker >= 0xC0 && marker <= 0xCF &&
                 marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
    if (isSof && segmentLength >= 7) {
      *height = ((uint16_t)data[offset + 3] << 8) | data[offset + 4];
      *width = ((uint16_t)data[offset + 5] << 8) | data[offset + 6];
      return *width > 0 && *height > 0;
    }

    offset += segmentLength;
  }

  return false;
}

bool jpegHasEndMarker(const uint8_t *data, size_t length) {
  if (!data || length < 4) return false;
  size_t first = length > 64 ? length - 64 : 2;
  for (size_t i = length - 1; i > first; i--) {
    if (data[i - 1] == 0xFF && data[i] == 0xD9) return true;
  }
  return false;
}

bool isExpectedCaptureSize(camera_fb_t *fb) {
  if (!fb || fb->format != PIXFORMAT_JPEG || fb->len == 0 ||
      !jpegHasEndMarker(fb->buf, fb->len)) return false;

  uint16_t jpegW = 0;
  uint16_t jpegH = 0;
  if (!readJpegDimensions(fb->buf, fb->len, &jpegW, &jpegH)) return false;

  uint16_t expectedW = frameSizeWidth(activeSensorFrameSize);
  uint16_t expectedH = frameSizeHeight(activeSensorFrameSize);
  if (expectedW == 0 || expectedH == 0) return true;

  return jpegW >= expectedW && jpegH >= expectedH;
}

camera_fb_t *takeAvailableCameraFrame(uint32_t timeoutMs) {
  unsigned long startedAt = millis();
  while (millis() - startedAt < timeoutMs) {
    if (esp_camera_available_frames()) {
      return esp_camera_fb_get();
    }
    delay(1);
  }
  return nullptr;
}

camera_fb_t *captureJpegFrameForPhoto() {
  camera_fb_t *fb = nullptr;

#if CAPTURE_WARMUP_FRAMES > 0
  for (uint8_t warmup = 0; warmup < CAPTURE_WARMUP_FRAMES; warmup++) {
    fb = esp_camera_fb_get();
    if (!fb) return nullptr;
    Serial.printf("Aquecimento captura: %ux%u formato=%d len=%u\n",
                  (unsigned)fb->width, (unsigned)fb->height, fb->format, (unsigned)fb->len);
    esp_camera_fb_return(fb);
    fb = nullptr;
    delay(30);
  }
#endif

  for (uint8_t attempt = 0; attempt < 3; attempt++) {
    fb = takeAvailableCameraFrame(CAPTURE_FRAME_READY_TIMEOUT_MS);
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
    delay(2);
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

  if (!fb) {
    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor && sensor->set_quality(sensor, CAMERA_CAPTURE_FALLBACK_QUALITY) == 0) {
      Serial.printf("Qualidade %u nao gerou quadro completo; tentando qualidade %u.\n",
                    CAMERA_CAPTURE_JPEG_QUALITY, CAMERA_CAPTURE_FALLBACK_QUALITY);
      esp_camera_return_all();
      fb = captureJpegFrameForPhoto();
    }
  }

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

  size_t jpegLength = fb->len;
  uint16_t capturedWidth = fb->width;
  uint16_t capturedHeight = fb->height;
  uint8_t *jpegCopy = (uint8_t *)allocImageBuffer(jpegLength);

  if (!jpegCopy) {
    Serial.printf("Sem memoria para isolar JPEG de %u bytes antes do SD.\n", (unsigned)jpegLength);
    esp_camera_fb_return(fb);
    showStatus("Memoria baixa", COLOR_BAD, 1400);
    initializeCamera(CAMERA_PREVIEW_JPEG);
    drawCameraScreen();
    return false;
  }

  memcpy(jpegCopy, fb->buf, jpegLength);
  esp_camera_fb_return(fb);
  fb = nullptr;

  // Interrompe I2S/DMA e libera os framebuffers antes de entregar o barramento
  // compartilhado ao SD. O JPEG ja esta preservado integralmente na PSRAM.
  desligarCamera();

  Serial.printf("JPEG isolado: %ux%u %u bytes\n",
                (unsigned)capturedWidth, (unsigned)capturedHeight, (unsigned)jpegLength);
  showStatus("Salvando SD", COLOR_TEXT, 500);

  if (!beginSDSession()) {
    heap_caps_free(jpegCopy);
    showStatus("SD nao encontrado", COLOR_WARN, 1400);
    initializeCamera(CAMERA_PREVIEW_JPEG);
    drawCameraScreen();
    return false;
  }

  bool ok = false;
  const char *failureStatus = nullptr;
  char photoPath[PHOTO_PATH_LEN];

  if (!ensureDCIMFolder()) {
    failureStatus = "Erro /DCIM";
  } else {
    scanNextPhotoNumber();
    if (!getNextAvailablePhotoPath(photoPath, sizeof(photoPath))) {
      failureStatus = "Nome indispon.";
    } else {
      uint64_t total = SD_MMC.totalBytes();
      uint64_t used = SD_MMC.usedBytes();
      uint64_t freeBytes = total > used ? total - used : 0;
      Serial.printf("SD pronto para escrita. Total=%llu Usado=%llu Livre=%llu Arquivo=%s Tamanho=%u\n",
                    total, used, freeBytes, photoPath, (unsigned)jpegLength);

      if (freeBytes > 0 && freeBytes < jpegLength + 4096) {
        Serial.println("Espaco insuficiente no SD.");
        failureStatus = "Espaco insuf.";
      } else {
        size_t savedBytes = 0;
        if (writeJpegWithSDRecovery(photoPath, jpegCopy, jpegLength, &savedBytes)) {
          Serial.printf("Foto JPEG salva: %s (%u bytes)\n", photoPath, (unsigned)savedBytes);
          nextPhotoNumber++;
          ok = true;
        } else {
          Serial.println("Falha ao salvar JPEG.");
          failureStatus = "Erro salvar";
        }
      }
    }
  }

  endSDSession();
  heap_caps_free(jpegCopy);
  initializeCamera(CAMERA_PREVIEW_JPEG);
  drawCameraScreen();

  if (ok) {
    char msg[18];
    snprintf(msg, sizeof(msg), "Salva %04u", nextPhotoNumber - 1);
    showStatus(msg, COLOR_OK, 1200);
  } else if (failureStatus) {
    // O TFT compartilha GPIO14/15 com o SD. Mensagens so podem ser desenhadas
    // depois que endSDSession() devolveu o barramento ao display.
    showStatus(failureStatus, COLOR_BAD, 1400);
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
    case STATE_RESET_CONFIRM:
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
  settingsIndex = (settingsIndex + 5 + delta) % 5;
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
    case 3:
      setAppState(STATE_RESET_CONFIRM);
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

    case STATE_RESET_CONFIRM:
      if (event.type == EVT_UP || event.type == EVT_DOWN) {
        confirmIndex = confirmIndex == 0 ? 1 : 0;
        uiDirty = true;
      } else if (event.type == EVT_OK_SHORT) {
        if (confirmIndex == 0) {
          bool cleared = preferences.clear();
          showCenteredMessage(cleared ? "Config resetada" : "Falha no reset",
                              cleared ? "Reiniciando..." : "Tente novamente",
                              cleared ? COLOR_OK : COLOR_BAD);
          delay(700);
          if (cleared) {
            preferences.end();
            ESP.restart();
          }
        }
        setAppState(STATE_SETTINGS);
      } else if (event.type == EVT_OK_LONG) {
        setAppState(STATE_SETTINGS);
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
    case STATE_RESET_CONFIRM:
      drawResetConfirmation();
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
  tft.initR(TFT_INIT_PROFILE);
  tft.setSPISpeed(40000000);
  tft.invertDisplay(false);
  tft.enableDisplay(true);
  tft.setRotation(TFT_APP_ROTATION);
  tft.fillScreen(ST77XX_BLACK);
}

void setup() {
#if ENABLE_SERIAL_DEBUG
  Serial.begin(115200);
  delay(100);
  Serial.println();
  Serial.println("PixieCam iniciando...");
#endif

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
  loadButtonCalibration();
  Serial.printf("Preferencia flash: %s\n", flashEnabled ? "ON" : "OFF");

  initializeDisplay();
  drawBootAnimation();
  bootFinished = true;

  if (!buttonCalibrationValid) {
    runButtonCalibration();
  }

  initializeSDCard();

  bool previousCameraBootInterrupted = cameraBootGuard == CAMERA_BOOT_GUARD_MAGIC;
  safeCameraBoot = preferences.getBool("cam_safe", false) || previousCameraBootInterrupted;
  if (previousCameraBootInterrupted) {
    preferences.putBool("cam_safe", true);
  }
  cameraBootGuard = CAMERA_BOOT_GUARD_MAGIC;

  if (safeCameraBoot) {
    Serial.println("Boot anterior interrompido durante a camera; usando perfil seguro.");
  }

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
