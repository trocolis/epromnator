// =============================================================================
//  Checksum.h — somas de verificação exibidas ao usuário e usadas na verificação
// =============================================================================
#pragma once

#include <cstdint>
#include <span>

namespace epn {

// Soma simples de 16 bits de todos os bytes (o "checksum" mostrado pelos
// gravadores comerciais, como TL866/XGPro).
uint16_t sum16(std::span<const uint8_t> data);

// CRC-32 IEEE (o mesmo do zip e do comando CRC32 do firmware).
uint32_t crc32(std::span<const uint8_t> data);

}  // namespace epn
