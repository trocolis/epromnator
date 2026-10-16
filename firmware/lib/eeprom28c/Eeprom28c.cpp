// =============================================================================
//  Eeprom28c.cpp — implementação do driver 28Cxxx
// =============================================================================
#include "Eeprom28c.h"

#include <Arduino.h>
#include <string.h>
#include <util/atomic.h>

#include "Bus.h"
#include "epromnator_protocol.h"

namespace Eeprom {

namespace {

// Configuração padrão: AT28C256 (32 KB, páginas de 64 bytes, tWC = 10 ms,
// usamos 15 ms de limite para ter margem).
Config current = {32768UL, 64, 15, false};

// Endereços "mágicos" das sequências SDP (valores do 28C256).
constexpr uint16_t SDP_ADDR_1 = 0x5555;
constexpr uint16_t SDP_ADDR_2 = 0x2AAA;

// Máscara de endereço do chip configurado (ex.: 0x7FFF no 28C256, 0x1FFF no
// 28C64). Aplicada aos endereços SDP: no 28C64 eles viram 1555/0AAA.
//
// Isso é IMPORTANTE no 28C64: o pino 1 dele é RDY/~BUSY (saída open-drain)
// e, nesta placa, está ligado à linha A14 do 74HC595. Usando 0x5555 o 595
// colocaria A14 em nível alto enquanto o chip puxa o pino para baixo
// (conflito elétrico). Com a máscara, A13/A14 ficam sempre em nível baixo.
inline uint16_t addressMask() {
    return (uint16_t)(current.size - 1);
}

// ---------------------------------------------------------------------------
//  Primitivas internas de escrita
// ---------------------------------------------------------------------------

// Prepara o barramento para uma sequência de cargas de bytes:
// saídas da EEPROM desligadas, dados como saída e chip selecionado.
inline void beginWrite() {
    Bus::oeHigh();
    Bus::dataAsOutput();
    Bus::ceLow();
}

// Libera o barramento ao fim de uma sequência de cargas.
inline void endWrite() {
    Bus::ceHigh();
    Bus::dataAsInput();
}

// "Carga" de um byte: um pulso em ~WE com endereço e dado estáveis.
// O endereço é travado na borda de DESCIDA de ~WE e o dado na de SUBIDA.
// Precisa ser rápida: entre duas cargas não podem passar mais de 150 µs.
inline void loadByte(uint16_t address, uint8_t value) {
    Bus::setAddress(address);
    Bus::writeData(value);
    // O dado precisa estar estável na borda de subida de ~WE (ver
    // Bus::settleDelay sobre as linhas lentas da montagem).
    Bus::settleDelay();
    Bus::weLow();
    Bus::writePulseDelay();
    Bus::weHigh();
}

// Sequência de 3 bytes que precede uma escrita quando o SDP está ativo
// (e que, sozinha, também habilita o SDP).
inline void loadSdpUnlock(uint16_t mask = addressMask()) {
    loadByte(SDP_ADDR_1 & mask, 0xAA);
    loadByte(SDP_ADDR_2 & mask, 0x55);
    loadByte(SDP_ADDR_1 & mask, 0xA0);
}

// Aguarda o fim do ciclo de gravação usando "data polling": enquanto a
// EEPROM está gravando, a leitura do último endereço carregado devolve o
// bit D7 INVERTIDO em relação ao dado escrito. Quando D7 volta a ser igual
// ao dado, o ciclo terminou.
Result waitWriteComplete(uint16_t lastAddress, uint8_t lastValue) {
    Bus::dataAsInput();
    Bus::setAddress(lastAddress);
    Bus::ceLow();

    const uint32_t limitUs = (uint32_t)current.writeTimeoutMs * 1000UL;
    const uint32_t start = micros();
    Result result = Result::Timeout;

    while ((uint32_t)(micros() - start) < limitUs) {
        // Cada leitura é feita com um pulso em ~OE.
        Bus::oeLow();
        Bus::accessDelay();
        const uint8_t value = Bus::readData();
        Bus::oeHigh();

        if (((value ^ lastValue) & 0x80) == 0) {
            result = Result::Ok;
            break;
        }
    }

    Bus::ceHigh();
    return result;
}

// Confere se os bytes gravados estão corretos, relendo a faixa.
Result verifyRange(uint16_t address, const uint8_t* data, uint8_t count,
                   uint16_t* failAddress) {
    for (uint8_t i = 0; i < count; ++i) {
        if (readByte(address + i) != data[i]) {
            if (failAddress) *failAddress = address + i;
            return Result::VerifyError;
        }
    }
    return Result::Ok;
}

// Grava até uma página inteira em UM ciclo de gravação.
Result writeOneCycle(uint16_t address, const uint8_t* data, uint8_t count,
                     uint16_t* failAddress) {
    // As cargas de bytes precisam acontecer sem interrupções longas entre
    // elas (tBLC ≤ 150 µs). Desligamos as interrupções só durante as cargas;
    // o host está aguardando a resposta, então a serial está ociosa.
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        beginWrite();
        if (current.protectedWrite) loadSdpUnlock();
        for (uint8_t i = 0; i < count; ++i) {
            loadByte(address + i, data[i]);
        }
        endWrite();
    }

