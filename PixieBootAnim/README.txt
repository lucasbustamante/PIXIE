PixieCam - ESP32-CAM AI Thinker + ST7735 0.96
================================================

Projeto de camera fotografica digital com ESP32-CAM, display TFT ST7735
160x80 em landscape, botoes fisicos por ladder analogico e armazenamento JPEG
em microSD.

Arquivos do projeto
-------------------

- PixieBootAnim.ino: codigo principal completo.
- boot_animation.h: animacao de inicializacao existente em PROGMEM, preservada.
- README.txt: instrucoes, pinos e dependencias.

Bibliotecas necessarias
-----------------------

Instale pelo Arduino IDE em Library Manager ou pelo arduino-cli:

- ESP32 Arduino core: 3.3.8 recomendado.
- Adafruit GFX Library: 1.12.6 recomendado.
- Adafruit ST7735 and ST7789 Library: 1.11.0 recomendado.
- Adafruit BusIO: 1.17.4 recomendado.
- TJpg_Decoder: 1.1.0 recomendado.

Com arduino-cli:

arduino-cli core install esp32:esp32
arduino-cli lib install "Adafruit GFX Library"
arduino-cli lib install "Adafruit ST7735 and ST7789 Library"
arduino-cli lib install "Adafruit BusIO"
arduino-cli lib install "TJpg_Decoder"

Placa para compilacao
---------------------

Selecione:

- Board: AI Thinker ESP32-CAM
- FQBN: esp32:esp32:esp32cam
- PSRAM: Enabled, se a sua configuracao da IDE exibir essa opcao.

Com arduino-cli:

arduino-cli compile --fqbn esp32:esp32:esp32cam PixieBootAnim
arduino-cli upload -p COMx --fqbn esp32:esp32:esp32cam PixieBootAnim

Troque COMx pela porta serial correta.

Pinos identificados no codigo
-----------------------------

Display ST7735 0.96:

- TFT_CS: GPIO13
- TFT_DC: GPIO15
- TFT_RST: GPIO16
- TFT_MOSI: GPIO12
- TFT_SCLK: GPIO14
- TFT_BL: GPIO1

Botoes e flash:

- BUTTON_ADC_PIN: GPIO2
- FLASH_PIN: GPIO4

Camera ESP32-CAM AI Thinker:

- PWDN: GPIO32
- RESET: -1
- XCLK: GPIO0
- SIOD/SDA: GPIO26
- SIOC/SCL: GPIO27
- Y9: GPIO35
- Y8: GPIO34
- Y7: GPIO39
- Y6: GPIO36
- Y5: GPIO21
- Y4: GPIO19
- Y3: GPIO18
- Y2: GPIO5
- VSYNC: GPIO25
- HREF: GPIO23
- PCLK: GPIO22

Cartao SD no slot interno do ESP32-CAM AI Thinker:

- CLK: GPIO14
- CMD: GPIO15
- D0: GPIO2
- D1/GPIO4 nao e usado no modo 1-bit, ficando disponivel para o flash.

Observacao importante de hardware
---------------------------------

O slot microSD interno do ESP32-CAM AI Thinker compartilha pinos com o TFT e
com o ladder de botoes deste projeto:

- SD CLK usa GPIO14, tambem usado como TFT_SCLK.
- SD CMD usa GPIO15, tambem usado como TFT_DC.
- SD D0 usa GPIO2, tambem usado como BUTTON_ADC_PIN.

Por isso o codigo monta o SD apenas durante operacoes de arquivo e desmonta
logo em seguida, restaurando o barramento SPI do display. Durante leitura ou
gravacao no SD a interface fica com uma tela de status, e depois o display e
redesenhado.

Para evitar travamento no boot em placas onde SD_MMC.begin() demora ou fica
preso nesses pinos compartilhados, a verificacao automatica do SD ao ligar
fica desativada por padrao:

- CHECK_SD_ON_BOOT 0

