// =============================================================================
//  ProtocolClient.cpp
// =============================================================================
#include "epromnator/ProtocolClient.h"

#include <optional>

#include "epromnator/Error.h"
#include "epromnator_protocol.h"

namespace epn {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

// Tempo padrão para comandos rápidos (GET_INFO, CONFIG, READ...).
constexpr auto kDefaultTimeout = 1000ms;

// Tempo restante até o prazo (nunca negativo).
std::chrono::milliseconds remaining(Clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    return left.count() > 0 ? left : 0ms;
}

}  // namespace

std::string FirmwareInfo::description() const {
    return name + " " + std::to_string(versionMajor) + "." + std::to_string(versionMinor) + "." +
           std::to_string(versionPatch);
}

ProtocolClient::ProtocolClient(Transport& transport) : transport_(transport) {}

// =============================================================================
//  Recepção
// =============================================================================

bool ProtocolClient::readExact(std::span<uint8_t> out, Clock::time_point deadline) {
    size_t done = 0;
    while (done < out.size()) {
        const auto left = remaining(deadline);
        if (left == 0ms) return false;
        done += transport_.read(out.subspan(done), left);
    }
    return true;
}

bool ProtocolClient::receiveResponse(Response& response, std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;

    // 1) Procura o SOF, descartando qualquer lixo (ex.: mensagens do
    //    bootloader ou restos de uma resposta anterior).
    uint8_t byte = 0;
    do {
        if (!readExact({&byte, 1}, deadline)) return false;
    } while (byte != EPN_SOF_RESPONSE);

    // 2) Cabeçalho: CMD, SEQ, STATUS, LEN_L, LEN_H.
    uint8_t header[EPN_RESPONSE_HEADER_SIZE - 1];
    if (!readExact(header, deadline)) return false;
    const uint16_t length = epn_get_u16(&header[3]);

    // 3) Payload e CRC.
    std::vector<uint8_t> payload(length);
    uint8_t crcBytes[EPN_CRC_SIZE];
    if (!readExact(payload, deadline) || !readExact(crcBytes, deadline)) return false;

    // 4) Confere o CRC (sobre cabeçalho sem SOF + payload).
    uint16_t crc = EPN_CRC16_INIT;
    for (uint8_t b : header) crc = epn_crc16_update(crc, b);
    for (uint8_t b : payload) crc = epn_crc16_update(crc, b);
    if (crc != epn_get_u16(crcBytes)) return false;

    response.command = header[0];
    response.sequence = header[1];
    response.status = header[2];
    response.payload = std::move(payload);
    return true;
}

// =============================================================================
//  Transação requisição/resposta com retransmissão
// =============================================================================

std::vector<uint8_t> ProtocolClient::transact(uint8_t command, std::span<const uint8_t> payload,
                                              std::chrono::milliseconds timeout) {
    if (payload.size() > EPN_MAX_REQUEST_PAYLOAD) {
        throw Error("Requisição maior que o suportado pelo firmware");
    }

    for (int attempt = 0; attempt <= retries_; ++attempt) {
        if (attempt > 0) {
            ++retransmissions_;
            // Antes de repetir, descarta o que tiver chegado de uma resposta
            // corrompida/atrasada para não confundi-la com a nova.
            transport_.flushInput();
        }

        // Cada tentativa usa um número de sequência novo: assim uma resposta
        // atrasada da tentativa anterior é reconhecida e descartada.
        const uint8_t seq = ++sequence_;

        // Monta o quadro: SOF CMD SEQ LEN_L LEN_H payload CRC_L CRC_H.
        std::vector<uint8_t> frame;
        frame.reserve(EPN_REQUEST_HEADER_SIZE + payload.size() + EPN_CRC_SIZE);
        frame.push_back(EPN_SOF_REQUEST);
        frame.push_back(command);
        frame.push_back(seq);
        frame.push_back(static_cast<uint8_t>(payload.size() & 0xFF));
        frame.push_back(static_cast<uint8_t>(payload.size() >> 8));
        frame.insert(frame.end(), payload.begin(), payload.end());
        const uint16_t crc = epn_crc16(frame.data() + 1, frame.size() - 1);
        frame.push_back(static_cast<uint8_t>(crc & 0xFF));
        frame.push_back(static_cast<uint8_t>(crc >> 8));
        transport_.write(frame);

        // Aguarda a resposta correspondente a ESTA sequência.
        const auto deadline = Clock::now() + timeout;
        Response response;
        bool received = false;
        while (remaining(deadline) > 0ms) {
            if (!receiveResponse(response, remaining(deadline))) break;  // timeout ou CRC
            if (response.sequence == seq &&
                response.command == static_cast<uint8_t>(command | EPN_RESPONSE_FLAG)) {
                received = true;
                break;
            }
            // Resposta de outra sequência (ou HELLO tardio): ignora e continua.
        }
        if (!received) continue;  // tenta de novo

        // O firmware recebeu a requisição corrompida: vale repetir.
        if (response.status == EPN_STATUS_ERR_CRC) continue;

        if (response.status != EPN_STATUS_OK) {
            std::optional<uint32_t> address;
            if (response.payload.size() == 4) address = epn_get_u32(response.payload.data());
            throw DeviceError(response.status, address);
        }
        return std::move(response.payload);
    }

    throw TransportError("O gravador não respondeu em " + transport_.name() +
                         " (verifique o cabo, a porta e se o firmware está gravado)");
}