    // Com as interrupções ligadas de novo (micros() depende delas),
    // aguardamos o fim do ciclo de gravação.
    const uint16_t last = address + count - 1;
    if (waitWriteComplete(last, data[count - 1]) != Result::Ok) {
        if (failAddress) *failAddress = last;
        return Result::Timeout;
    }
    return verifyRange(address, data, count, failAddress);
}

// Tamanho efetivo de página (1 quando o chip só aceita modo byte).
inline uint8_t effectivePageSize() {
    return current.pageSize > 1 ? current.pageSize : 1;
}

}  // namespace

// =============================================================================
//  Configuração
// =============================================================================

void configure(const Config& config) {
    current = config;

    // Saturação de valores inválidos.
    if (current.size == 0 || current.size > 65536UL) current.size = 65536UL;
    if (current.pageSize > MAX_PAGE_SIZE) current.pageSize = MAX_PAGE_SIZE;
    if (current.writeTimeoutMs == 0) current.writeTimeoutMs = 15;
}

const Config& config() { return current; }

// =============================================================================
//  Leitura
// =============================================================================

uint8_t readByte(uint16_t address) {
    Bus::dataAsInput();
    Bus::setAddress(address);
    Bus::ceLow();
    Bus::oeLow();
    Bus::accessDelay();
    const uint8_t value = Bus::readData();
    Bus::oeHigh();
    Bus::ceHigh();
    return value;
}

void readBlock(uint16_t address, uint8_t* out, uint16_t count) {
    Bus::dataAsInput();
    Bus::ceLow();
    Bus::oeLow();
    for (uint16_t i = 0; i < count; ++i) {
        // Com ~CE e ~OE ativos, basta trocar o endereço e aguardar tACC.
        Bus::setAddress(address + i);
        Bus::accessDelay();
        out[i] = Bus::readData();
    }
    Bus::oeHigh();
    Bus::ceHigh();
}

// =============================================================================
//  Gravação
// =============================================================================

Result write(uint16_t address, const uint8_t* data, uint8_t count,
             uint16_t* failAddress) {
    if (count == 0) return Result::Ok;
    if ((uint32_t)address + count > current.size) return Result::RangeError;

    const uint8_t page = effectivePageSize();

    if (page == 1) {
        // Modo byte: um ciclo de gravação para cada byte.
        for (uint8_t i = 0; i < count; ++i) {
            const Result r = writeOneCycle(address + i, &data[i], 1, failAddress);
            if (r != Result::Ok) return r;
        }
        return Result::Ok;
    }

    // Modo página: o primeiro e o último byte precisam estar na mesma página
    // (os bits de endereço acima do tamanho da página devem ser iguais).
    const uint16_t last = address + count - 1;
    if ((address / page) != (last / page)) return Result::RangeError;

    return writeOneCycle(address, data, count, failAddress);
}

Result erase(uint16_t address, uint32_t count, uint16_t* failAddress) {
    if ((uint32_t)address + count > current.size) return Result::RangeError;

    // Buffer de uma página cheia de 0xFF (64 bytes na pilha).
    uint8_t blank[MAX_PAGE_SIZE];
    memset(blank, 0xFF, sizeof(blank));

    const uint8_t page = effectivePageSize();
    uint32_t a = address;
    const uint32_t end = (uint32_t)address + count;

    while (a < end) {
        // Tamanho deste pedaço: até o fim da página atual ou da faixa.
        uint32_t chunk = page - (a % page);
        if (chunk > end - a) chunk = end - a;

        // Só grava se houver algum byte diferente de 0xFF (economiza tempo
        // e ciclos de gravação do chip).
        if (!isBlank((uint16_t)a, chunk)) {
            const Result r = write((uint16_t)a, blank, (uint8_t)chunk, failAddress);
            if (r != Result::Ok) return r;
        }
        a += chunk;
    }
    return Result::Ok;
}

