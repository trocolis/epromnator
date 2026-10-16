// =============================================================================
//  CommandHandler.cpp — implementação do atendimento do protocolo
// =============================================================================
#include "CommandHandler.h"

// Compilado apenas no modo protocolo binário (padrão).
#ifndef EPN_DEBUG_SHELL

#include <Arduino.h>

#include "Bus.h"
#include "Eeprom28c.h"
#include "FrameParser.h"
#include "ResponseWriter.h"
#include "Version.h"
#include "epromnator_protocol.h"

namespace CommandHandler {

namespace {

// Envia um byte pela serial (função compatível com ResponseWriter::Sink).
void serialSink(uint8_t byte) { Serial.write(byte); }

FrameParser parser;
ResponseWriter writer(serialSink);

// Nome anunciado em GET_INFO/HELLO.
const char DEVICE_NAME[] = "EPROMnator";

// ---------------------------------------------------------------------------
//  Utilitários
// ---------------------------------------------------------------------------

// Converte o resultado do driver para um código de status do protocolo.
uint8_t toStatus(Eeprom::Result result) {
    switch (result) {
        case Eeprom::Result::Ok:          return EPN_STATUS_OK;
        case Eeprom::Result::Timeout:     return EPN_STATUS_ERR_TIMEOUT;
        case Eeprom::Result::VerifyError: return EPN_STATUS_ERR_VERIFY;
        case Eeprom::Result::RangeError:  return EPN_STATUS_ERR_RANGE;
    }
    return EPN_STATUS_ERR_PARAM;
}

// Confere se a faixa [address, address + count) está dentro do chip.
bool inRange(uint32_t address, uint32_t count) {
    const uint32_t size = Eeprom::config().size;
    return address < size && count <= size - address;
}

// Responde com o resultado de uma gravação; em caso de erro, o payload
// leva o endereço onde a falha aconteceu.
void replyWriteResult(uint8_t cmd, uint8_t seq, Eeprom::Result result,
                      uint16_t failAddress) {
    if (result == Eeprom::Result::Ok) {
        writer.send(cmd, seq, EPN_STATUS_OK);
    } else {
        uint8_t payload[4];
        epn_put_u32(payload, failAddress);
        writer.send(cmd, seq, toStatus(result), payload, sizeof(payload));
    }
}

// ---------------------------------------------------------------------------
//  Comandos
// ---------------------------------------------------------------------------

// Payload de GET_INFO/HELLO: versões, limites, capacidades e nome.
void sendInfo(uint8_t cmd, uint8_t seq) {
    uint8_t info[EPN_INFO_MIN_SIZE];
    info[EPN_INFO_PROTOCOL] = EPN_PROTOCOL_VERSION;
    info[EPN_INFO_FW_MAJOR] = FW_VERSION_MAJOR;
    info[EPN_INFO_FW_MINOR] = FW_VERSION_MINOR;
    info[EPN_INFO_FW_PATCH] = FW_VERSION_PATCH;
    epn_put_u16(&info[EPN_INFO_MAX_READ], EPN_MAX_READ_CHUNK);
    info[EPN_INFO_MAX_WRITE] = EPN_MAX_WRITE_CHUNK;
    info[EPN_INFO_FLAGS] = EPN_CAP_SDP | EPN_CAP_PAGE_WRITE | EPN_CAP_IDENTIFY;

    const uint16_t nameLength = sizeof(DEVICE_NAME) - 1;  // sem o '\0'
    writer.begin(cmd, seq, EPN_STATUS_OK, sizeof(info) + nameLength);
    writer.put(info, sizeof(info));
    writer.put(reinterpret_cast<const uint8_t*>(DEVICE_NAME), nameLength);
    writer.end();
}

// CONFIG: [size u32][page u8][timeout u8][flags u8]
void handleConfig(uint8_t cmd, uint8_t seq, const uint8_t* p, uint16_t len) {
    if (len != EPN_CONFIG_PAYLOAD_LEN) {
        writer.send(cmd, seq, EPN_STATUS_ERR_LENGTH);
        return;
    }
    Eeprom::Config config;
    config.size = epn_get_u32(&p[EPN_CONFIG_SIZE]);
    config.pageSize = p[EPN_CONFIG_PAGE_SIZE];
    config.writeTimeoutMs = p[EPN_CONFIG_TIMEOUT_MS];
    config.protectedWrite = (p[EPN_CONFIG_FLAGS] & EPN_CFG_PROTECTED_WRITE) != 0;

    // Rejeita valores que o hardware não suporta (16 linhas de endereço e
    // buffers de no máximo uma página de 64 bytes).
    if (config.size == 0 || config.size > 65536UL ||
        config.pageSize > Eeprom::MAX_PAGE_SIZE) {
        writer.send(cmd, seq, EPN_STATUS_ERR_PARAM);
        return;
    }
    Eeprom::configure(config);
    writer.send(cmd, seq, EPN_STATUS_OK);
}

// READ: [addr u32][len u16] → len bytes. A resposta é transmitida em
// pedaços pequenos, lidos da EEPROM sob demanda (economia de RAM).
void handleRead(uint8_t cmd, uint8_t seq, const uint8_t* p, uint16_t len) {
    if (len != 6) {
        writer.send(cmd, seq, EPN_STATUS_ERR_LENGTH);
        return;
    }
    const uint32_t address = epn_get_u32(&p[0]);
    const uint16_t count = epn_get_u16(&p[4]);
    if (count == 0 || count > EPN_MAX_READ_CHUNK || !inRange(address, count)) {
        writer.send(cmd, seq, EPN_STATUS_ERR_RANGE);
        return;
    }

    writer.begin(cmd, seq, EPN_STATUS_OK, count);
    uint8_t chunk[32];
    for (uint16_t done = 0; done < count; done += sizeof(chunk)) {
        uint16_t n = count - done;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        Eeprom::readBlock((uint16_t)(address + done), chunk, n);
        writer.put(chunk, n);
    }
    writer.end();
}

// WRITE: [addr u32][dados...]
void handleWrite(uint8_t cmd, uint8_t seq, const uint8_t* p, uint16_t len) {
    if (len < 5 || len - 4 > EPN_MAX_WRITE_CHUNK) {
        writer.send(cmd, seq, EPN_STATUS_ERR_LENGTH);
        return;
    }
    const uint32_t address = epn_get_u32(&p[0]);
    const uint8_t count = (uint8_t)(len - 4);
    if (!inRange(address, count)) {
        writer.send(cmd, seq, EPN_STATUS_ERR_RANGE);
        return;
    }
    uint16_t failAddress = 0;
    const Eeprom::Result r = Eeprom::write((uint16_t)address, &p[4], count, &failAddress);
    replyWriteResult(cmd, seq, r, failAddress);
}

// Lê os parâmetros [addr u32][len u32] comuns a BLANK_CHECK, CRC32 e ERASE.
// Responde com erro e retorna false se forem inválidos.
bool parseRange(uint8_t cmd, uint8_t seq, const uint8_t* p, uint16_t len,
                uint32_t& address, uint32_t& count) {
    if (len != 8) {
        writer.send(cmd, seq, EPN_STATUS_ERR_LENGTH);
        return false;
    }
    address = epn_get_u32(&p[0]);
    count = epn_get_u32(&p[4]);
    if (count == 0 || !inRange(address, count)) {
        writer.send(cmd, seq, EPN_STATUS_ERR_RANGE);
        return false;
    }
    return true;
}

// BLANK_CHECK → [vazio u8][primeiro_usado u32][valor u8]
void handleBlankCheck(uint8_t cmd, uint8_t seq, const uint8_t* p, uint16_t len) {
    uint32_t address, count;
    if (!parseRange(cmd, seq, p, len, address, count)) return;

    uint16_t first = 0;
    uint8_t value = 0xFF;
    const bool blank = Eeprom::isBlank((uint16_t)address, count, &first, &value);

    uint8_t payload[6];
    payload[0] = blank ? 1 : 0;
    epn_put_u32(&payload[1], blank ? 0 : first);
    payload[5] = value;
    writer.send(cmd, seq, EPN_STATUS_OK, payload, sizeof(payload));
}

// CRC32 → [crc u32]
void handleCrc32(uint8_t cmd, uint8_t seq, const uint8_t* p, uint16_t len) {
    uint32_t address, count;
    if (!parseRange(cmd, seq, p, len, address, count)) return;

    uint8_t payload[4];
    epn_put_u32(payload, Eeprom::crc32((uint16_t)address, count));
    writer.send(cmd, seq, EPN_STATUS_OK, payload, sizeof(payload));
}

// ERASE → vazio, ou [addr_falha u32] em caso de erro
void handleErase(uint8_t cmd, uint8_t seq, const uint8_t* p, uint16_t len) {
    uint32_t address, count;
    if (!parseRange(cmd, seq, p, len, address, count)) return;

    uint16_t failAddress = 0;
    const Eeprom::Result r = Eeprom::erase((uint16_t)address, count, &failAddress);
    replyWriteResult(cmd, seq, r, failAddress);
}

// IDENTIFY → [tipo u8][tamanho u32][ciclo de gravação µs u32]
void handleIdentify(uint8_t cmd, uint8_t seq, uint16_t len) {
    if (len != 0) {
        writer.send(cmd, seq, EPN_STATUS_ERR_LENGTH);
        return;
    }
    const Eeprom::Identity id = Eeprom::identify();
    uint8_t payload[EPN_IDENTIFY_PAYLOAD_LEN];
    payload[EPN_IDENTIFY_KIND] = id.kind;
    epn_put_u32(&payload[EPN_IDENTIFY_SIZE], id.size);
    epn_put_u32(&payload[EPN_IDENTIFY_WRITE_US], id.writeTimeUs);
    writer.send(cmd, seq, EPN_STATUS_OK, payload, sizeof(payload));
}

// Executa um quadro válido.
void dispatch(uint8_t cmd, uint8_t seq, const uint8_t* p, uint16_t len) {
    switch (cmd) {
        case EPN_CMD_PING:
            writer.send(cmd, seq, EPN_STATUS_OK, p, len);  // eco
            break;
        case EPN_CMD_GET_INFO:
            sendInfo(cmd, seq);
            break;
        case EPN_CMD_CONFIG:
            handleConfig(cmd, seq, p, len);
            break;
        case EPN_CMD_READ:
            handleRead(cmd, seq, p, len);
            break;
        case EPN_CMD_WRITE:
            handleWrite(cmd, seq, p, len);
            break;
        case EPN_CMD_BLANK_CHECK:
            handleBlankCheck(cmd, seq, p, len);
            break;
        case EPN_CMD_CRC32:
            handleCrc32(cmd, seq, p, len);
            break;
        case EPN_CMD_ERASE:
            handleErase(cmd, seq, p, len);
            break;
        case EPN_CMD_SDP_ENABLE:
            Eeprom::sdpEnable();
            writer.send(cmd, seq, EPN_STATUS_OK);
            break;
        case EPN_CMD_SDP_DISABLE:
            Eeprom::sdpDisable();
            writer.send(cmd, seq, EPN_STATUS_OK);
            break;
        case EPN_CMD_IDENTIFY:
            handleIdentify(cmd, seq, len);
            break;
        default:
            writer.send(cmd, seq, EPN_STATUS_ERR_UNKNOWN);
            break;
    }
}

}  // namespace

void begin() {
    // Anuncia que o firmware está pronto. O PC aguarda este quadro depois
    // de abrir a porta (que reinicia o Nano via DTR).
    sendInfo(EPN_CMD_HELLO, 0);
}

void poll() {
    while (Serial.available()) {
        const uint8_t byte = (uint8_t)Serial.read();
        switch (parser.feed(byte, millis())) {
            case FrameParser::Event::None:
                break;

            case FrameParser::Event::Frame:
                // LED aceso enquanto o comando é executado.
                Bus::busyLed(true);
                dispatch(parser.command(), parser.sequence(), parser.payload(),
                         parser.length());
                Bus::busyLed(false);
                break;

            case FrameParser::Event::CrcError:
                writer.send(parser.command(), parser.sequence(), EPN_STATUS_ERR_CRC);
                break;

            case FrameParser::Event::LengthError:
                writer.send(parser.command(), parser.sequence(), EPN_STATUS_ERR_LENGTH);
                break;
        }
    }
}

}  // namespace CommandHandler

#endif  // !EPN_DEBUG_SHELL
