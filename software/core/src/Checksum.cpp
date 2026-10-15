// =============================================================================
//  Checksum.cpp
// =============================================================================
#include "epromnator/Checksum.h"

#include <array>

#include "epromnator_protocol.h"

namespace epn {

uint16_t sum16(std::span<const uint8_t> data) {
    uint16_t sum = 0;
    for (uint8_t b : data) sum = static_cast<uint16_t>(sum + b);  // estoura de propósito
    return sum;
}

uint32_t crc32(std::span<const uint8_t> data) {
    // No PC vale a pena usar tabela (256 entradas), calculada uma única vez a
    // partir da mesma função bit a bit do protocolo — garantindo resultado
    // idêntico ao do firmware.
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            // epn_crc32_update aplica 8 iterações sobre (crc ^ byte); com
            // crc = 0 e byte = i obtemos exatamente a entrada i da tabela.
            t[i] = epn_crc32_update(0, static_cast<uint8_t>(i));
        }
        return t;
    }();

    uint32_t crc = EPN_CRC32_INIT;
    for (uint8_t b : data) crc = (crc >> 8) ^ table[(crc ^ b) & 0xFF];
    return epn_crc32_final(crc);
}

}  // namespace epn
