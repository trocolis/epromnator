// =============================================================================
//  Error.h — exceções usadas pela biblioteca core
// =============================================================================
//
//  Todas as falhas são reportadas por exceções derivadas de epn::Error, com
//  mensagens em português prontas para exibição ao usuário (CLI e GUI).
// =============================================================================
#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace epn {

// Classe base de todos os erros da biblioteca.
class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Falha de comunicação: porta serial, timeout, CRC inválido, etc.
class TransportError : public Error {
public:
    using Error::Error;
};

// O firmware respondeu com um status de erro (ver epn_status_t).
class DeviceError : public Error {
public:
    DeviceError(uint8_t status, std::optional<uint32_t> address);

    // Código de status do protocolo.
    uint8_t status() const { return status_; }

    // Endereço onde a falha ocorreu (para erros de gravação).
    std::optional<uint32_t> address() const { return address_; }

private:
    uint8_t status_;
    std::optional<uint32_t> address_;
};

// Erro de leitura/escrita/interpretação de arquivo (HEX, S-Record, ...).
class FileError : public Error {
public:
    using Error::Error;
};

// Operação cancelada pelo usuário.
class CancelledError : public Error {
public:
    CancelledError() : Error("Operação cancelada pelo usuário") {}
};

// Descrição em português de um código de status do protocolo.
std::string statusDescription(uint8_t status);

}  // namespace epn
