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
        EXPECT_LE(d.size, 32768u) << d.name << ": o soquete só tem A0..A14";
        EXPECT_EQ(d.size & (d.size - 1), 0u) << d.name << ": tamanho deve ser potência de 2";
        EXPECT_LE(d.pageSize, EPN_MAX_WRITE_CHUNK) << d.name;
        EXPECT_GT(d.writeTimeoutMs, 0) << d.name;
    }
}