Assim o dispositivo abre direto a camera. O SD e testado sob demanda ao tirar
foto, abrir Cartao SD ou abrir Galeria. Se o seu hardware estiver estavel e
voce quiser voltar a verificar o SD durante a inicializacao, troque para:

- CHECK_SD_ON_BOOT 1

Se o seu hardware usar outro display ou um modulo SD externo em pinos livres,
ajuste os defines de pinos no inicio do PixieBootAnim.ino.

Orientacao da tela
------------------

O projeto agora usa o display em landscape, virado para a direita:

- TFT_APP_ROTATION 3
- SCREEN_W 160
- SCREEN_H 80

Se no seu hardware a tela ficar de cabeca para baixo, altere somente:

- TFT_APP_ROTATION 1

O restante da interface ja foi redesenhado para 160x80.

Fluxo de inicializacao
----------------------

1. Inicializa Serial, flash, backlight, display e preferencias.
2. Exibe a animacao existente de boot exatamente a partir de boot_animation.h.
3. Verifica PSRAM.
4. Nao monta o SD no boot por padrao, para evitar travamento em pinos
   compartilhados.
5. Inicializa a camera em RGB565 QQVGA para preview.
6. Entra na tela Camera com visualizacao em tempo real.

Estados da interface
--------------------

O sketch usa enum AppState:

- STATE_BOOT
- STATE_CAMERA
- STATE_MAIN_MENU
- STATE_SETTINGS
- STATE_SD_INFO
- STATE_SD_FORMAT_CONFIRM
- STATE_FLASH_SETTINGS
- STATE_ABOUT
- STATE_GALLERY
- STATE_POWER_CONFIRM
- STATE_SHUTDOWN

Controles
---------

Os botoes sao lidos pelo GPIO2 usando os limiares existentes:

- valor < 100: OK/FOTO
- 200 a 900: DOWN
- 900 a 3900: UP

Comportamento:

- Na tela Camera, UP ou DOWN abre o menu principal.
- Na tela Camera, OK tira foto.
- Na tela Camera, segurar OK/FOTO por 3 segundos abre a confirmacao de desligar.
- Nos menus, UP e DOWN navegam.
- Nos menus, OK seleciona.
- Como nao ha botao fisico dedicado de voltar, segurar OK por cerca de 900 ms
  volta para a tela anterior.
- Ha debounce e repeticao controlada para botoes segurados.
- Se um botao ficar pressionado por tempo excessivo, o codigo mostra
  "Solte o botao" e ignora repeticoes.

Menu principal
--------------

Ordem:

1. Camera
2. Configuracoes
3. Galeria
4. Desligar

O item selecionado recebe destaque visual, borda, icone simples e transicao
curta de selecao.

Configuracoes
-------------

Ordem:

1. Cartao SD
2. Flash
3. Sobre
4. Voltar

Cartao SD mostra status, tipo, capacidade total, usado, livre e quantidade de
fotos. Tambem oferece Formatar e Voltar.

Formatar SD nao executa imediatamente. O codigo mostra confirmacao com:

- Sim
- Nao

A selecao padrao e Nao. Ao confirmar Sim, o conteudo do cartao e apagado,
/DCIM e recriado e a numeracao das fotos volta para 1.

Flash alterna entre ON e OFF. A preferencia e salva com Preferences e mantida
apos reiniciar. O LED do flash acende apenas durante a captura.

Sobre mostra nome do projeto, versao, plataforma, descricao curta e um campo
de desenvolvedor editavel no define PROJECT_DEVELOPER.

Captura e armazenamento
-----------------------

A camera usa dois modos:

- Preview: RGB565 em QQVGA, rapido e leve para o display 160x80.
- Captura: JPEG em alta resolucao somente no momento de salvar no SD.

Assim o display pode ficar com qualidade baixa e fluida, enquanto o arquivo
salvo no SD usa a melhor qualidade possivel para a placa.

Resolucao:

