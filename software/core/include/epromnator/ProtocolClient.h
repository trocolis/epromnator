// =============================================================================
//  ProtocolClient.h — lado PC do protocolo binário (ver epromnator_protocol.h)
// =============================================================================
//
//  Responsável por:
//    * montar e enviar quadros de requisição;
//    * reconhecer quadros de resposta (descartando lixo e respostas antigas);
//    * retransmitir em caso de timeout ou CRC inválido;
//    * converter status de erro em exceções (DeviceError).
//
//  Cada método tipado (read, write, crc32...) corresponde a um comando do
//  firmware. A lógica de alto nível (dividir o chip em blocos, progresso,
//  verificação) fica no Programmer.
// =============================================================================
#pragma once

#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "epromnator/Transport.h"

namespace epn {

// Informações devolvidas por HELLO/GET_INFO.
struct FirmwareInfo {
    uint8_t protocolVersion = 0;
    uint8_t versionMajor = 0;
    uint8_t versionMinor = 0;
    uint8_t versionPatch = 0;
    uint16_t maxReadChunk = 0;
    uint8_t maxWriteChunk = 0;
    uint8_t capabilities = 0;  // EPN_CAP_*
    std::string name;

    // "EPROMnator 1.0.0"
    std::string description() const;
};

// Parâmetros enviados ao firmware pelo comando CONFIG.
struct ChipConfig {
    uint32_t size = 32768;
    uint8_t pageSize = 64;
    uint8_t writeTimeoutMs = 15;
    bool protectedWrite = false;
};

// Resultado de BLANK_CHECK.
struct BlankCheckResult {
    bool blank = true;
    uint32_t firstUsedAddress = 0;  // válido somente se !blank
    uint8_t value = 0xFF;           // valor encontrado em firstUsedAddress
};

class ProtocolClient {
public:
    explicit ProtocolClient(Transport& transport);

    // Espera o HELLO enviado pelo firmware após o reset (até `helloTimeout`)
    // e então confirma com GET_INFO. Funciona também com placas que não
    // reiniciam ao abrir a porta (nesse caso o HELLO simplesmente não vem).
    FirmwareInfo handshake(std::chrono::milliseconds helloTimeout = std::chrono::milliseconds(2500));

    // --- Comandos ---------------------------------------------------------
    FirmwareInfo getInfo();
    std::vector<uint8_t> ping(std::span<const uint8_t> payload);
    void configure(const ChipConfig& config);
    std::vector<uint8_t> read(uint32_t address, uint16_t length);
    void write(uint32_t address, std::span<const uint8_t> data, std::chrono::milliseconds timeout);
    BlankCheckResult blankCheck(uint32_t address, uint32_t length);
    uint32_t crc32(uint32_t address, uint32_t length);
    void erase(uint32_t address, uint32_t length, std::chrono::milliseconds timeout);
    void setSdp(bool enabled, std::chrono::milliseconds timeout);

    // Quantas vezes uma requisição é repetida após timeout/CRC inválido.
    void setRetries(int retries) { retries_ = retries; }

    // Estatística: total de retransmissões desde a criação.
    int retransmissions() const { return retransmissions_; }

private:
    struct Response {
        uint8_t command = 0;
        uint8_t sequence = 0;
        uint8_t status = 0;
        std::vector<uint8_t> payload;
    };

    // Envia a requisição e devolve a resposta com status OK. Lança
    // DeviceError para status de erro e TransportError se esgotar as
    // tentativas.
    std::vector<uint8_t> transact(uint8_t command, std::span<const uint8_t> payload,
                                  std::chrono::milliseconds timeout);

    // Lê uma resposta completa. Retorna false em timeout ou CRC inválido.
    bool receiveResponse(Response& response, std::chrono::milliseconds timeout);

    // Lê exatamente `out.size()` bytes até o prazo final.
    bool readExact(std::span<uint8_t> out, std::chrono::steady_clock::time_point deadline);

    static FirmwareInfo decodeInfo(std::span<const uint8_t> payload);

    Transport& transport_;
    uint8_t sequence_ = 0;
    int retries_ = 3;
    int retransmissions_ = 0;
};

}  // namespace epn
