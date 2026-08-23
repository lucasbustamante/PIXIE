# Gravacao do PixieCam 1.2.0

## Pelo Arduino IDE

1. Instale o core **esp32 by Espressif Systems 3.3.11** e as bibliotecas
   listadas no `README.txt`.
2. Abra `PixieBootAnim.ino`.
3. Selecione **AI Thinker ESP32-CAM** e a porta do adaptador USB/serial.
4. Ligue GPIO0 ao GND, reinicie a placa e envie o sketch.
5. Remova GPIO0 do GND e reinicie para executar o firmware.

## Com o binario unificado

O pacote inclui `PixieCam-1.2.0-merged.bin`, gerado para flash de 4 MB. Com a
placa em modo de gravacao:

```text
esptool --chip esp32 --port COMx --baud 460800 write-flash 0x0 PixieCam-1.2.0-merged.bin
```

Troque `COMx` pela porta correta. O binario unificado grava a imagem completa
de 4 MB e limpa configuracoes anteriores; no primeiro boot, siga a calibracao
de CIMA, BAIXO e OK mostrada no display.
