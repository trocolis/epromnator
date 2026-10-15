// =============================================================================
//  Error.cpp — mensagens de erro
// =============================================================================
#include "epromnator/Error.h"

#include <cstdio>

#include "epromnator_protocol.h"

namespace epn {

std::string statusDescription(uint8_t status) {
    switch (status) {
        case EPN_STATUS_OK:          return "sucesso";
        case EPN_STATUS_ERR_CRC:     return "o firmware recebeu um quadro com CRC inválido";
        case EPN_STATUS_ERR_LENGTH:  return "tamanho de dados inválido";
        case EPN_STATUS_ERR_UNKNOWN: return "comando desconhecido pelo firmware";
        case EPN_STATUS_ERR_RANGE:   return "endereço fora da capacidade do chip";
        case EPN_STATUS_ERR_TIMEOUT:
            return "a gravação não terminou no tempo limite "
                   "(chip protegido por SDP, ausente ou mal encaixado?)";
        case EPN_STATUS_ERR_VERIFY:  return "o byte relido é diferente do gravado";
        case EPN_STATUS_ERR_PARAM:   return "parâmetro inválido";
        default:                     return "status desconhecido";
    }
}

// Monta a mensagem completa: descrição + endereço (se houver).
static std::string makeDeviceMessage(uint8_t status, std::optional<uint32_t> address) {
    std::string msg = "Erro do gravador: " + statusDescription(status);
    if (address) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), " (endereço 0x%04X)", *address);
        msg += buf;
    }
    return msg;
}

DeviceError::DeviceError(uint8_t status, std::optional<uint32_t> address)
    : Error(makeDeviceMessage(status, address)), status_(status), address_(address) {}

}  // namespace epn
