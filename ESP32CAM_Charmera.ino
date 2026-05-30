#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>

// =====================================================
// PROJETO: ESP32-CAM estilo mini câmera retrô
// Funções:
// - Cria rede Wi-Fi própria
// - Página web pelo celular
// - Visualização ao vivo
// - Tirar foto
// - Gravar vídeo pelo navegador do celular
// - Aplicar filtros visuais na tela/foto/vídeo gravado
// =====================================================

// ====== CONFIGURAÇÕES DO WI-FI ======
const char* AP_SSID = "ESP32-CAM-CHARMERA";
const char* AP_PASS = "12345678"; // mínimo 8 caracteres

WebServer server(80);

// ====== PINOS DO ESP32-CAM AI THINKER ======
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

// Flash LED do AI Thinker geralmente fica no GPIO 4
#define FLASH_LED_PIN      4

// ====== HTML DA INTERFACE ======
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=no" />
  <title>ESP32-CAM Charmera</title>
  <style>
    :root {
      --bg: #101014;
      --card: #1b1b22;
      --text: #f5f5f5;
      --muted: #aaa;
      --accent: #ffcc66;
      --danger: #ff4d4d;
      --ok: #31d27c;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      background: radial-gradient(circle at top, #2a2231, var(--bg));
      color: var(--text);
      font-family: Arial, Helvetica, sans-serif;
      min-height: 100vh;
      padding: 14px;
    }
    .app {
      max-width: 520px;
      margin: 0 auto;
    }
    .header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 12px;
      margin-bottom: 12px;
    }
    h1 {
      font-size: 22px;
      margin: 0;
      letter-spacing: 0.5px;
    }
    .badge {
      font-size: 12px;
      color: #111;
      background: var(--accent);
      padding: 6px 9px;
      border-radius: 999px;
      font-weight: bold;
      white-space: nowrap;
    }
    .camera-card {
      background: var(--card);
      border-radius: 24px;
      padding: 12px;
      box-shadow: 0 12px 30px rgba(0,0,0,.35);
      border: 1px solid rgba(255,255,255,.08);
    }
    .viewer-wrap {
      position: relative;
      overflow: hidden;
      border-radius: 18px;
      background: #000;
      aspect-ratio: 4 / 3;
      display: flex;
      align-items: center;
      justify-content: center;
    }
    #stream {
      width: 100%;
      height: 100%;
      object-fit: cover;
      transform: scaleX(-1);
    }
    .frame {
      pointer-events: none;
      position: absolute;
      inset: 12px;
      border: 2px solid rgba(255,255,255,.55);
      border-radius: 14px;
      display: none;
    }
    .frame.on { display: block; }
    .status {
      margin: 10px 2px 0;
      color: var(--muted);
      font-size: 13px;
      line-height: 1.35;
    }
    .controls {
      margin-top: 12px;
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 10px;
    }
    button, select {
      border: 0;
      border-radius: 14px;
      padding: 13px 12px;
      font-size: 15px;
      font-weight: bold;
      color: var(--text);
      background: #2d2d38;
      outline: none;
    }
    button:active { transform: scale(.98); }
    .photo { background: #2d78ff; }
    .record { background: var(--danger); }
    .stop { background: #555; }
    .flash { background: #8b5cf6; }
    .full { grid-column: 1 / -1; }
    select { width: 100%; }
    .preview {
      margin-top: 12px;
      display: none;
      background: rgba(255,255,255,.05);
      border-radius: 18px;
      padding: 10px;
    }
    .preview img, .preview video, canvas {
      width: 100%;
      border-radius: 14px;
      background: #000;
    }
    a.download {
      display: block;
      text-align: center;
      color: #111;
      background: var(--accent);
      text-decoration: none;
      padding: 12px;
      border-radius: 14px;
      margin-top: 10px;
      font-weight: bold;
    }
    .hint {
      color: var(--muted);
      font-size: 12px;
      margin-top: 10px;
    }

    /* Filtros */
    .f-normal { filter: none; }
    .f-pb { filter: grayscale(1) contrast(1.15); }
    .f-sepia { filter: sepia(1) contrast(1.05) saturate(.85); }
    .f-vintage { filter: sepia(.55) contrast(1.2) saturate(1.4) brightness(.95); }
    .f-frio { filter: hue-rotate(180deg) saturate(1.25) contrast(1.05); }
    .f-quente { filter: sepia(.35) saturate(1.8) contrast(1.08); }
    .f-dream { filter: brightness(1.18) contrast(.9) saturate(1.4) blur(.2px); }
    .f-noir { filter: grayscale(1) contrast(1.7) brightness(.85); }
  </style>
</head>
<body>
  <div class="app">
    <div class="header">
      <h1>ESP32-CAM Charmera</h1>
      <div class="badge">Wi-Fi próprio</div>
    </div>

    <div class="camera-card">
      <div class="viewer-wrap">
        <img id="stream" class="f-normal" src="/stream" crossorigin="anonymous" />
        <div id="frame" class="frame"></div>
      </div>
      <div class="status" id="status">Conectado. Escolha um filtro e tire uma foto ou grave um vídeo.</div>

      <div class="controls">
        <select id="filterSelect" class="full">
          <option value="f-normal">Filtro: Normal</option>
          <option value="f-pb">Filtro: Preto e branco</option>
          <option value="f-sepia">Filtro: Sépia</option>
          <option value="f-vintage">Filtro: Vintage</option>
          <option value="f-quente">Filtro: Quente</option>
          <option value="f-frio">Filtro: Frio</option>
          <option value="f-dream">Filtro: Dream</option>
          <option value="f-noir">Filtro: Noir</option>
        </select>

        <button class="photo" onclick="takePhoto()">📸 Tirar foto</button>
        <button id="recordBtn" class="record" onclick="toggleRecord()">⏺ Gravar</button>
        <button class="flash" onclick="flashOn()">⚡ Flash</button>
        <button onclick="toggleFrame()">▢ Moldura</button>
      </div>

      <div class="hint">
        O vídeo é gravado pelo navegador do celular usando a imagem ao vivo. Para melhor resultado, use Chrome/Edge no Android.
      </div>
    </div>

    <div class="preview" id="preview"></div>
  </div>

<script>
const streamImg = document.getElementById('stream');
const filterSelect = document.getElementById('filterSelect');
const statusEl = document.getElementById('status');
const preview = document.getElementById('preview');
const frame = document.getElementById('frame');
const recordBtn = document.getElementById('recordBtn');

let mediaRecorder = null;
let recordedChunks = [];
let drawTimer = null;
let recordingCanvas = null;
let frameOn = false;

filterSelect.addEventListener('change', () => {
  streamImg.className = filterSelect.value;
  statusEl.textContent = 'Filtro aplicado: ' + filterSelect.options[filterSelect.selectedIndex].text.replace('Filtro: ', '');
});

function cssFilterForClass(cls) {
  const map = {
    'f-normal': 'none',
    'f-pb': 'grayscale(1) contrast(1.15)',
    'f-sepia': 'sepia(1) contrast(1.05) saturate(.85)',
    'f-vintage': 'sepia(.55) contrast(1.2) saturate(1.4) brightness(.95)',
    'f-frio': 'hue-rotate(180deg) saturate(1.25) contrast(1.05)',
    'f-quente': 'sepia(.35) saturate(1.8) contrast(1.08)',
    'f-dream': 'brightness(1.18) contrast(.9) saturate(1.4) blur(.2px)',
    'f-noir': 'grayscale(1) contrast(1.7) brightness(.85)'
  };
  return map[cls] || 'none';
}

function drawFilteredToCanvas(canvas) {
  const ctx = canvas.getContext('2d');
  const w = canvas.width;
  const h = canvas.height;
  ctx.save();
  ctx.clearRect(0, 0, w, h);
  ctx.filter = cssFilterForClass(filterSelect.value);
  ctx.translate(w, 0);
  ctx.scale(-1, 1);
  ctx.drawImage(streamImg, 0, 0, w, h);
  ctx.restore();

  if (frameOn) {
    ctx.save();
    ctx.strokeStyle = 'rgba(255,255,255,.85)';
    ctx.lineWidth = 8;
    ctx.strokeRect(28, 28, w - 56, h - 56);
    ctx.restore();
  }
}

function takePhoto() {
  const canvas = document.createElement('canvas');
  canvas.width = 640;
  canvas.height = 480;
  drawFilteredToCanvas(canvas);
  const dataUrl = canvas.toDataURL('image/jpeg', 0.92);

  preview.style.display = 'block';
  preview.innerHTML = '';
  const img = document.createElement('img');
  img.src = dataUrl;
  const link = document.createElement('a');
  link.href = dataUrl;
  link.download = 'esp32cam-charmera-foto.jpg';
  link.className = 'download';
  link.textContent = 'Baixar foto';
  preview.appendChild(img);
  preview.appendChild(link);
  statusEl.textContent = 'Foto criada com filtro aplicado.';
}

function toggleFrame() {
  frameOn = !frameOn;
  frame.classList.toggle('on', frameOn);
  statusEl.textContent = frameOn ? 'Moldura ativada.' : 'Moldura desativada.';
}

function flashOn() {
  fetch('/flash').then(() => {
    statusEl.textContent = 'Flash acionado.';
  }).catch(() => {
    statusEl.textContent = 'Não foi possível acionar o flash.';
  });
}

function toggleRecord() {
  if (mediaRecorder && mediaRecorder.state === 'recording') {
    stopRecording();
  } else {
    startRecording();
  }
}

function startRecording() {
  if (!HTMLCanvasElement.prototype.captureStream || !window.MediaRecorder) {
    statusEl.textContent = 'Seu navegador não suporta gravação por canvas/MediaRecorder.';
    return;
  }

  recordingCanvas = document.createElement('canvas');
  recordingCanvas.width = 640;
  recordingCanvas.height = 480;

  const canvasStream = recordingCanvas.captureStream(12); // 12 fps para aliviar o ESP32-CAM/celular
  recordedChunks = [];

  let options = {};
  if (MediaRecorder.isTypeSupported('video/webm;codecs=vp9')) {
    options.mimeType = 'video/webm;codecs=vp9';
  } else if (MediaRecorder.isTypeSupported('video/webm;codecs=vp8')) {
    options.mimeType = 'video/webm;codecs=vp8';
  } else {
    options.mimeType = 'video/webm';
  }

  mediaRecorder = new MediaRecorder(canvasStream, options);
  mediaRecorder.ondataavailable = e => {
    if (e.data && e.data.size > 0) recordedChunks.push(e.data);
  };
  mediaRecorder.onstop = showRecordedVideo;

  drawTimer = setInterval(() => drawFilteredToCanvas(recordingCanvas), 83);
  mediaRecorder.start();

  recordBtn.textContent = '⏹ Parar';
  recordBtn.className = 'stop';
  statusEl.textContent = 'Gravando vídeo com filtro aplicado...';
}

function stopRecording() {
  clearInterval(drawTimer);
  drawTimer = null;
  if (mediaRecorder && mediaRecorder.state === 'recording') mediaRecorder.stop();
  recordBtn.textContent = '⏺ Gravar';
  recordBtn.className = 'record';
}

function showRecordedVideo() {
  const blob = new Blob(recordedChunks, { type: 'video/webm' });
  const url = URL.createObjectURL(blob);

  preview.style.display = 'block';
  preview.innerHTML = '';

  const video = document.createElement('video');
  video.src = url;
  video.controls = true;
  video.autoplay = false;

  const link = document.createElement('a');
  link.href = url;
  link.download = 'esp32cam-charmera-video.webm';
  link.className = 'download';
  link.textContent = 'Baixar vídeo';

  preview.appendChild(video);
  preview.appendChild(link);
  statusEl.textContent = 'Vídeo finalizado. Você pode baixar o arquivo WebM.';
}
</script>
</body>
</html>
)rawliteral";

// ====== INICIALIZA A CÂMERA ======
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

  if (psramFound()) {
    config.frame_size = FRAMESIZE_VGA;   // 640x480
    config.jpeg_quality = 12;            // menor = melhor qualidade
    config.fb_count = 2;
    config.grab_mode = CAMERA_GRAB_LATEST;
  } else {
    config.frame_size = FRAMESIZE_QVGA;  // 320x240 se não tiver PSRAM
    config.jpeg_quality = 15;
    config.fb_count = 1;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Erro ao iniciar câmera: 0x%x\n", err);
    return false;
  }

  sensor_t * s = esp_camera_sensor_get();
  if (s) {
    s->set_brightness(s, 0);
    s->set_contrast(s, 1);
    s->set_saturation(s, 0);
    s->set_framesize(s, psramFound() ? FRAMESIZE_VGA : FRAMESIZE_QVGA);
  }

  return true;
}

// ====== PÁGINA PRINCIPAL ======
void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

// ====== FOTO JPG DIRETA ======
void handleCapture() {
  camera_fb_t * fb = esp_camera_fb_get();
  if (!fb) {
    server.send(500, "text/plain", "Erro ao capturar foto");
    return;
  }

  server.sendHeader("Content-Disposition", "inline; filename=capture.jpg");
  server.send_P(200, "image/jpeg", (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
}

// ====== STREAM MJPEG ======
void handleStream() {
  WiFiClient client = server.client();

  String response = "HTTP/1.1 200 OK\r\n";
  response += "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";
  server.sendContent(response);

  while (client.connected()) {
    camera_fb_t * fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Falha ao obter frame");
      break;
    }

    response = "--frame\r\n";
    response += "Content-Type: image/jpeg\r\n";
    response += "Content-Length: " + String(fb->len) + "\r\n\r\n";

    server.sendContent(response);
    client.write(fb->buf, fb->len);
    server.sendContent("\r\n");

    esp_camera_fb_return(fb);

    // Pequeno delay para estabilidade
    delay(80);
  }
}

// ====== FLASH ======
void handleFlash() {
  digitalWrite(FLASH_LED_PIN, HIGH);
  delay(180);
  digitalWrite(FLASH_LED_PIN, LOW);
  server.send(200, "text/plain", "OK");
}

void handleNotFound() {
  server.send(404, "text/plain", "Página não encontrada");
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);

  Serial.println();
  Serial.println("Iniciando ESP32-CAM Charmera...");

  if (!initCamera()) {
    Serial.println("Camera falhou. Verifique modelo/pinos/alimentação.");
    while (true) {
      digitalWrite(FLASH_LED_PIN, HIGH);
      delay(150);
      digitalWrite(FLASH_LED_PIN, LOW);
      delay(850);
    }
  }

  WiFi.mode(WIFI_AP);
  bool ok = WiFi.softAP(AP_SSID, AP_PASS);

  if (ok) {
    Serial.println("Rede Wi-Fi criada com sucesso!");
    Serial.print("SSID: ");
    Serial.println(AP_SSID);
    Serial.print("Senha: ");
    Serial.println(AP_PASS);
    Serial.print("Acesse: http://");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.println("Erro ao criar rede Wi-Fi");
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/capture", HTTP_GET, handleCapture);
  server.on("/stream", HTTP_GET, handleStream);
  server.on("/flash", HTTP_GET, handleFlash);
  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("Servidor iniciado.");
}

void loop() {
  server.handleClient();
}
