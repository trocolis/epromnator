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

| # | Etapa                                         | Critério de aceite                         | Status |
|---|-----------------------------------------------|--------------------------------------------|--------|
| 0 | Estrutura do repositório + plano              | repositório organizado                     | ✅ |
| 1 | Revisão do hardware                           | LED corrigido, C3 com valor definido       | ✅ (netlist conferida) |
| 2 | HAL do firmware + shell de depuração texto    | ler/gravar 1 byte pelo monitor serial      | ✅ testado na placa |
| 3 | Driver 28C256 (página + polling + SDP)        | gravar e ler 32 KB sem erro (`t !`)        | ✅ testado na placa (32 KB, 0 erros) |
| 4 | Protocolo binário + testes nativos            | `pio test -e native` + script Python       | ✅ 11 testes + script na placa |
| 5 | Core C++ + formatos de arquivo + emulador     | testes unitários passando                  | ✅ 46 testes |
| 6 | CLI                                           | ler/gravar/verificar/apagar                | ✅ testada na placa |
| 7 | GUI Qt 6                                      | todas as operações com progresso e log     | ✅ (emulador) |
| 8 | Recursos avançados (AUTO, pular 0xFF, diff)   | incorporados nas etapas 5–7                | ✅ |
| 9 | CI + empacotamento + README                   | build Linux/Windows/macOS                  | ✅ local · ⏳ CI no GitHub |

⏳ = depende da placa montada ou de um repositório no GitHub.

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
- **Endereços SDP mascarados pelo tamanho do chip**: no 28C64 o pino 1 é
  RDY/~BUSY e está ligado à linha A14 do 595; as sequências usam 1555/0AAA
  para não provocar conflito elétrico.
- **Tempo de assentamento do barramento de dados (1 µs)**: na primeira
  montagem foram necessários 15 µs, por causa de resistores série de valor
  errado. Com os 220R corrigidos, a varredura na placa real passou com
  125 ns; 1 µs dá 8x de margem. Ver `Bus::SETTLE_CYCLES`.
