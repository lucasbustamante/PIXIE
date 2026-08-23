# Diagnostico de captura e microSD - PixieCam 1.2.0

Depois de apertar OK na tela Camera, o firmware mantem o resultado visivel.

- `Salva 0001`: o JPEG foi gravado, fechado, reaberto e teve o tamanho conferido.
- `CAM E1 iniciar`: a camera nao iniciou no perfil de foto.
- `CAM E2 JPEG`: a camera nao entregou um JPEG completo nem no perfil seguro.
- `CAM E3 invalido`: o framebuffer recebido nao era um JPEG utilizavel.
- `CAM E4 memoria`: faltou RAM/PSRAM para preservar o JPEG antes de montar o SD.
- `SD E1 GPIO2 LOW`: DAT0 esta preso em nivel baixo. Solte os botoes; se continuar,
  o ladder no GPIO2 precisa de correcao eletrica/pull-up para 3,3 V.
- `SD E2 pinos`: o driver recusou CLK=14, CMD=15 ou DAT0=2.
- `SD E3 montar`: nenhuma tentativa em 20/10/5 MHz montou o volume FAT.
- `SD E4 sem cartao`: o controlador iniciou, mas nao detectou um cartao.
- `SD E5 criar DCIM`: nao foi possivel criar `/DCIM`.
- `SD E6 abrir`: nao foi possivel abrir o novo arquivo para escrita.
- `SD E7 gravar`: a quantidade escrita foi menor que o JPEG.
- `SD E8 conferir`: o arquivo reaberto tem tamanho diferente do esperado.
- `SD E9 cheio`: nao ha espaco livre suficiente.
- `SD E10 nome`: nao foi encontrado um nome livre entre `PHOTO_0001.jpg` e
  `PHOTO_9999.jpg`.
- `SD E11 formatar`: a API FAT nao conseguiu formatar o cartao.
- `SD E12 memoria`: faltou memoria para a tarefa de montagem/formatacao.
- `SD E13 verificar`: a formatacao terminou, mas o volume nao passou no remount.
- `SD E14 JPEG vazio`: a rotina de escrita recebeu um JPEG vazio.
- `SD E15 listar`: `/DCIM` existe, mas nao pode ser aberta pela galeria.

O codigo exato separa falha de software de incompatibilidade eletrica. Em
especial, `SD E1 GPIO2 LOW` confirma conflito fisico no pino compartilhado pelo
ladder de botoes e pelo DAT0 do slot microSD.
