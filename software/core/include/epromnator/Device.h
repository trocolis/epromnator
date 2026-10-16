// =============================================================================
//  Device.h — descrição dos chips suportados e banco de dispositivos
// =============================================================================
//
//  O hardware do EPROMnator aceita EEPROMs paralelas de 5 V com pinagem
//  JEDEC de 28 pinos (família 28Cxxx), SRAMs com a mesma pinagem (62256) e,
//  com um adaptador que leva A15 ao chip, EEPROMs de 32 pinos e 64 KB
//  (28C512). Cada entrada do banco informa os parâmetros que o firmware
//  precisa para gravar o chip corretamente.
// =============================================================================
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace epn {

enum class ChipType {
    Eeprom,  // grava com ciclo interno (data polling), pode ter SDP
    Sram,    // grava na hora; conteúdo mantido por bateria
};

struct Device {
    std::string name;          // ex.: "AT28C256"
    std::string manufacturer;  // ex.: "Atmel/Microchip"
    uint32_t size;             // capacidade em bytes
    uint8_t pageSize;          // bytes por página (0 = só modo byte)
    uint8_t writeTimeoutMs;    // limite do ciclo de gravação (data polling)
    bool supportsSdp;          // possui proteção de dados por software
    std::string notes;         // observações exibidas ao usuário
    ChipType type = ChipType::Eeprom;
};

// "EEPROM" ou "SRAM".
std::string chipTypeName(ChipType type);

// Lista de todos os dispositivos conhecidos, em ordem de exibição.
std::span<const Device> deviceDatabase();

// Procura um dispositivo pelo nome, sem diferenciar maiúsculas/minúsculas.
// Retorna nullptr se não existir.
const Device* findDevice(std::string_view name);

// Entrada sugerida para um chip identificado pelo tipo e tamanho: a genérica,
// se houver, senão a primeira compatível. Retorna nullptr se nenhuma servir.
const Device* suggestDevice(ChipType type, uint32_t size);

}  // namespace epn
