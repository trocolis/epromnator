// =============================================================================
//  Testes do banco de dispositivos
// =============================================================================
#include <gtest/gtest.h>

#include <set>

#include "epromnator/Device.h"
#include "epromnator_protocol.h"

using namespace epn;

TEST(DeviceDatabase, FindIsCaseInsensitive) {
    const Device* d = findDevice("at28c256");
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->name, "AT28C256");
    EXPECT_EQ(d->size, 32768u);
    EXPECT_EQ(d->pageSize, 64);
}

TEST(DeviceDatabase, UnknownDeviceReturnsNull) {
    EXPECT_EQ(findDevice("27C512"), nullptr);
}

// Todas as entradas precisam caber nos limites do hardware/firmware.
TEST(DeviceDatabase, EntriesAreWithinHardwareLimits) {
    std::set<std::string> names;
    for (const Device& d : deviceDatabase()) {
        EXPECT_TRUE(names.insert(d.name).second) << "nome duplicado: " << d.name;
        EXPECT_GT(d.size, 0u);
        EXPECT_LE(d.size, 65536u) << d.name << ": o 74HC595 só gera A0..A15";
        EXPECT_EQ(d.size & (d.size - 1), 0u) << d.name << ": tamanho deve ser potência de 2";
        EXPECT_LE(d.pageSize, EPN_MAX_WRITE_CHUNK) << d.name;
        EXPECT_GT(d.writeTimeoutMs, 0) << d.name;
    }
}

// SRAM não tem SDP: as sequências de desbloqueio sobrescreveriam dados.
TEST(DeviceDatabase, SramEntriesHaveNoSdp) {
    bool found = false;
    for (const Device& d : deviceDatabase()) {
        if (d.type != ChipType::Sram) continue;
        found = true;
        EXPECT_FALSE(d.supportsSdp) << d.name;
    }
    EXPECT_TRUE(found) << "nenhuma SRAM no banco";
}

TEST(DeviceDatabase, SuggestPrefersGenericEntry) {
    const Device* sram = suggestDevice(ChipType::Sram, 32768);
    ASSERT_NE(sram, nullptr);
    EXPECT_EQ(sram->name, "62256 (SRAM)");

    const Device* e32 = suggestDevice(ChipType::Eeprom, 32768);
    ASSERT_NE(e32, nullptr);
    EXPECT_EQ(e32->name, "28C256 (genérico)");

    const Device* e64 = suggestDevice(ChipType::Eeprom, 65536);
    ASSERT_NE(e64, nullptr);
    EXPECT_EQ(e64->name, "28C512 (genérico)");

    // 8 KB: o genérico só grava byte a byte; prefere o primeiro com página.
    const Device* e8 = suggestDevice(ChipType::Eeprom, 8192);
    ASSERT_NE(e8, nullptr);
    EXPECT_GT(e8->pageSize, 1);

    EXPECT_EQ(suggestDevice(ChipType::Sram, 8192), nullptr);
}
