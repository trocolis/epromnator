// =============================================================================
//  Emulator.h — emulador do gravador (firmware + EEPROM) em memória
// =============================================================================
//
//  Implementa o mesmo protocolo do firmware, sobre uma EEPROM simulada.
//  Serve para:
//    * testes unitários do ProtocolClient e do Programmer;
//    * desenvolver e demonstrar a CLI e a GUI sem a placa (porta "emu").
//
//  O reconhecimento de quadros usa o MESMO código do firmware
//  (firmware/lib/framing/FrameParser.cpp), garantindo compatibilidade.
//
//  Comportamentos simulados:
//    * restrição de página na escrita (bytes devem estar na mesma página);
//    * SDP: com a proteção ativa, escritas sem a sequência de desbloqueio
//      falham com ERR_TIMEOUT (como no chip real, que as ignora);
//    * injeção de falhas: respostas corrompidas ou perdidas.
// =============================================================================
#pragma once

#include <deque>
#include <memory>
#include <vector>

#include "epromnator/Transport.h"

class FrameParser;  // firmware/lib/framing/FrameParser.h

namespace epn {

class EmulatorTransport final : public Transport {
public:
    // Cria o emulador com uma EEPROM de `chipSize` bytes cheia de 0xFF.
    // Como o Nano real, envia HELLO assim que é "aberto".
    explicit EmulatorTransport(uint32_t chipSize = 32768);
    ~EmulatorTransport() override;

    void write(std::span<const uint8_t> data) override;
    size_t read(std::span<uint8_t> buffer, std::chrono::milliseconds timeout) override;
    void flushInput() override { output_.clear(); }
    std::string name() const override { return "emulador"; }

    // --- Acesso para testes ------------------------------------------------

    // Conteúdo da EEPROM simulada (pode ser alterado diretamente).
    std::vector<uint8_t>& memory() { return memory_; }

    // Estado da proteção SDP simulada.
    bool sdpEnabled() const { return sdpEnabled_; }
    void setSdpEnabled(bool enabled) { sdpEnabled_ = enabled; }

    // Corrompe um byte das próximas `count` respostas (testa retransmissão).
    void corruptNextResponses(int count) { corruptResponses_ = count; }

    // Descarta as próximas `count` respostas (simula perda/timeout).
    void dropNextResponses(int count) { dropResponses_ = count; }

    // Quantidade de ciclos de gravação executados (para testar "pular 0xFF").
    size_t writeCycles() const { return writeCycles_; }

private:
    // Executa um quadro reconhecido e enfileira a resposta.
    void dispatch(uint8_t command, uint8_t sequence, const uint8_t* payload, uint16_t length);

    // Enfileira um quadro de resposta (aplicando a injeção de falhas).
    void respond(uint8_t command, uint8_t sequence, uint8_t status,
                 const std::vector<uint8_t>& payload = {});

    // Grava bytes simulando o chip; devolve o status do protocolo.
    uint8_t writeBytes(uint32_t address, const uint8_t* data, size_t count, uint32_t& failAddress);

    std::vector<uint8_t> helloPayload() const;

    std::unique_ptr<FrameParser> parser_;
    std::deque<uint8_t> output_;  // bytes "enviados pelo firmware" ainda não lidos
    std::vector<uint8_t> memory_;

    // Configuração atual (comando CONFIG).
    uint32_t size_;
    uint8_t pageSize_ = 64;
    bool protectedWrite_ = false;

    bool sdpEnabled_ = false;
    int corruptResponses_ = 0;
    int dropResponses_ = 0;
    size_t writeCycles_ = 0;
};

}  // namespace epn
