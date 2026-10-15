// =============================================================================
//  Testes das somas de verificação
// =============================================================================
#include <gtest/gtest.h>

#include <string_view>
#include <vector>

#include "epromnator/Checksum.h"
#include "epromnator_protocol.h"

using namespace epn;

namespace {

std::vector<uint8_t> bytesOf(std::string_view s) { return {s.begin(), s.end()}; }

}  // namespace

// Vetor de verificação padrão do CRC-32 IEEE.
TEST(Checksum, Crc32CheckValue) {
    EXPECT_EQ(crc32(bytesOf("123456789")), 0xCBF43926u);
}

// A versão com tabela (PC) deve coincidir com a bit a bit (firmware).
TEST(Checksum, Crc32MatchesFirmwareImplementation) {
    std::vector<uint8_t> data(1000);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i * 37 + 11);
    EXPECT_EQ(crc32(data), epn_crc32(data.data(), data.size()));
}

TEST(Checksum, Crc32OfEmptyIsZero) { EXPECT_EQ(crc32({}), 0u); }

TEST(Checksum, Sum16WrapsAround) {
    // 32 KB de 0xFF: 32768 * 255 = 0x7F8000 → 16 bits inferiores = 0x8000.
    const std::vector<uint8_t> blank(32768, 0xFF);
    EXPECT_EQ(sum16(blank), 0x8000);
    EXPECT_EQ(sum16(bytesOf("\x01\x02\x03")), 6);
}
