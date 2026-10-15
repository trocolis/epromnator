// =============================================================================
//  Emulator.cpp
// =============================================================================
#include "epromnator/Emulator.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include "FrameParser.h"
#include "epromnator/Checksum.h"
#include "epromnator_protocol.h"

namespace epn {

namespace {

// Tempo atual em milissegundos (para o timeout de quadro do FrameParser).
uint32_t nowMs() {
    using namespace std::chrono;
    return static_cast<uint32_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

}  // namespace

EmulatorTransport::EmulatorTransport(uint32_t chipSize)
    : parser_(std::make_unique<FrameParser>()), memory_(chipSize, 0xFF), size_(chipSize) {
    // Mesmo comportamento do firmware ao ligar.
    respond(EPN_CMD_HELLO, 0, EPN_STATUS_OK, helloPayload());
}

EmulatorTransport::~EmulatorTransport() = default;

// =============================================================================
//  Transport
// =============================================================================

void EmulatorTransport::write(std::span<const uint8_t> data) {
    // Cada byte "chega ao firmware" e passa pelo mesmo FrameParser do Nano.
    for (uint8_t b : data) {
        switch (parser_->feed(b, nowMs())) {
            case FrameParser::Event::None:
                break;
            case FrameParser::Event::Frame:
                dispatch(parser_->command(), parser_->sequence(), parser_->payload(),
                         parser_->length());
                break;
            case FrameParser::Event::CrcError:
                respond(parser_->command(), parser_->sequence(), EPN_STATUS_ERR_CRC);
                break;
            case FrameParser::Event::LengthError:
                respond(parser_->command(), parser_->sequence(), EPN_STATUS_ERR_LENGTH);
                break;
        }
    }
}

size_t EmulatorTransport::read(std::span<uint8_t> buffer, std::chrono::milliseconds timeout) {
    // O emulador responde de forma síncrona: se não há bytes agora, não
    // haverá depois. Dormimos o timeout inteiro (como uma porta real faria),
    // evitando que quem chama fique em laço ocupado até o prazo.
    if (output_.empty()) {
        std::this_thread::sleep_for(timeout);
        return 0;
    }
    const size_t n = std::min(buffer.size(), output_.size());
    std::copy_n(output_.begin(), n, buffer.begin());
    output_.erase(output_.begin(), output_.begin() + static_cast<std::ptrdiff_t>(n));
    return n;
}

// =============================================================================
//  Respostas
// =============================================================================

std::vector<uint8_t> EmulatorTransport::helloPayload() const {
    std::vector<uint8_t> p(EPN_INFO_MIN_SIZE);
    p[EPN_INFO_PROTOCOL] = EPN_PROTOCOL_VERSION;
    p[EPN_INFO_FW_MAJOR] = 1;
    p[EPN_INFO_FW_MINOR] = 0;
    p[EPN_INFO_FW_PATCH] = 0;
    epn_put_u16(&p[EPN_INFO_MAX_READ], EPN_MAX_READ_CHUNK);
    p[EPN_INFO_MAX_WRITE] = EPN_MAX_WRITE_CHUNK;
    p[EPN_INFO_FLAGS] = EPN_CAP_SDP | EPN_CAP_PAGE_WRITE;
    const std::string name = "EPROMnator (emulador)";
    p.insert(p.end(), name.begin(), name.end());
    return p;
}

void EmulatorTransport::respond(uint8_t command, uint8_t sequence, uint8_t status,
                                const std::vector<uint8_t>& payload) {
    // Simula perda total da resposta.
    if (dropResponses_ > 0) {
        --dropResponses_;
        return;
    }

    std::vector<uint8_t> frame;
    frame.push_back(EPN_SOF_RESPONSE);
    frame.push_back(static_cast<uint8_t>(command | EPN_RESPONSE_FLAG));
    frame.push_back(sequence);
    frame.push_back(status);
    frame.push_back(static_cast<uint8_t>(payload.size() & 0xFF));
    frame.push_back(static_cast<uint8_t>(payload.size() >> 8));
    frame.insert(frame.end(), payload.begin(), payload.end());
    const uint16_t crc = epn_crc16(frame.data() + 1, frame.size() - 1);
    frame.push_back(static_cast<uint8_t>(crc & 0xFF));
    frame.push_back(static_cast<uint8_t>(crc >> 8));

    // Simula ruído na linha: inverte um bit do CRC (o host deve rejeitar).
    if (corruptResponses_ > 0) {
        --corruptResponses_;
        frame.back() ^= 0x01;
    }
    output_.insert(output_.end(), frame.begin(), frame.end());
}

// =============================================================================
//  EEPROM simulada
// =============================================================================

uint8_t EmulatorTransport::writeBytes(uint32_t address, const uint8_t* data, size_t count,
                                      uint32_t& failAddress) {
    // Chip protegido e escrita sem desbloqueio: o chip real ignora a escrita
    // e o data polling nunca termina → timeout.
    if (sdpEnabled_ && !protectedWrite_) {
        failAddress = address + static_cast<uint32_t>(count) - 1;
        return EPN_STATUS_ERR_TIMEOUT;
    }
    std::copy_n(data, count, memory_.begin() + address);
    // Um ciclo por página (ou por byte no modo byte).
    writeCycles_ += (pageSize_ > 1) ? 1 : count;
    return EPN_STATUS_OK;
}

void EmulatorTransport::dispatch(uint8_t cmd, uint8_t seq, const uint8_t* p, uint16_t len) {
    // Faixa [address, address + count) dentro do chip?
    auto inRange = [this](uint32_t address, uint32_t count) {
        return address < size_ && count <= size_ - address;
    };

    switch (cmd) {
        case EPN_CMD_PING:
            respond(cmd, seq, EPN_STATUS_OK, std::vector<uint8_t>(p, p + len));
            return;

        case EPN_CMD_GET_INFO:
            respond(cmd, seq, EPN_STATUS_OK, helloPayload());
            return;

        case EPN_CMD_CONFIG: {
            if (len != EPN_CONFIG_PAYLOAD_LEN) return respond(cmd, seq, EPN_STATUS_ERR_LENGTH);
            const uint32_t size = epn_get_u32(&p[EPN_CONFIG_SIZE]);
            const uint8_t page = p[EPN_CONFIG_PAGE_SIZE];
            if (size == 0 || size > 65536 || page > EPN_MAX_WRITE_CHUNK) {
                return respond(cmd, seq, EPN_STATUS_ERR_PARAM);
            }
            size_ = size;
            pageSize_ = page;
            protectedWrite_ = (p[EPN_CONFIG_FLAGS] & EPN_CFG_PROTECTED_WRITE) != 0;
            // A EEPROM "física" pode ser maior que o configurado; só cresce.
            if (memory_.size() < size_) memory_.resize(size_, 0xFF);
            return respond(cmd, seq, EPN_STATUS_OK);
        }

        case EPN_CMD_READ: {
            if (len != 6) return respond(cmd, seq, EPN_STATUS_ERR_LENGTH);
            const uint32_t address = epn_get_u32(&p[0]);
            const uint16_t count = epn_get_u16(&p[4]);
            if (count == 0 || count > EPN_MAX_READ_CHUNK || !inRange(address, count)) {
                return respond(cmd, seq, EPN_STATUS_ERR_RANGE);
            }
            return respond(cmd, seq, EPN_STATUS_OK,
                           std::vector<uint8_t>(memory_.begin() + address,
                                                memory_.begin() + address + count));
        }

        case EPN_CMD_WRITE: {
            if (len < 5 || len - 4 > EPN_MAX_WRITE_CHUNK) {
                return respond(cmd, seq, EPN_STATUS_ERR_LENGTH);
            }
            const uint32_t address = epn_get_u32(&p[0]);
            const uint32_t count = len - 4u;
            if (!inRange(address, count)) return respond(cmd, seq, EPN_STATUS_ERR_RANGE);
            // Em modo página, todos os bytes precisam estar na mesma página.
            if (pageSize_ > 1 && address / pageSize_ != (address + count - 1) / pageSize_) {
                return respond(cmd, seq, EPN_STATUS_ERR_RANGE);
            }
            uint32_t fail = 0;
            const uint8_t status = writeBytes(address, &p[4], count, fail);
            if (status == EPN_STATUS_OK) return respond(cmd, seq, status);
            std::vector<uint8_t> payload(4);
            epn_put_u32(payload.data(), fail);
            return respond(cmd, seq, status, payload);
        }

        case EPN_CMD_BLANK_CHECK:
        case EPN_CMD_CRC32:
        case EPN_CMD_ERASE: {
            if (len != 8) return respond(cmd, seq, EPN_STATUS_ERR_LENGTH);
            const uint32_t address = epn_get_u32(&p[0]);
            const uint32_t count = epn_get_u32(&p[4]);
            if (count == 0 || !inRange(address, count)) {
                return respond(cmd, seq, EPN_STATUS_ERR_RANGE);
            }
            const auto first = memory_.begin() + address;
            const auto last = first + count;

            if (cmd == EPN_CMD_BLANK_CHECK) {
                const auto it = std::find_if(first, last, [](uint8_t b) { return b != 0xFF; });
                std::vector<uint8_t> payload(6, 0);
                payload[0] = (it == last) ? 1 : 0;
                if (it != last) epn_put_u32(&payload[1], static_cast<uint32_t>(it - memory_.begin()));
                payload[5] = (it == last) ? 0xFF : *it;
                return respond(cmd, seq, EPN_STATUS_OK, payload);
            }

            if (cmd == EPN_CMD_CRC32) {
                std::vector<uint8_t> payload(4);
                epn_put_u32(payload.data(), epn::crc32(std::span<const uint8_t>(&*first, count)));
                return respond(cmd, seq, EPN_STATUS_OK, payload);
            }

            // ERASE: preenche com 0xFF (com as mesmas regras de SDP da escrita).
            const std::vector<uint8_t> blank(count, 0xFF);
            uint32_t fail = 0;
            const uint8_t status = writeBytes(address, blank.data(), count, fail);
            if (status == EPN_STATUS_OK) return respond(cmd, seq, status);
            std::vector<uint8_t> payload(4);
            epn_put_u32(payload.data(), fail);
            return respond(cmd, seq, status, payload);
        }

        case EPN_CMD_SDP_ENABLE:
            sdpEnabled_ = true;
            return respond(cmd, seq, EPN_STATUS_OK);

        case EPN_CMD_SDP_DISABLE:
            sdpEnabled_ = false;
            return respond(cmd, seq, EPN_STATUS_OK);

        default:
            return respond(cmd, seq, EPN_STATUS_ERR_UNKNOWN);
    }
}

}  // namespace epn