// =============================================================================
//  SDP
// =============================================================================

void sdpEnable() {
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        beginWrite();
        loadSdpUnlock();
        endWrite();
    }
    // A sequência inicia um ciclo interno; aguardamos o tWC máximo com o
    // endereço em 0 (todas as linhas altas desligadas).
    Bus::setAddress(0);
    delay(current.writeTimeoutMs);
}

void sdpDisable() {
    const uint16_t a1 = SDP_ADDR_1 & addressMask();
    const uint16_t a2 = SDP_ADDR_2 & addressMask();
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        beginWrite();
        loadByte(a1, 0xAA);
        loadByte(a2, 0x55);
        loadByte(a1, 0x80);
        loadByte(a1, 0xAA);
        loadByte(a2, 0x55);
        loadByte(a1, 0x20);
        endWrite();
    }
    Bus::setAddress(0);
    delay(current.writeTimeoutMs);
}

// =============================================================================
//  Consultas
// =============================================================================

bool isBlank(uint16_t address, uint32_t count, uint16_t* firstUsed,
             uint8_t* value) {
    Bus::dataAsInput();
    Bus::ceLow();
    Bus::oeLow();
    bool blank = true;
    for (uint32_t i = 0; i < count; ++i) {
        const uint16_t a = (uint16_t)(address + i);
        Bus::setAddress(a);
        Bus::accessDelay();
        const uint8_t v = Bus::readData();
        if (v != 0xFF) {
            if (firstUsed) *firstUsed = a;
            if (value) *value = v;
            blank = false;
            break;
        }
    }
    Bus::oeHigh();
    Bus::ceHigh();
    return blank;
}

uint32_t crc32(uint16_t address, uint32_t count) {
    // CRC32 bit a bit (sem tabela, para economizar 1 KB de flash/RAM),
    // usando as mesmas funções do protocolo que o software do PC usa.
    uint32_t crc = EPN_CRC32_INIT;

    Bus::dataAsInput();
    Bus::ceLow();
    Bus::oeLow();
    for (uint32_t i = 0; i < count; ++i) {
        Bus::setAddress((uint16_t)(address + i));
        Bus::accessDelay();
        crc = epn_crc32_update(crc, Bus::readData());
    }
    Bus::oeHigh();
    Bus::ceHigh();
    return epn_crc32_final(crc);
}

// =============================================================================
//  Identificação
// =============================================================================

namespace {

// Limite para o fim de um ciclo de gravação durante a identificação (o
// tWC máximo dos chips suportados é 10 ms).
constexpr uint32_t PROBE_TIMEOUT_US = 20000UL;

// Endereços usados para detectar o tamanho por espelhamento. Só A13 e A15:
// A14 em nível alto entraria em conflito com o RDY/~BUSY do 28C64.
constexpr uint16_t ALIAS_8K  = 0x2000;  // 28C64 não tem A13
constexpr uint16_t ALIAS_32K = 0x8000;  // soquete de 28 pinos não tem A15

// Máscaras das tentativas de escrita protegida: 1555/0AAA (28C64, sem A14)
// e depois 5555/2AAA (28C256 e 28C512).
const uint16_t UNLOCK_MASKS[] = {0x1FFF, 0x7FFF};

// Lê um byte depois de pré-carregar o barramento com `precharge`. Com o
// chip ausente (ou com as saídas desligadas) as linhas flutuam e mantêm o
// valor pré-carregado na sua capacitância; com o chip presente, ele impõe
// o valor real dentro do tempo de acesso.
uint8_t readPrecharged(uint16_t address, uint8_t precharge) {
    Bus::setAddress(address);
    Bus::dataAsOutput();
    Bus::writeData(precharge);
    Bus::settleDelay();
    Bus::dataAsInput();
    Bus::ceLow();
    Bus::oeLow();
    Bus::accessDelay();
    const uint8_t value = Bus::readData();
    Bus::oeHigh();
    Bus::ceHigh();
    return value;
}

// Resposta do chip a uma escrita de teste.
enum class Probe : uint8_t {
    Immediate,  // valor gravado aparece na hora (SRAM)
    Delayed,    // valor aparece depois de um ciclo de gravação (EEPROM)
    Ignored,    // valor nunca aparece (escrita recusada)
};

// Grava `value` em `address` (com a sequência SDP se `unlockMask` != 0) e
// observa quando o valor aparece. Toda leitura é feita com o barramento
// pré-carregado com o complemento, para não confundir a capacitância das
// linhas com o chip respondendo.
Probe probeWrite(uint16_t address, uint8_t value, uint16_t unlockMask,
                 uint32_t* busyUs = nullptr) {
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        beginWrite();
        if (unlockMask) loadSdpUnlock(unlockMask);
        loadByte(address, value);
        endWrite();
    }
    const uint8_t precharge = (uint8_t)~value;
    const uint32_t start = micros();

