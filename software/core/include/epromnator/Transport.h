// =============================================================================
//  Transport.h — interface de transporte de bytes (porta serial ou emulador)
// =============================================================================
//
//  O ProtocolClient fala com o gravador através desta interface, o que
//  permite trocar a porta serial real pelo emulador (testes e demonstração)
//  sem alterar nada nas camadas de cima.
// =============================================================================
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace epn {

class Transport {
public:
    virtual ~Transport() = default;

    // Envia todos os bytes (bloqueia até terminar). Lança TransportError.
    virtual void write(std::span<const uint8_t> data) = 0;

    // Lê até `buffer.size()` bytes, esperando no máximo `timeout` pelo
    // PRIMEIRO byte. Retorna a quantidade lida (0 = timeout).
    virtual size_t read(std::span<uint8_t> buffer, std::chrono::milliseconds timeout) = 0;

    // Descarta tudo o que já foi recebido e ainda não foi lido.
    virtual void flushInput() = 0;

    // Nome para exibição (ex.: "/dev/ttyUSB0", "COM3", "emulador").
    virtual std::string name() const = 0;
};

}  // namespace epn
