// =============================================================================
//  DeviceDatabase.cpp — banco de dispositivos embutido
// =============================================================================
//
//  Para adicionar um chip: inclua uma entrada na tabela abaixo. Os tempos
//  de gravação usam o máximo do datasheet com margem (o firmware só espera
//  o necessário graças ao data polling; o valor é apenas o limite).
// =============================================================================
#include "epromnator/Device.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace epn {

namespace {

// Observação comum aos chips de 8 KB nesta placa.
constexpr const char* kNote28C64 =
    "Chip de 8 KB: o pino 1 é RDY/~BUSY e o 26 não é ligado; "
    "o firmware mantém A13/A14 em nível baixo.";

const std::array<Device, 8> kDevices = {{
    {"AT28C256", "Atmel/Microchip", 32768, 64, 15, true,
     "EEPROM 32K x 8, páginas de 64 bytes, tWC 10 ms."},
    {"AT28C256F", "Atmel/Microchip", 32768, 64, 8, true,
     "Versão rápida: tWC 3 ms."},
    {"CAT28C256", "Catalyst/onsemi", 32768, 64, 15, true,
     "EEPROM 32K x 8, páginas de 64 bytes."},
    {"X28C256", "Xicor/Intersil", 32768, 64, 15, true,
     "EEPROM 32K x 8, páginas de 64 bytes, tWC 5 ms."},
    {"28C256 (genérico)", "Genérico", 32768, 64, 20, true,
     "Parâmetros conservadores para clones de origem desconhecida."},
    {"AT28C64B", "Atmel/Microchip", 8192, 64, 15, true, kNote28C64},
    {"X28C64", "Xicor/Intersil", 8192, 64, 15, true, kNote28C64},
    {"28C64 (modo byte)", "Genérico", 8192, 0, 15, false,
     "Grava byte a byte: compatível com chips sem modo página (mais lento)."},
}};

// Compara duas strings ignorando maiúsculas/minúsculas (apenas ASCII).
bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

}  // namespace

std::span<const Device> deviceDatabase() { return kDevices; }

const Device* findDevice(std::string_view name) {
    for (const Device& d : kDevices) {
        if (equalsIgnoreCase(d.name, name)) return &d;
    }
    return nullptr;
}

}  // namespace epn
