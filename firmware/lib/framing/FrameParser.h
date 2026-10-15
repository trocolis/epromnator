// =============================================================================
//  FrameParser.h — reconhecimento dos quadros de requisição do protocolo
// =============================================================================
//
//  Máquina de estados que recebe os bytes da serial um a um e reconhece um
//  quadro completo de requisição (ver protocol/epromnator_protocol.h).
//
//  Não depende do Arduino: o tempo atual é passado como parâmetro. Assim o
//  mesmo código roda no Nano e nos testes nativos no PC (pio test -e native).
// =============================================================================
#pragma once

#include <stdint.h>

#include "epromnator_protocol.h"

class FrameParser {
public:
    // Resultado do processamento de um byte.
    enum class Event : uint8_t {
        None,         // quadro ainda incompleto (ou byte de lixo descartado)
        Frame,        // quadro completo e com CRC válido
        CrcError,     // quadro completo, mas o CRC não confere
        LengthError,  // o campo LEN excede EPN_MAX_REQUEST_PAYLOAD
    };

    FrameParser() { reset(); }

    // Descarta qualquer quadro parcial e volta a procurar o SOF.
    void reset();

    // Processa um byte recebido. `nowMs` é o tempo atual em milissegundos,
    // usado para descartar quadros que ficaram pela metade.
    Event feed(uint8_t byte, uint32_t nowMs);

    // Dados do último quadro reconhecido (válidos após Frame/CrcError/
    // LengthError, até a próxima chamada de feed()).
    uint8_t command() const { return command_; }
    uint8_t sequence() const { return sequence_; }
    uint16_t length() const { return length_; }
    const uint8_t* payload() const { return payload_; }

private:
    // Estados da máquina, na ordem em que os campos chegam.
    enum class State : uint8_t {
        WaitSof,
        Command,
        Sequence,
        LengthLow,
        LengthHigh,
        Payload,
        CrcLow,
        CrcHigh,
    };

    State state_;
    uint8_t command_;
    uint8_t sequence_;
    uint16_t length_;
    uint16_t received_;      // bytes de payload já recebidos
    uint16_t crc_;           // CRC calculado incrementalmente
    uint16_t receivedCrc_;   // CRC informado no quadro
    uint32_t lastByteMs_;    // instante do último byte (para o timeout)
    uint8_t payload_[EPN_MAX_REQUEST_PAYLOAD];
};
