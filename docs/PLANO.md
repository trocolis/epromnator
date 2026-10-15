# Plano de implementação — EPROMnator

Gravador de EEPROMs paralelas 28Cxxx baseado em Arduino Nano, composto por:

- **hardware/** — esquema KiCad (Nano + 2× 74HC595 + soquete 28 pinos);
- **firmware/** — firmware PlatformIO para o Nano;
- **protocol/** — definição do protocolo serial, compartilhada entre firmware e PC;
- **software/** — biblioteca C++ (core), CLI e GUI Qt 6 multiplataforma.

## Mapa de pinos (conferido na netlist)

| Função          | Pino Nano | Porta AVR | Observação          |
|-----------------|-----------|-----------|---------------------|
| EEPROM D0–D5    | D2–D7     | PD2–PD7   | série 220R          |
| EEPROM D6–D7    | D8–D9     | PB0–PB1   | série 220R          |
| ~CE             | D10       | PB2       | pull-up 10k         |
| ~OE             | D11       | PB3       | pull-up 10k         |
| ~WE             | D12       | PB4       | pull-up 10k         |
| SER (595)       | A3        | PC3       | U1 → U2 em cascata  |
| SRCLK           | A4        | PC4       |                     |
| RCLK            | A5        | PC5       |                     |
| LED "ocupado"   | D13       | PB5       | LED onboard do Nano |

Endereço de 16 bits deslocado **MSB primeiro** (A15 → A0).

## Etapas

| # | Etapa                                         | Critério de aceite                         |
|---|-----------------------------------------------|--------------------------------------------|
| 0 | Estrutura do repositório + plano              | repositório organizado                     |
| 1 | Revisão do hardware                           | LED corrigido, C3 com valor definido       |
| 2 | HAL do firmware + shell de depuração texto    | ler/gravar 1 byte pelo monitor serial      |
| 3 | Driver 28C256 (página + polling + SDP)        | gravar e ler 32 KB sem erro                |
| 4 | Protocolo binário + testes nativos            | `pio test -e native` + script Python       |
| 5 | Core C++ + formatos de arquivo + emulador     | testes unitários passando                  |
| 6 | CLI                                           | ler/gravar/verificar/apagar                |
| 7 | GUI Qt 6                                      | todas as operações com progresso e log     |
| 8 | Recursos avançados (AUTO, pular 0xFF, diff)   | incorporados nas etapas 5–7                |
| 9 | CI + empacotamento + README                   | build Linux/Windows/macOS                  |

## Decisões tomadas

- **Core em C++20 puro (sem Qt)**: a porta serial é implementada diretamente
  (termios no Linux/macOS, Win32 no Windows). Assim a CLI não depende do Qt e a
  GUI não precisa do módulo `QtSerialPort`.
- **Banco de dispositivos embutido** em C++ (`DeviceDatabase.cpp`), sem
  dependência de biblioteca JSON.
- **Emulador** do firmware dentro do core (porta `emu`): permite testar CLI,
  GUI e testes unitários sem a placa.
- **Protocolo binário stop-and-wait** com CRC16-CCITT e número de sequência;
  ver `protocol/epromnator_protocol.h`.
