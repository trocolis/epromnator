// =============================================================================
//  Testes unitários (nativos) do framing e dos CRCs do protocolo
// =============================================================================
//
//  Executar com:  pio test -e native
// =============================================================================
#include <unity.h>

#include <string.h>
#include <vector>

#include "FrameParser.h"
#include "ResponseWriter.h"
#include "epromnator_protocol.h"

// ---------------------------------------------------------------------------
//  Utilitários dos testes
// ---------------------------------------------------------------------------

// Monta um quadro de requisição completo, como o software do PC faria.
static std::vector<uint8_t> buildRequest(uint8_t cmd, uint8_t seq,
                                         const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> frame;
    frame.push_back(EPN_SOF_REQUEST);
    frame.push_back(cmd);
    frame.push_back(seq);
    frame.push_back((uint8_t)(payload.size() & 0xFF));
    frame.push_back((uint8_t)(payload.size() >> 8));
    frame.insert(frame.end(), payload.begin(), payload.end());
    // CRC sobre tudo exceto o SOF.
    const uint16_t crc = epn_crc16(frame.data() + 1, frame.size() - 1);
    frame.push_back((uint8_t)(crc & 0xFF));
    frame.push_back((uint8_t)(crc >> 8));
    return frame;
}

// Alimenta o parser com todos os bytes e devolve o último evento diferente
// de None (ou None, se nenhum ocorreu).
static FrameParser::Event feedAll(FrameParser& parser, const std::vector<uint8_t>& bytes,
                                  uint32_t nowMs = 0) {
    FrameParser::Event last = FrameParser::Event::None;
    for (uint8_t b : bytes) {
        const FrameParser::Event e = parser.feed(b, nowMs);
        if (e != FrameParser::Event::None) last = e;
    }
    return last;
}

// Compara eventos do parser (enum class não converte sozinho para int).
#define ASSERT_EVENT(expected, actual) \
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(actual))

// Saída capturada pelo ResponseWriter nos testes.
static std::vector<uint8_t> captured;
static void captureSink(uint8_t b) { captured.push_back(b); }

void setUp() { captured.clear(); }
void tearDown() {}

// ---------------------------------------------------------------------------
//  CRCs: vetores de verificação padrão ("123456789")
// ---------------------------------------------------------------------------

void test_crc16_check_value() {
    const char* s = "123456789";
    TEST_ASSERT_EQUAL_HEX16(0x29B1, epn_crc16((const uint8_t*)s, strlen(s)));
}

void test_crc32_check_value() {
    const char* s = "123456789";
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926, epn_crc32((const uint8_t*)s, strlen(s)));
}

void test_little_endian_helpers() {
    uint8_t buf[4];
    epn_put_u32(buf, 0x12345678);
    TEST_ASSERT_EQUAL_HEX8(0x78, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x12, buf[3]);
    TEST_ASSERT_EQUAL_HEX32(0x12345678, epn_get_u32(buf));
    epn_put_u16(buf, 0xBEEF);
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, epn_get_u16(buf));
}

// ---------------------------------------------------------------------------
//  FrameParser
// ---------------------------------------------------------------------------

void test_parser_accepts_valid_frame() {
    FrameParser parser;
    const auto frame = buildRequest(EPN_CMD_READ, 7, {1, 2, 3, 4, 5, 6});
    ASSERT_EVENT(FrameParser::Event::Frame, feedAll(parser, frame));
    TEST_ASSERT_EQUAL_UINT8(EPN_CMD_READ, parser.command());
    TEST_ASSERT_EQUAL_UINT8(7, parser.sequence());
    TEST_ASSERT_EQUAL_UINT16(6, parser.length());
    TEST_ASSERT_EQUAL_UINT8(6, parser.payload()[5]);
}

void test_parser_accepts_empty_payload() {
    FrameParser parser;
    const auto frame = buildRequest(EPN_CMD_GET_INFO, 1, {});
    ASSERT_EVENT(FrameParser::Event::Frame, feedAll(parser, frame));
    TEST_ASSERT_EQUAL_UINT16(0, parser.length());
}

void test_parser_skips_garbage_before_sof() {
    FrameParser parser;
    std::vector<uint8_t> bytes = {0x00, 0x13, 0xFF, 0x42};
    const auto frame = buildRequest(EPN_CMD_PING, 3, {0xAB});
    bytes.insert(bytes.end(), frame.begin(), frame.end());
    ASSERT_EVENT(FrameParser::Event::Frame, feedAll(parser, bytes));
    TEST_ASSERT_EQUAL_UINT8(0xAB, parser.payload()[0]);
}

