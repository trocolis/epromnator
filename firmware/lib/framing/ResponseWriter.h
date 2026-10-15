// =============================================================================
//  ResponseWriter.h — montagem dos quadros de resposta
// =============================================================================
//
//  Envia uma resposta byte a byte, calculando o CRC "em voo". Isso permite
//  transmitir respostas grandes (READ de 256 bytes) sem precisar guardá-las
//  inteiras na RAM do ATmega328 (2 KB no total).
//
//  Uso típico:
//      writer.begin(cmd, seq, status, len);
//      for (...) writer.put(byte);      // exatamente `len` bytes
//      writer.end();
//
//  A saída é feita por um ponteiro de função (`sink`), o que mantém este
//  módulo independente do Arduino e testável no PC.
// =============================================================================
#pragma once

#include <stdint.h>

#include "epromnator_protocol.h"

class ResponseWriter {
public:
    // Função que efetivamente envia um byte (ex.: Serial.write).
    using Sink = void (*)(uint8_t);

    explicit ResponseWriter(Sink sink) : sink_(sink), crc_(EPN_CRC16_INIT) {}

    // Envia o cabeçalho da resposta. `command` é o código da REQUISIÇÃO;
    // o bit EPN_RESPONSE_FLAG é somado aqui.
    void begin(uint8_t command, uint8_t sequence, uint8_t status, uint16_t length) {
        sink_(EPN_SOF_RESPONSE);  // o SOF não entra no CRC
        crc_ = EPN_CRC16_INIT;
        put((uint8_t)(command | EPN_RESPONSE_FLAG));
        put(sequence);
        put(status);
        put((uint8_t)(length & 0xFF));
        put((uint8_t)(length >> 8));
    }

    // Envia um byte de payload, atualizando o CRC.
    void put(uint8_t byte) {
        crc_ = epn_crc16_update(crc_, byte);
        sink_(byte);
    }

    // Envia vários bytes de payload.
    void put(const uint8_t* data, uint16_t count) {
        for (uint16_t i = 0; i < count; ++i) put(data[i]);
    }

    // Envia o CRC e encerra o quadro.
    void end() {
        const uint16_t crc = crc_;
        sink_((uint8_t)(crc & 0xFF));
        sink_((uint8_t)(crc >> 8));
    }

    // Atalho para respostas sem payload ou com payload já em memória.
    void send(uint8_t command, uint8_t sequence, uint8_t status,
              const uint8_t* data = nullptr, uint16_t length = 0) {
        begin(command, sequence, status, length);
        if (data) put(data, length);
        end();
    }

private:
    Sink sink_;
    uint16_t crc_;
};
