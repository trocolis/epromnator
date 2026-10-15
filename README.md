# EPROMnator

Gravador de EEPROMs paralelas **28Cxxx** (AT28C256 e compatíveis) baseado em
**Arduino Nano**, com firmware PlatformIO e software multiplataforma em C++
(CLI e interface gráfica Qt 6).

```
hardware/   esquema KiCad (Nano + 2× 74HC595 + soquete de 28 pinos)
firmware/   firmware do Nano (PlatformIO)
protocol/   protocolo serial compartilhado entre firmware e PC
software/   biblioteca core, CLI, GUI e testes (CMake)
tools/      script Python de teste do firmware
docs/       plano de implementação
```

## Chips suportados

| Chip                | Tamanho | Página | SDP |
|---------------------|---------|--------|-----|
| AT28C256 / AT28C256F| 32 KB   | 64     | sim |
| CAT28C256, X28C256  | 32 KB   | 64     | sim |
| 28C256 (genérico)   | 32 KB   | 64     | sim |
| AT28C64B, X28C64    | 8 KB    | 64     | sim |
| 28C64 (modo byte)   | 8 KB    | —      | não |

Somente EEPROMs de 5 V com pinagem JEDEC de 28 pinos. EPROMs UV (que precisam
de Vpp) e flashes de 32 pinos não são suportadas por este hardware.

---

## Hardware

Mapa de pinos (conferido na netlist):

| Função        | Pino Nano | Porta AVR | Observação          |
|---------------|-----------|-----------|---------------------|
| EEPROM D0–D5  | D2–D7     | PD2–PD7   | série 220R          |
| EEPROM D6–D7  | D8–D9     | PB0–PB1   | série 220R          |
| ~CE / ~OE / ~WE | D10 / D11 / D12 | PB2 / PB3 / PB4 | pull-up 10k |
| SER / SRCLK / RCLK (595) | A3 / A4 / A5 | PC3 / PC4 / PC5 | endereços A0–A15 |
| LED "ocupado" | D13       | PB5       | LED onboard do Nano |

> ⚠️ A EEPROM fica sempre alimentada: **não troque o chip com o USB ligado.**

Correções já aplicadas ao esquema: LED D1 estava invertido; C3 recebeu valor
(10 µF).

> ⚠️ **Resistores série das linhas de dados (R3–R10).** No primeiro teste com a
> placa montada, as linhas de dados levaram microssegundos para assentar (D4/D5,
> R7/R8, as piores). A leitura devolvia o dado do endereço anterior e a escrita
> travava bits errados. O firmware compensa com 15 µs de assentamento
> (`Bus::SETTLE_CYCLES`), mas confira se os resistores são mesmo de **220 Ω**
> (vermelho-vermelho-marrom), não de 22 kΩ (vermelho-vermelho-laranja). Depois
> de corrigidos, o tempo pode voltar a ~1 µs, o que deixa a leitura cerca de
> 0,5 s mais rápida.

---

## Firmware