- Com PSRAM, o codigo tenta salvar em UXGA 1600x1200 e cai para SXGA, XGA,
  SVGA, VGA ou QVGA se a placa nao aceitar.
- Sem PSRAM, tenta QVGA e depois QQVGA.

O preview volta automaticamente para RGB565 depois da captura.

A escrita no SD usa um buffer global DMA de 512 bytes. O codigo copia o JPEG
em blocos pequenos para esse buffer antes de gravar, evitando gravacao direta
do framebuffer em PSRAM pelo SD_MMC. Depois de fechar o arquivo, ele reabre a
foto e confere o tamanho salvo antes de mostrar a confirmacao.

Nomes de arquivo:

- /DCIM/PHOTO_0001.jpg
- /DCIM/PHOTO_0002.jpg
- /DCIM/PHOTO_0003.jpg

Ao tirar foto ou depois de ler a galeria, o codigo procura o maior numero
existente e define o proximo nome sem sobrescrever arquivos.

Erros tratados visualmente e no Serial:

- SD ausente.
- Falha ao criar /DCIM.
- Falha de camera.
- Framebuffer invalido.
- Espaco insuficiente.
- Falha ao abrir ou escrever arquivo.
- Escrita incompleta.

Galeria
-------

A Galeria le os arquivos .jpg e .bmp de /DCIM, organiza pela numeracao do nome
e permite navegar com UP e DOWN. A tela mostra o nome do arquivo e a posicao,
por exemplo "Foto 2 de 15". As fotos novas sao JPEG; BMP antigo continua sendo
aceito para compatibilidade.

Se nao houver fotos, mostra "Nenhuma foto salva". Se nao houver SD, mostra
"SD nao encontrado".

Como o SD interno compartilha pinos com o display, cada JPEG e lido para um
buffer temporario em PSRAM, quando disponivel, ou heap interna. O SD e entao
desmontado e a imagem e desenhada no TFT com TJpg_Decoder. Isso evita usar o
SD e o display ao mesmo tempo nos mesmos pinos.

O redimensionamento usa escala 1, 2, 4 ou 8 da biblioteca TJpg_Decoder,
preservando proporcao e centralizando a imagem.

Desligamento e deep sleep
-------------------------

A opcao Desligar mostra confirmacao com:

- Sim
- Nao

A selecao padrao e Nao. Ao confirmar Sim:

1. Mostra "Desligando...".
2. Desliga flash.
3. Deinitializa a camera.
4. Desmonta SD se estiver montado.
5. Desliga display e backlight.
6. Entra em deep sleep.

O wakeup configurado e ext0 no GPIO2 em nivel baixo. Na pratica, isso deve
acordar quando o botao OK/FOTO puxar o GPIO2 para baixo. Caso o seu ladder de
botoes nao leve GPIO2 realmente a nivel baixo, ajuste o circuito ou troque a
fonte de wakeup no codigo.

O backlight esta no GPIO1, que tambem e o TX do Serial. Por isso o codigo
encerra o Serial antes de desligar o backlight, coloca GPIO1 em LOW e usa
gpio_hold_en() com gpio_deep_sleep_hold_en() para manter a luz apagada durante
o deep sleep.

Possiveis ajustes de display
----------------------------

Se a imagem estiver invertida, ajuste:

- tft.setRotation(2)
- sensor vflip/hmirror em applySensorDefaults()

Se as cores da galeria aparecerem trocadas, altere:

- TJpgDec.setSwapBytes(false)

para:

- TJpgDec.setSwapBytes(true)

Se o seu ST7735 nao for o modelo mini 160x80, altere:

- tft.initR(INITR_MINI160x80)

para o inicializador correto da sua tela.

Verificacao feita
-----------------

Compilado localmente com:

arduino-cli compile --fqbn esp32:esp32:esp32cam PixieBootAnim

Resultado:

- Sketch: 564163 bytes de flash.
- Variaveis globais: 47200 bytes de RAM.
