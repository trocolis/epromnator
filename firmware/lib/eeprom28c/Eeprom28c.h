// =============================================================================
//  Eeprom28c.h — driver para EEPROMs paralelas da família 28Cxxx
// =============================================================================
//
//  Implementa as operações da EEPROM sobre a HAL (Bus.h):
//
//   * leitura de bytes e blocos;
//   * escrita em modo PÁGINA (até 64 bytes por ciclo de gravação) ou em modo
//     BYTE, para chips sem suporte a página;
//   * detecção de fim de gravação por "data polling" (bit D7 invertido);
//   * proteção de dados por software (SDP — Software Data Protection):
//     habilitar, desabilitar e "escrita protegida";
//   * apagamento (preenchimento com 0xFF), pulando páginas já vazias;
//   * verificação de vazio e CRC32 de uma faixa.
//
//  Referência: datasheet Atmel/Microchip AT28C256.
//
//  Tempos relevantes do AT28C256:
//    tWC  (ciclo de gravação)          máx. 10 ms
//    tBLC (intervalo entre bytes)      máx. 150 µs  → por isso a HAL usa
//                                                     acesso direto às portas
//    tWP  (largura do pulso de ~WE)    mín. 100 ns
//    tACC (tempo de acesso)            máx. 150 ns (versão -15)
// =============================================================================
#pragma once

#include <stdint.h>

namespace Eeprom {

// Resultado das operações de gravação.
enum class Result : uint8_t {
    Ok = 0,        // sucesso
    Timeout,       // o data polling não terminou dentro do tempo limite
    VerifyError,   // gravação terminou mas o byte relido é diferente
    RangeError,    // endereço/tamanho fora do chip ou cruzando página
};

// Parâmetros do chip, enviados pelo software do PC (comando CONFIG).
struct Config {
    uint32_t size;            // capacidade em bytes (ex.: 32768 no 28C256)
    uint8_t  pageSize;        // bytes por página; 0 ou 1 = modo byte
    uint8_t  writeTimeoutMs;  // limite do data polling por ciclo (ms)
    bool     protectedWrite;  // precede cada ciclo com a sequência SDP
};

// Maior página suportada pelo firmware (limita buffers na RAM).
constexpr uint8_t MAX_PAGE_SIZE = 64;

// Aplica uma nova configuração. Valores inválidos são saturados.
void configure(const Config& config);

// Configuração atualmente em uso (padrão: AT28C256).
const Config& config();

// ---------------------------------------------------------------------------
//  Leitura
// ---------------------------------------------------------------------------

// Lê um único byte.
uint8_t readByte(uint16_t address);

// Lê `count` bytes consecutivos para `out`, mantendo ~CE ativo durante todo
// o bloco (mais rápido que chamar readByte repetidamente).
void readBlock(uint16_t address, uint8_t* out, uint16_t count);

// ---------------------------------------------------------------------------
//  Gravação
// ---------------------------------------------------------------------------

// Grava `count` bytes a partir de `address`.
//
//  * Em modo página, todos os bytes precisam estar dentro da MESMA página
//    (ex.: 0x0040..0x007F para páginas de 64 bytes); senão retorna RangeError.
//  * Em modo byte, cada byte é um ciclo de gravação separado.
//
// Após a gravação, os bytes são relidos e comparados. Em caso de erro,
// `failAddress` (se não for nulo) recebe o endereço problemático.
Result write(uint16_t address, const uint8_t* data, uint8_t count,
             uint16_t* failAddress = nullptr);

// Preenche a faixa com 0xFF, pulando páginas que já estão vazias.
Result erase(uint16_t address, uint32_t count, uint16_t* failAddress = nullptr);

// ---------------------------------------------------------------------------
//  Proteção de dados por software (SDP)
// ---------------------------------------------------------------------------

// Envia a sequência que HABILITA a proteção (AA→5555, 55→2AAA, A0→5555).
// Com a proteção ativa, gravações só funcionam se precedidas da mesma
// sequência (ver Config::protectedWrite).
void sdpEnable();

// Envia a sequência de 6 bytes que DESABILITA a proteção.
void sdpDisable();

// ---------------------------------------------------------------------------
//  Consultas sobre uma faixa
// ---------------------------------------------------------------------------

// Verifica se todos os bytes da faixa valem 0xFF. Se não, `firstUsed`
// recebe o primeiro endereço com outro valor e `value` o valor lido.
bool isBlank(uint16_t address, uint32_t count, uint16_t* firstUsed = nullptr,
             uint8_t* value = nullptr);

// Calcula o CRC32 (IEEE 802.3, o mesmo do zip) dos bytes da faixa.
uint32_t crc32(uint16_t address, uint32_t count);

}  // namespace Eeprom