    // SRAM: o valor aparece na hora. Conferimos de novo depois de 1 ms
    // (além do tBLC de 150 µs), para que uma EEPROM ainda na janela de
    // carga de página não se passe por SRAM.
    if (readPrecharged(address, precharge) == value) {
        delayMicroseconds(1000);
        if (readPrecharged(address, precharge) == value) return Probe::Immediate;
    }

    while ((uint32_t)(micros() - start) < PROBE_TIMEOUT_US) {
        if (readPrecharged(address, precharge) == value) {
            if (busyUs) *busyUs = micros() - start;
            return Probe::Delayed;
        }
    }
    return Probe::Ignored;
}

// Grava em `alias` um valor diferente do que está em 0x0000 e confere se
// ele aparece em 0x0000, ou seja, se o chip não tem a linha de endereço que
// separa os dois. O byte original é restaurado.
bool isAliased(uint16_t alias, uint16_t unlockMask) {
    const uint8_t original = readByte(alias);
    // Conteúdos diferentes não podem ser a mesma célula: sem gravar nada.
    if (readByte(0) != original) return false;

    const uint8_t marker = (uint8_t)~original;
    if (probeWrite(alias, marker, unlockMask) == Probe::Ignored) return false;
    const bool aliased = readByte(0) == marker;
    probeWrite(alias, original, unlockMask);
    return aliased;
}

}  // namespace

Identity identify() {
    Identity id = {EPN_CHIP_NONE, 0, 0};

    // 1) Há chip? Com o soquete vazio, a leitura acompanha a pré-carga.
    const uint16_t probeAddress = 0;
    const uint8_t original = readPrecharged(probeAddress, 0x00);
    if (readPrecharged(probeAddress, 0xFF) != original) return id;

    // 2) Escrita simples; se for ignorada, escrita protegida (SDP ativo).
    const uint8_t marker = (uint8_t)~original;
    uint16_t unlockMask = 0;
    Probe probe = probeWrite(probeAddress, marker, 0, &id.writeTimeUs);
    for (uint8_t i = 0; probe == Probe::Ignored && i < sizeof(UNLOCK_MASKS) / sizeof(UNLOCK_MASKS[0]); ++i) {
        unlockMask = UNLOCK_MASKS[i];
        probe = probeWrite(probeAddress, marker, unlockMask, &id.writeTimeUs);
    }
    if (probe == Probe::Ignored) {
        id.kind = EPN_CHIP_READ_ONLY;
        id.writeTimeUs = 0;
        return id;
    }
    if (probe == Probe::Immediate) {
        id.kind = EPN_CHIP_SRAM;
        id.writeTimeUs = 0;
    } else {
        id.kind = unlockMask ? EPN_CHIP_EEPROM_SDP : EPN_CHIP_EEPROM;
    }
    probeWrite(probeAddress, original, unlockMask);

    // 3) Tamanho por espelhamento: 8 KB sem A13, 32 KB sem A15.
    if (isAliased(ALIAS_8K, unlockMask)) {
        id.size = 8192UL;
    } else if (isAliased(ALIAS_32K, unlockMask)) {
        id.size = 32768UL;
    } else {
        id.size = 65536UL;
    }
    return id;
}

}  // namespace Eeprom
