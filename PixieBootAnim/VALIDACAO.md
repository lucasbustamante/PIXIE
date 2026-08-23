# PixieCam 1.2.0 - validacao tecnica

## Resultado

O firmware foi compilado com sucesso para **AI Thinker ESP32-CAM**
(`esp32:esp32:esp32cam`) usando ESP32 Arduino 3.3.11 e avisos habilitados.

- Programa: 551.703 bytes (17% de 3.145.728 bytes)
- RAM global: 76.472 bytes (23% de 327.680 bytes)
- Erros de compilacao: 0
- Avisos do sketch: 0

## Fluxos implementados e conferidos no codigo

1. **Menu:** Camera, Configuracoes, Galeria e Desligar; navegacao por CIMA,
   BAIXO e OK com calibracao do ladder analogico.
2. **Captura:** muda o sensor para o maior perfil JPEG reservado, valida o
   marcador final e as dimensoes, copia o JPEG para memoria independente,
   desliga a camera e so entao monta o SD. Usa qualidade 10, fallback 12 e
   reducao automatica de resolucao se o JPEG de alta resolucao vier incompleto.
3. **Gravacao:** cria `/DCIM`, escolhe um nome sem sobrescrever fotos, grava em
   blocos e reabre o arquivo para conferir o tamanho. Uma falha provoca nova
   tentativa e remontagem do barramento.
4. **Galeria:** lista JPEG/BMP, copia cada arquivo para RAM/PSRAM, desmonta o SD
   e so depois desenha no TFT, evitando uso simultaneo dos pinos compartilhados.
5. **Formatacao pelo menu:** pede confirmacao com `Nao` selecionado por padrao.
   Um FAT montavel usa a formatacao explicita do ESP-IDF; um volume vazio,
   exFAT ou FAT corrompido usa `format_if_mount_failed`. As operacoes longas
   rodam fora do `loop`. Depois, `/DCIM` e criado e o cartao e remontado para
   verificacao antes da mensagem de sucesso.
6. **Diagnostico no aparelho:** o preview permanece pausado durante a mensagem
   final; falhas de camera e armazenamento aparecem com codigo persistente.
7. **Compatibilidade do cartao:** a montagem normal tenta 20, 10 e 5 MHz antes
   de declarar falha.

## Base tecnica pesquisada

- Driver oficial de camera e APIs de framebuffer:
  https://github.com/espressif/esp32-camera/blob/master/driver/include/esp_camera.h
- Implementacao oficial de `SD_MMC.begin`, `setPins` e `end`:
  https://github.com/espressif/arduino-esp32/blob/master/libraries/SD_MMC/src/SD_MMC.cpp
- API e regras oficiais de formatacao FAT:
  https://github.com/espressif/esp-idf/blob/master/components/fatfs/vfs/esp_vfs_fat.h
- Implementacao que particiona/formata quando o volume nao monta:
  https://github.com/espressif/esp-idf/blob/master/components/fatfs/vfs/vfs_fat_sdmmc.c
- Exemplo oficial de montagem e formatacao SDMMC:
  https://github.com/espressif/esp-idf/blob/master/examples/storage/sd_card/sdmmc/main/sd_card_example_main.c
- Relato de falha de SD no AI Thinker e importancia das linhas/pull-ups:
  https://github.com/espressif/arduino-esp32/issues/4680

## Limite da validacao

A compilacao e a revisao foram feitas neste computador. Ha um adaptador CH340
na COM26, mas nenhum dispositivo foi apagado ou gravado automaticamente. O
teste final de camera, botoes, display e cartao depende da montagem fisica. Em
especial, GPIO2/DAT0 precisa ficar em HIGH sem botao pressionado; um pull-down
permanente torna o SD fisicamente impossivel de operar por software.