void test_parser_detects_crc_error() {
    FrameParser parser;
    auto frame = buildRequest(EPN_CMD_PING, 9, {1, 2, 3});
    frame[5] ^= 0x01;  // corrompe um byte do payload
    ASSERT_EVENT(FrameParser::Event::CrcError, feedAll(parser, frame));
    TEST_ASSERT_EQUAL_UINT8(9, parser.sequence());  // seq preservado p/ resposta
}

void test_parser_rejects_oversized_length() {
    FrameParser parser;
    const std::vector<uint8_t> header = {EPN_SOF_REQUEST, EPN_CMD_WRITE, 1,
                                         0xFF, 0x00};  // LEN = 255
    ASSERT_EVENT(FrameParser::Event::LengthError, feedAll(parser, header));
}

void test_parser_resyncs_after_timeout() {
    FrameParser parser;
    // Quadro truncado no instante 0...
    auto partial = buildRequest(EPN_CMD_PING, 1, {1, 2, 3});
    partial.resize(4);
    ASSERT_EVENT(FrameParser::Event::None, feedAll(parser, partial, 0));
    // ...e um quadro completo bem depois do timeout deve ser aceito.
    const auto frame = buildRequest(EPN_CMD_GET_INFO, 2, {});
    ASSERT_EVENT(FrameParser::Event::Frame,
                      feedAll(parser, frame, EPN_FRAME_TIMEOUT_MS + 50));
    TEST_ASSERT_EQUAL_UINT8(2, parser.sequence());
}

void test_parser_back_to_back_frames() {
    FrameParser parser;
    const auto a = buildRequest(EPN_CMD_PING, 1, {0x11});
    const auto b = buildRequest(EPN_CMD_PING, 2, {0x22});
    ASSERT_EVENT(FrameParser::Event::Frame, feedAll(parser, a));
    ASSERT_EVENT(FrameParser::Event::Frame, feedAll(parser, b));
    TEST_ASSERT_EQUAL_UINT8(0x22, parser.payload()[0]);
}

// ---------------------------------------------------------------------------
//  ResponseWriter
// ---------------------------------------------------------------------------

void test_writer_encodes_response() {
    ResponseWriter writer(captureSink);
    const uint8_t data[] = {0xDE, 0xAD};
    writer.send(EPN_CMD_READ, 5, EPN_STATUS_OK, data, sizeof(data));

    // SOF + 5 de cabeçalho + 2 de payload + 2 de CRC
    TEST_ASSERT_EQUAL_size_t(EPN_RESPONSE_HEADER_SIZE + 2 + EPN_CRC_SIZE, captured.size());
    TEST_ASSERT_EQUAL_HEX8(EPN_SOF_RESPONSE, captured[0]);
    TEST_ASSERT_EQUAL_HEX8(EPN_CMD_READ | EPN_RESPONSE_FLAG, captured[1]);
    TEST_ASSERT_EQUAL_UINT8(5, captured[2]);
    TEST_ASSERT_EQUAL_UINT8(EPN_STATUS_OK, captured[3]);
    TEST_ASSERT_EQUAL_UINT16(2, epn_get_u16(&captured[4]));
    TEST_ASSERT_EQUAL_HEX8(0xDE, captured[6]);

    // O CRC transmitido deve conferir com o calculado sobre CMD..payload.
    const uint16_t crc = epn_crc16(captured.data() + 1, captured.size() - 3);
    TEST_ASSERT_EQUAL_HEX16(crc, epn_get_u16(&captured[captured.size() - 2]));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_crc16_check_value);
    RUN_TEST(test_crc32_check_value);
    RUN_TEST(test_little_endian_helpers);
    RUN_TEST(test_parser_accepts_valid_frame);
    RUN_TEST(test_parser_accepts_empty_payload);
    RUN_TEST(test_parser_skips_garbage_before_sof);
    RUN_TEST(test_parser_detects_crc_error);
    RUN_TEST(test_parser_rejects_oversized_length);
    RUN_TEST(test_parser_resyncs_after_timeout);
    RUN_TEST(test_parser_back_to_back_frames);
    RUN_TEST(test_writer_encodes_response);
    return UNITY_END();
}
