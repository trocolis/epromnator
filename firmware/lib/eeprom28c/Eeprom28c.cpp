// =============================================================================
//  Eeprom28c.cpp — implementação do driver 28Cxxx
// =============================================================================
#include "Eeprom28c.h"

#include <Arduino.h>
#include <string.h>
#include <util/atomic.h>

#include "Bus.h"

namespace Eeprom {

namespace {

// Configuração padrão: AT28C256 (32 KB, páginas de 64 bytes, tWC = 10 ms,
// usamos 15 ms de limite para ter margem).
Config current = {32768UL, 64, 15, false};

// Endereços "mágicos" das sequências SDP. Valem para toda a família: no
// 28C64 as linhas A13/A14 não existem e os endereços se reduzem a 1555/0AAA,
// o que acontece naturalmente porque esses pinos não estão ligados no chip.
constexpr uint16_t SDP_ADDR_1 = 0x5555;
constexpr uint16_t SDP_ADDR_2 = 0x2AAA;

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
    Bus::weLow();
    Bus::writePulseDelay();
    Bus::weHigh();
}

// Sequência de 3 bytes que precede uma escrita quando o SDP está ativo
// (e que, sozinha, também habilita o SDP).
inline void loadSdpUnlock() {
    loadByte(SDP_ADDR_1, 0xAA);
    loadByte(SDP_ADDR_2, 0x55);
    loadByte(SDP_ADDR_1, 0xA0);
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
    // A sequência inicia um ciclo interno; aguardamos o tWC máximo.
    delay(current.writeTimeoutMs);
}

void sdpDisable() {
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        beginWrite();
        loadByte(SDP_ADDR_1, 0xAA);
        loadByte(SDP_ADDR_2, 0x55);
        loadByte(SDP_ADDR_1, 0x80);
        loadByte(SDP_ADDR_1, 0xAA);
        loadByte(SDP_ADDR_2, 0x55);
        loadByte(SDP_ADDR_1, 0x20);
        endWrite();
    }
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
    // CRC32 bit a bit (sem tabela, para economizar 1 KB de flash/RAM).
    // Polinômio refletido 0xEDB88320, valor inicial e XOR final 0xFFFFFFFF.
    uint32_t crc = 0xFFFFFFFFUL;

    Bus::dataAsInput();
    Bus::ceLow();
    Bus::oeLow();
    for (uint32_t i = 0; i < count; ++i) {
        Bus::setAddress((uint16_t)(address + i));
        Bus::accessDelay();
        crc ^= Bus::readData();
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
        }
    }
    Bus::oeHigh();
    Bus::ceHigh();
    return ~crc;
}

}  // namespace Eeprom