Requer [PlatformIO](https://platformio.org/).

```sh
cd firmware
pio run -e nano -t upload          # Nano com bootloader novo
pio run -e nano_old -t upload      # clones com bootloader antigo (57600)
pio test -e native                 # testes unitários no PC
```

### Shell de depuração (montagem da placa)

O ambiente `nano_debug` grava um shell de texto, útil para conferir a placa com
multímetro antes de usar o software:

```sh
pio run -e nano_debug -t upload && pio device monitor
# (nano_old_debug para clones com bootloader antigo)
```

| Comando           | Função                                             |
|-------------------|----------------------------------------------------|
| `a <end>`         | fixa um endereço no barramento (medir A0–A14)      |
| `d <byte>` / `z`  | dirige / solta o barramento de dados               |
| `r <end> [qtd]`   | lê bytes                                           |
| `w <end> <b0>...` | grava bytes                                        |
| `b` / `c` / `e`   | verifica vazio / CRC32 / apaga o chip              |
| `s 0` / `s 1`     | desabilita / habilita SDP                          |
| `v [end]`         | mede o VCC (e o mínimo durante a regravação da página) |
| `t !`             | **autoteste destrutivo**: grava e confere o chip inteiro |

### Teste rápido pelo protocolo binário

Depois de gravar o firmware principal (`env:nano`):

```sh
pip install pyserial
tools/fw_smoke_test.py /dev/ttyUSB0                  # não destrutivo
tools/fw_smoke_test.py /dev/ttyUSB0 --destructive    # grava o chip inteiro
```

---

## Software

### Dependências

- CMake ≥ 3.21 e um compilador C++20 (GCC 11+, Clang 14+, MSVC 2022);
- Qt 6 (módulo Widgets), apenas para a GUI;
- GoogleTest (opcional: é baixado automaticamente se não estiver instalado).

| Sistema        | Pacotes                                                             |
|----------------|---------------------------------------------------------------------|
| Arch Linux     | `sudo pacman -S cmake ninja qt6-base qt6-svg gtest`                 |
| Debian/Ubuntu  | `sudo apt install cmake ninja-build qt6-base-dev libgtest-dev`      |
| macOS          | `brew install cmake ninja qt`                                       |
| Windows        | Visual Studio 2022 + Qt 6 (instalador oficial ou `aqtinstall`)      |

### Compilação

```sh
cmake -S software -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Opções: `-DEPN_BUILD_GUI=OFF` (sem Qt), `-DEPN_BUILD_CLI=OFF`,
`-DEPN_BUILD_TESTS=OFF`.

### Permissão de acesso à porta serial (Linux)

```sh
sudo usermod -aG uucp $USER      # Arch
sudo usermod -aG dialout $USER   # Debian/Ubuntu
```

Depois, encerre a sessão e entre de novo.

### Interface gráfica

```sh
build/gui/epromnator-gui [arquivo]
```

Fluxo típico: escolha a **porta** e o **chip** → **Abrir** o arquivo → **AUTO**
(F9). Se não estiver conectado, a primeira operação conecta automaticamente.

| Atalho | Operação        | Atalho  | Operação              |
|--------|-----------------|---------|-----------------------|
| F5     | Ler             | Ctrl+B  | Verificar vazio       |
| F6     | Gravar          | Ctrl+D  | Selecionar chip       |
| F7     | Verificar       | Ctrl+G  | Ir para endereço      |
| F8     | Apagar          | Ctrl+L  | Preencher buffer      |
| F9     | AUTO            | Esc     | Cancelar              |

Recursos:

- editor hexadecimal com edição hex/ASCII, bytes alterados destacados e
  diferenças da verificação em vermelho;
- abrir/salvar binário, Intel HEX e S-Record, arquivos recentes, arrastar e
  soltar;
- HEX montado acima da capacidade do chip (ex.: ROM de 6502 em `0x8000`): o
  programa oferece o deslocamento automaticamente;
- faixa de endereços parcial, comparação com arquivo, preenchimento,
  checksum (soma16 e CRC32);
- escrita protegida, para gravar chips com SDP ativo sem desativar a proteção;
- porta **"Emulador (sem hardware)"**, para experimentar sem a placa.

### Linha de comando

```sh
epromnator-cli list-ports
epromnator-cli -p /dev/ttyUSB0 read dump.bin
epromnator-cli -p /dev/ttyUSB0 write rom.hex --skip-ff
epromnator-cli -p COM3 -d AT28C64B auto rom.bin
epromnator-cli -p /dev/ttyUSB0 verify rom.s19 -o -0x8000
epromnator-cli -p emu auto rom.bin                     # emulador
```

Operações: `read`, `write`, `verify`, `erase`, `blank`, `auto`, `sdp-on`,
`sdp-off`, `info`, `checksum`, `list-devices`, `list-ports`. Use
`epromnator-cli --help` para ver todas as opções.

Códigos de saída: `0` sucesso, `1` erro, `2` falha de verificação ou chip não
vazio, `3` uso incorreto.

---

## Protocolo

O protocolo é binário, no modelo pare-e-espere, com CRC-16/CCITT e número de
sequência. Ao reiniciar (o que acontece ao abrir a porta, via DTR), o firmware
envia um quadro `HELLO`. A especificação completa está em
[protocol/epromnator_protocol.h](protocol/epromnator_protocol.h).

| Comando        | Função                                     |
|----------------|--------------------------------------------|
| `GET_INFO`     | versão e limites do firmware               |
| `CONFIG`       | tamanho, página, timeout e escrita protegida |
| `READ`         | até 256 bytes                              |
| `WRITE`        | até 64 bytes (uma página)                  |
| `BLANK_CHECK`  | primeiro byte diferente de 0xFF            |
| `CRC32`        | CRC de uma faixa (verificação rápida)      |
| `ERASE`        | preenche com 0xFF, pulando páginas vazias  |
| `SDP_ENABLE` / `SDP_DISABLE` | proteção de dados por software |

Para a verificação, o firmware calcula o CRC32 de cada bloco de 1 KB e só os
blocos que divergem são lidos byte a byte.

---

## Testes e CI

Validado em hardware (Nano clone CH340, bootloader antigo, AT28C256):

- 32 KB gravados em 10,4 s, lidos em 3,5 s, com 0 erros e CRC32 conferido
  (`tools/fw_smoke_test.py --destructive`);
- AUTO completo com dados aleatórios em 17 s; detecção de diferença de 1 bit
  no endereço exato;
- SDP: com a proteção ativa, a escrita comum é bloqueada e a protegida passa;
  depois de desabilitada, a escrita comum volta a funcionar.

Testes automatizados:

- `firmware/test`: 11 testes Unity (framing e CRC), com `pio test -e native`;
- `software/tests`: 46 testes GTest (formatos de arquivo, protocolo e operações
  contra o emulador, que usa o mesmo `FrameParser` do firmware);
- `.github/workflows/ci.yml`: compila o firmware e o software em Linux, Windows
  e macOS e gera AppImage, `.zip` e `.dmg`. Em tags `v*`, publica um release.
