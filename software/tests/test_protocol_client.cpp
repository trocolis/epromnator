// =============================================================================
//  Testes do ProtocolClient contra o emulador
// =============================================================================
#include <gtest/gtest.h>

#include <chrono>

#include "epromnator/Emulator.h"
#include "epromnator/Error.h"
#include "epromnator/ProtocolClient.h"
#include "epromnator_protocol.h"

using namespace epn;
using namespace std::chrono_literals;

namespace {

// Ambiente comum: emulador + cliente já com handshake feito.
class ProtocolClientTest : public ::testing::Test {
protected:
    void SetUp() override { info = client.handshake(100ms); }

    EmulatorTransport emu;
    ProtocolClient client{emu};
    FirmwareInfo info;
};

}  // namespace

TEST_F(ProtocolClientTest, HandshakeReadsFirmwareInfo) {
    EXPECT_EQ(info.protocolVersion, EPN_PROTOCOL_VERSION);
    EXPECT_EQ(info.maxReadChunk, EPN_MAX_READ_CHUNK);
    EXPECT_EQ(info.maxWriteChunk, EPN_MAX_WRITE_CHUNK);
    EXPECT_TRUE(info.capabilities & EPN_CAP_SDP);
    EXPECT_TRUE(info.capabilities & EPN_CAP_IDENTIFY);
    EXPECT_NE(info.name.find("EPROMnator"), std::string::npos);
}

TEST_F(ProtocolClientTest, PingEchoesPayload) {
    const std::vector<uint8_t> probe = {1, 2, 3, 0xA5, 0x5A};
    EXPECT_EQ(client.ping(probe), probe);
}

TEST_F(ProtocolClientTest, WriteThenRead) {
    const std::vector<uint8_t> data = {0xDE, 0xAD, 0xBE, 0xEF};
    client.write(0x0100, data, 500ms);
    EXPECT_EQ(client.read(0x0100, 4), data);
    EXPECT_EQ(emu.memory()[0x0103], 0xEF);
}

TEST_F(ProtocolClientTest, WriteAcrossPageBoundaryIsRejected) {
    const std::vector<uint8_t> data(8, 0x00);
    try {
        client.write(0x003C, data, 500ms);  // 0x3C..0x43 cruza a página 0x40
        FAIL() << "esperava DeviceError";
    } catch (const DeviceError& e) {
        EXPECT_EQ(e.status(), EPN_STATUS_ERR_RANGE);
    }
}

TEST_F(ProtocolClientTest, ReadOutOfRangeIsRejected) {
    EXPECT_THROW(client.read(0x7FF0, 32), DeviceError);
}

TEST_F(ProtocolClientTest, BlankCheckReportsFirstUsedByte) {
    EXPECT_TRUE(client.blankCheck(0, 32768).blank);
    emu.memory()[0x1234] = 0x42;
    const BlankCheckResult r = client.blankCheck(0, 32768);
    EXPECT_FALSE(r.blank);
    EXPECT_EQ(r.firstUsedAddress, 0x1234u);
    EXPECT_EQ(r.value, 0x42);
}

TEST_F(ProtocolClientTest, Crc32MatchesStandardValue) {
    const char* s = "123456789";
    std::copy(s, s + 9, emu.memory().begin());
    EXPECT_EQ(client.crc32(0, 9), 0xCBF43926u);
}

// Respostas corrompidas devem ser descartadas e a requisição repetida.
TEST_F(ProtocolClientTest, RetriesAfterCorruptedResponse) {
    emu.corruptNextResponses(2);
    const std::vector<uint8_t> probe = {7};
    EXPECT_EQ(client.ping(probe), probe);
    EXPECT_EQ(client.retransmissions(), 2);
}

// Resposta perdida: o cliente espera o timeout e repete.
TEST_F(ProtocolClientTest, RetriesAfterLostResponse) {
    emu.dropNextResponses(1);
    EXPECT_NO_THROW(client.getInfo());
    EXPECT_EQ(client.retransmissions(), 1);
}

// Sem resposta alguma, o cliente desiste após as tentativas.
TEST_F(ProtocolClientTest, GivesUpAfterRetries) {
    client.setRetries(1);
    emu.dropNextResponses(5);
    EXPECT_THROW(client.getInfo(), TransportError);
}

// Com SDP ativo, escrita comum falha (timeout) e escrita protegida funciona.
TEST_F(ProtocolClientTest, SdpBlocksUnprotectedWrites) {
    client.setSdp(true, 500ms);
    EXPECT_TRUE(emu.sdpEnabled());

    const std::vector<uint8_t> data = {0x11};
    try {
        client.write(0, data, 500ms);
        FAIL() << "esperava DeviceError";
    } catch (const DeviceError& e) {
        EXPECT_EQ(e.status(), EPN_STATUS_ERR_TIMEOUT);
        ASSERT_TRUE(e.address().has_value());
        EXPECT_EQ(*e.address(), 0u);
    }

    ChipConfig config;
    config.protectedWrite = true;
    client.configure(config);
    EXPECT_NO_THROW(client.write(0, data, 500ms));
    EXPECT_EQ(emu.memory()[0], 0x11);
}

TEST_F(ProtocolClientTest, ConfigRejectsInvalidValues) {
    ChipConfig config;
    config.size = 1u << 20;  // 1 MB: impossível com 16 linhas de endereço
    EXPECT_THROW(client.configure(config), DeviceError);
}

TEST_F(ProtocolClientTest, IdentifyReportsEeprom) {
    const ChipIdentity id = client.identify();
    EXPECT_EQ(id.kind, EPN_CHIP_EEPROM);
    EXPECT_EQ(id.size, 32768u);
    EXPECT_GT(id.writeTimeUs, 0u);

    emu.setSdpEnabled(true);
    EXPECT_EQ(client.identify().kind, EPN_CHIP_EEPROM_SDP);
}

TEST_F(ProtocolClientTest, IdentifyReportsSram) {
    emu.setChipType(ChipType::Sram);
    const ChipIdentity id = client.identify();
    EXPECT_EQ(id.kind, EPN_CHIP_SRAM);
    EXPECT_EQ(id.size, 32768u);
    EXPECT_EQ(id.writeTimeUs, 0u);
}