// =============================================================================
//  Comandos
// =============================================================================

FirmwareInfo ProtocolClient::decodeInfo(std::span<const uint8_t> p) {
    if (p.size() < EPN_INFO_MIN_SIZE) throw TransportError("Resposta GET_INFO curta demais");
    FirmwareInfo info;
    info.protocolVersion = p[EPN_INFO_PROTOCOL];
    info.versionMajor = p[EPN_INFO_FW_MAJOR];
    info.versionMinor = p[EPN_INFO_FW_MINOR];
    info.versionPatch = p[EPN_INFO_FW_PATCH];
    info.maxReadChunk = epn_get_u16(&p[EPN_INFO_MAX_READ]);
    info.maxWriteChunk = p[EPN_INFO_MAX_WRITE];
    info.capabilities = p[EPN_INFO_FLAGS];
    info.name.assign(p.begin() + EPN_INFO_NAME, p.end());
    return info;
}

FirmwareInfo ProtocolClient::handshake(std::chrono::milliseconds helloTimeout) {
    // Abrir a porta reinicia o Nano: o bootloader roda por ~1-2 s e depois o
    // firmware envia HELLO. Esperamos por ele (ignorando qualquer outro lixo).
    const auto deadline = Clock::now() + helloTimeout;
    Response response;
    while (remaining(deadline) > 0ms) {
        if (!receiveResponse(response, remaining(deadline))) break;
        if (response.command == (EPN_CMD_HELLO | EPN_RESPONSE_FLAG)) break;
    }

    // Com ou sem HELLO, GET_INFO confirma que o enlace funciona.
    FirmwareInfo info = getInfo();
    if (info.protocolVersion != EPN_PROTOCOL_VERSION) {
        throw Error("Versão de protocolo incompatível: firmware v" +
                    std::to_string(info.protocolVersion) + ", software v" +
                    std::to_string(EPN_PROTOCOL_VERSION) + ". Atualize o firmware.");
    }
    return info;
}

FirmwareInfo ProtocolClient::getInfo() {
    return decodeInfo(transact(EPN_CMD_GET_INFO, {}, kDefaultTimeout));
}

std::vector<uint8_t> ProtocolClient::ping(std::span<const uint8_t> payload) {
    return transact(EPN_CMD_PING, payload, kDefaultTimeout);
}

void ProtocolClient::configure(const ChipConfig& config) {
    uint8_t p[EPN_CONFIG_PAYLOAD_LEN];
    epn_put_u32(&p[EPN_CONFIG_SIZE], config.size);
    p[EPN_CONFIG_PAGE_SIZE] = config.pageSize;
    p[EPN_CONFIG_TIMEOUT_MS] = config.writeTimeoutMs;
    p[EPN_CONFIG_FLAGS] = config.protectedWrite ? EPN_CFG_PROTECTED_WRITE : 0;
    transact(EPN_CMD_CONFIG, p, kDefaultTimeout);
}

std::vector<uint8_t> ProtocolClient::read(uint32_t address, uint16_t length) {
    uint8_t p[6];
    epn_put_u32(&p[0], address);
    epn_put_u16(&p[4], length);
    auto data = transact(EPN_CMD_READ, p, kDefaultTimeout);
    if (data.size() != length) throw TransportError("READ devolveu quantidade errada de bytes");
    return data;
}

void ProtocolClient::write(uint32_t address, std::span<const uint8_t> data,
                           std::chrono::milliseconds timeout) {
    std::vector<uint8_t> p(4 + data.size());
    epn_put_u32(p.data(), address);
    std::copy(data.begin(), data.end(), p.begin() + 4);
    transact(EPN_CMD_WRITE, p, timeout);
}

BlankCheckResult ProtocolClient::blankCheck(uint32_t address, uint32_t length) {
    uint8_t p[8];
    epn_put_u32(&p[0], address);
    epn_put_u32(&p[4], length);
    // Leitura de ~20 µs por byte no firmware, com margem.
    const auto timeout = kDefaultTimeout + std::chrono::milliseconds(length / 20);
    const auto r = transact(EPN_CMD_BLANK_CHECK, p, timeout);
    if (r.size() != 6) throw TransportError("Resposta BLANK_CHECK com tamanho inválido");
    return {r[0] != 0, epn_get_u32(&r[1]), r[5]};
}

uint32_t ProtocolClient::crc32(uint32_t address, uint32_t length) {
    uint8_t p[8];
    epn_put_u32(&p[0], address);
    epn_put_u32(&p[4], length);
    const auto timeout = kDefaultTimeout + std::chrono::milliseconds(length / 20);
    const auto r = transact(EPN_CMD_CRC32, p, timeout);
    if (r.size() != 4) throw TransportError("Resposta CRC32 com tamanho inválido");
    return epn_get_u32(r.data());
}

void ProtocolClient::erase(uint32_t address, uint32_t length, std::chrono::milliseconds timeout) {
    uint8_t p[8];
    epn_put_u32(&p[0], address);
    epn_put_u32(&p[4], length);
    transact(EPN_CMD_ERASE, p, timeout);
}

void ProtocolClient::setSdp(bool enabled, std::chrono::milliseconds timeout) {
    transact(enabled ? EPN_CMD_SDP_ENABLE : EPN_CMD_SDP_DISABLE, {}, timeout);
}

}  // namespace epn
