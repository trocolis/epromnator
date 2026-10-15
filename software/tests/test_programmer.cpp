// =============================================================================
//  Testes do Programmer (operações de alto nível) contra o emulador
// =============================================================================
#include <gtest/gtest.h>

#include <atomic>

#include "epromnator/Emulator.h"
#include "epromnator/Error.h"
#include "epromnator/Programmer.h"

using namespace epn;

namespace {

class ProgrammerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // O Programmer assume a posse do emulador; guardamos um ponteiro
        // para inspecionar a "EEPROM" nos testes.
        auto transport = std::make_unique<EmulatorTransport>();
        emu = transport.get();
        programmer = std::make_unique<Programmer>(std::move(transport));
        programmer->connect();
        programmer->selectDevice(*findDevice("AT28C256"));

        // Imagem de teste: padrão dependente do endereço, com uma região
        // de 0xFF no meio (para testar "pular páginas vazias").
        image.assign(32768, 0xFF);
        for (size_t i = 0; i < image.size(); ++i) {
            if (i < 0x4000 || i >= 0x5000) image[i] = static_cast<uint8_t>(i ^ (i >> 8));
        }
    }

    EmulatorTransport* emu = nullptr;
    std::unique_ptr<Programmer> programmer;
    std::vector<uint8_t> image;
};

}  // namespace

TEST_F(ProgrammerTest, ProgramAndVerifyWholeChip) {
    const auto diffs = programmer->program(image);
    EXPECT_TRUE(diffs.empty());
    EXPECT_EQ(emu->memory(), image);
    EXPECT_EQ(emu->writeCycles(), 32768u / 64u);  // um ciclo por página
}

TEST_F(ProgrammerTest, SkipBlankPagesSavesWriteCycles) {
    ProgramOptions options;
    options.skipBlankPages = true;
    EXPECT_TRUE(programmer->program(image, {}, options).empty());
    // 4 KB de 0xFF = 64 páginas a menos.
    EXPECT_EQ(emu->writeCycles(), 32768u / 64u - 64u);
}

TEST_F(ProgrammerTest, ReadReturnsChipContent) {
    emu->memory() = image;
    EXPECT_EQ(programmer->read(), image);
    const auto part = programmer->read({0x100, 10});
    EXPECT_EQ(part, std::vector<uint8_t>(image.begin() + 0x100, image.begin() + 0x10A));
}

TEST_F(ProgrammerTest, VerifyFindsDifferences) {
    emu->memory() = image;
    emu->memory()[0x0042] ^= 0xFF;
    emu->memory()[0x7FFF] ^= 0x01;
    const auto diffs = programmer->verify(image);
    ASSERT_EQ(diffs.size(), 2u);
    EXPECT_EQ(diffs[0].address, 0x0042u);
    EXPECT_EQ(diffs[0].expected, image[0x42]);
    EXPECT_EQ(diffs[0].actual, static_cast<uint8_t>(image[0x42] ^ 0xFF));
    EXPECT_EQ(diffs[1].address, 0x7FFFu);
}

TEST_F(ProgrammerTest, VerifyHonorsMaxDifferences) {
    const std::vector<uint8_t> zeros(32768, 0x00);  // chip vazio x imagem zerada
    EXPECT_EQ(programmer->verify(zeros, {}, 10).size(), 10u);
}

TEST_F(ProgrammerTest, EraseAndBlankCheck) {
    emu->memory() = image;
    EXPECT_FALSE(programmer->blankCheck().blank);
    programmer->erase();
    EXPECT_TRUE(programmer->blankCheck().blank);
}

TEST_F(ProgrammerTest, AutoProgramRunsAllSteps) {
    emu->memory().assign(32768, 0x00);  // chip "sujo"
    const auto diffs = programmer->autoProgram(image);
    EXPECT_TRUE(diffs.empty());
    EXPECT_EQ(emu->memory(), image);
}

TEST_F(ProgrammerTest, RangeOutsideChipThrows) {
    EXPECT_THROW(programmer->read({0x7F00, 0x200}), Error);
}

// Chip de 8 KB em modo byte: cada byte é um ciclo de gravação.
TEST_F(ProgrammerTest, ByteModeDevice) {
    programmer->selectDevice(*findDevice("28C64 (modo byte)"));
    std::vector<uint8_t> small(8192);
    for (size_t i = 0; i < small.size(); ++i) small[i] = static_cast<uint8_t>(i * 7);
    EXPECT_TRUE(programmer->program(small, {0, 256}).empty());
    EXPECT_EQ(emu->writeCycles(), 256u);
}

TEST_F(ProgrammerTest, ProtectedWriteWorksWithSdpEnabled) {
    programmer->setSdp(true);
    EXPECT_THROW(programmer->program(image, {0, 64}), DeviceError);

    programmer->setProtectedWrite(true);
    EXPECT_TRUE(programmer->program(image, {0, 64}).empty());

    programmer->setSdp(false);
    EXPECT_FALSE(emu->sdpEnabled());
}

TEST_F(ProgrammerTest, CancelStopsOperation) {
    std::atomic<bool> cancel{false};
    programmer->setCancelFlag(&cancel);
    // Cancela assim que a primeira notificação de progresso chegar.
    programmer->setProgressCallback([&](const std::string&, uint64_t, uint64_t) { cancel = true; });
    EXPECT_THROW(programmer->read(), CancelledError);
}

TEST_F(ProgrammerTest, ProgressReachesTotal) {
    uint64_t lastDone = 0, lastTotal = 0;
    programmer->setProgressCallback([&](const std::string&, uint64_t done, uint64_t total) {
        EXPECT_GE(done, lastDone);  // nunca regride dentro da mesma etapa
        lastDone = done;
        lastTotal = total;
    });
    programmer->read();
    EXPECT_EQ(lastDone, 32768u);
    EXPECT_EQ(lastTotal, 32768u);
}

TEST_F(ProgrammerTest, LogsAreEmitted) {
    std::vector<std::string> messages;
    programmer->setLogCallback([&](LogLevel, const std::string& m) { messages.push_back(m); });
    programmer->blankCheck();
    ASSERT_FALSE(messages.empty());
    EXPECT_NE(messages.back().find("vazio"), std::string::npos);
}
