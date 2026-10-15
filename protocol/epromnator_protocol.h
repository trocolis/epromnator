/* ============================================================================
 *  epromnator_protocol.h — protocolo serial entre o PC e o firmware
 * ============================================================================
 *
 *  Este arquivo é COMPARTILHADO entre o firmware (avr-gcc, C++11) e o
 *  software do PC (C++20). Por isso usa apenas C puro e funções `static
 *  inline`, sem dependências além de <stdint.h>/<stddef.h>.
 *
 *  Visão geral
 *  -----------
 *  Comunicação "pare e espere" (stop-and-wait): o PC envia UMA requisição e
 *  aguarda UMA resposta antes de enviar a próxima. Isso dispensa controle de
 *  fluxo e mantém o buffer de recepção do Nano pequeno.
 *
 *  Formato da requisição (PC → firmware):
 *
 *     +------+-----+-----+-------+-------+-----------+-------+-------+
 *     | 0xA5 | CMD | SEQ | LEN_L | LEN_H | payload…  | CRC_L | CRC_H |
 *     +------+-----+-----+-------+-------+-----------+-------+-------+
 *
 *  Formato da resposta (firmware → PC):
 *
 *     +------+----------+-----+--------+-------+-------+-----------+-------+-------+
 *     | 0x5A | CMD|0x80 | SEQ | STATUS | LEN_L | LEN_H | payload…  | CRC_L | CRC_H |
 *     +------+----------+-----+--------+-------+-------+-----------+-------+-------+
 *
 *   * SEQ: número de sequência escolhido pelo PC e devolvido na resposta;
 *     permite descartar respostas atrasadas após uma retransmissão.
 *   * CRC: CRC-16/CCITT-FALSE (polinômio 0x1021, valor inicial 0xFFFF)
 *     calculado sobre todos os bytes ENTRE o SOF e o CRC (exclusive).
 *   * Todos os inteiros multibyte são little-endian.
 *
 *  Ao ligar (ou após o reset causado pela abertura da porta serial, via
 *  DTR), o firmware envia espontaneamente uma resposta HELLO com o mesmo
 *  payload de GET_INFO. O PC usa isso para saber que o Nano está pronto.
 * ========================================================================== */
#ifndef EPROMNATOR_PROTOCOL_H
#define EPROMNATOR_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 *  Constantes gerais
 * ------------------------------------------------------------------------- */

/* Versão do protocolo. Incrementar a cada mudança incompatível. */
#define EPN_PROTOCOL_VERSION 1

/* Velocidade padrão da serial. */
#define EPN_DEFAULT_BAUD 115200UL

/* Bytes de início de quadro (Start Of Frame). */
#define EPN_SOF_REQUEST  0xA5
#define EPN_SOF_RESPONSE 0x5A

/* Bit somado ao código do comando na resposta. */
#define EPN_RESPONSE_FLAG 0x80

/* Tamanhos de cabeçalho (incluindo o SOF) e do CRC. */
#define EPN_REQUEST_HEADER_SIZE  5
#define EPN_RESPONSE_HEADER_SIZE 6
#define EPN_CRC_SIZE             2

/* Maior payload de requisição aceito pelo firmware: endereço (4 bytes)
 * + uma página de 64 bytes, com folga. Limitado pela RAM do ATmega328. */
#define EPN_MAX_REQUEST_PAYLOAD 80

/* Maior bloco que pode ser pedido em um READ e gravado em um WRITE. */
#define EPN_MAX_READ_CHUNK  256
#define EPN_MAX_WRITE_CHUNK 64

/* Se um quadro ficar incompleto por mais que isto, o firmware o descarta
 * e volta a procurar um novo SOF (ressincronização). */
#define EPN_FRAME_TIMEOUT_MS 200

/* ---------------------------------------------------------------------------
 *  Comandos
 * ------------------------------------------------------------------------- */
typedef enum {
    /* Resposta espontânea enviada no boot (nunca é requisitada). */
    EPN_CMD_HELLO = 0x00,

    /* Eco do payload; usado para testar o enlace. */
    EPN_CMD_PING = 0x01,

    /* Versões e capacidades do firmware (payload: ver EPN_INFO_*). */
    EPN_CMD_GET_INFO = 0x02,

    /* Parâmetros do chip (payload: ver EPN_CONFIG_*). */
    EPN_CMD_CONFIG = 0x03,

    /* READ: [addr u32][len u16] → resposta: len bytes lidos. */
    EPN_CMD_READ = 0x10,

    /* WRITE: [addr u32][dados 1..64] → em erro: [addr_falha u32].
     * No modo página todos os bytes devem estar na mesma página. */
    EPN_CMD_WRITE = 0x11,

    /* BLANK_CHECK: [addr u32][len u32]
     * → [vazio u8][primeiro_usado u32][valor u8]. */
    EPN_CMD_BLANK_CHECK = 0x12,

    /* CRC32: [addr u32][len u32] → [crc u32]. */
    EPN_CMD_CRC32 = 0x13,

    /* ERASE (preenche com 0xFF): [addr u32][len u32] → em erro: [addr u32]. */
    EPN_CMD_ERASE = 0x14,

    /* Proteção de dados por software (sem payload). */
    EPN_CMD_SDP_ENABLE  = 0x20,
    EPN_CMD_SDP_DISABLE = 0x21
} epn_command_t;

/* ---------------------------------------------------------------------------
 *  Códigos de status da resposta
 * ------------------------------------------------------------------------- */
typedef enum {
    EPN_STATUS_OK            = 0x00, /* sucesso */
    EPN_STATUS_ERR_CRC       = 0x01, /* CRC da requisição inválido */
    EPN_STATUS_ERR_LENGTH    = 0x02, /* tamanho de payload inválido */
    EPN_STATUS_ERR_UNKNOWN   = 0x03, /* comando desconhecido */
    EPN_STATUS_ERR_RANGE     = 0x04, /* endereço/tamanho fora do chip */
    EPN_STATUS_ERR_TIMEOUT   = 0x05, /* gravação não terminou (SDP ativo?) */
    EPN_STATUS_ERR_VERIFY    = 0x06, /* byte relido diferente do gravado */
    EPN_STATUS_ERR_PARAM     = 0x07  /* parâmetro inválido */
} epn_status_t;

/* ---------------------------------------------------------------------------
 *  Payload de HELLO / GET_INFO (offsets em bytes)
 * ------------------------------------------------------------------------- */
#define EPN_INFO_PROTOCOL     0 /* u8:  EPN_PROTOCOL_VERSION */
#define EPN_INFO_FW_MAJOR     1 /* u8 */
#define EPN_INFO_FW_MINOR     2 /* u8 */
#define EPN_INFO_FW_PATCH     3 /* u8 */
#define EPN_INFO_MAX_READ     4 /* u16: maior READ aceito */
#define EPN_INFO_MAX_WRITE    6 /* u8:  maior WRITE aceito */
#define EPN_INFO_FLAGS        7 /* u8:  EPN_CAP_* */
#define EPN_INFO_NAME         8 /* texto ASCII até o fim do payload */
#define EPN_INFO_MIN_SIZE     8

/* Capacidades do hardware/firmware. */
#define EPN_CAP_SDP        0x01 /* suporta comandos SDP */
#define EPN_CAP_PAGE_WRITE 0x02 /* suporta escrita em modo página */

/* ---------------------------------------------------------------------------
 *  Payload de CONFIG (offsets em bytes)
 * ------------------------------------------------------------------------- */
#define EPN_CONFIG_SIZE        0 /* u32: capacidade do chip em bytes */
#define EPN_CONFIG_PAGE_SIZE   4 /* u8:  bytes por página (0/1 = modo byte) */
#define EPN_CONFIG_TIMEOUT_MS  5 /* u8:  limite do data polling em ms */
#define EPN_CONFIG_FLAGS       6 /* u8:  EPN_CFG_* */
#define EPN_CONFIG_PAYLOAD_LEN 7

/* Precede cada ciclo de gravação com a sequência SDP (AA/55/A0). */
#define EPN_CFG_PROTECTED_WRITE 0x01

/* ---------------------------------------------------------------------------
 *  Funções auxiliares: inteiros little-endian
 * ------------------------------------------------------------------------- */
static inline void epn_put_u16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static inline void epn_put_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)(v >> 24);
}

static inline uint16_t epn_get_u16(const uint8_t* p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t epn_get_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* ---------------------------------------------------------------------------
 *  CRC-16/CCITT-FALSE — protege os quadros
 *  (verificação: "123456789" → 0x29B1)
 * ------------------------------------------------------------------------- */
#define EPN_CRC16_INIT 0xFFFFu

static inline uint16_t epn_crc16_update(uint16_t crc, uint8_t byte) {
    crc ^= (uint16_t)byte << 8;
    for (uint8_t i = 0; i < 8; ++i) {
        crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
    return crc;
}

static inline uint16_t epn_crc16(const uint8_t* data, size_t len) {
    uint16_t crc = EPN_CRC16_INIT;
    for (size_t i = 0; i < len; ++i) crc = epn_crc16_update(crc, data[i]);
    return crc;
}

/* ---------------------------------------------------------------------------
 *  CRC-32 (IEEE 802.3, o mesmo do zip) — verificação do conteúdo do chip
 *  (verificação: "123456789" → 0xCBF43926)
 *
 *  Uso: crc = EPN_CRC32_INIT; crc = epn_crc32_update(crc, b)…;
 *       resultado = epn_crc32_final(crc);
 * ------------------------------------------------------------------------- */
#define EPN_CRC32_INIT 0xFFFFFFFFul

static inline uint32_t epn_crc32_update(uint32_t crc, uint8_t byte) {
    crc ^= byte;
    for (uint8_t i = 0; i < 8; ++i) {
        crc = (crc >> 1) ^ (0xEDB88320ul & (0ul - (crc & 1ul)));
    }
    return crc;
}

static inline uint32_t epn_crc32_final(uint32_t crc) { return ~crc; }

static inline uint32_t epn_crc32(const uint8_t* data, size_t len) {
    uint32_t crc = EPN_CRC32_INIT;
    for (size_t i = 0; i < len; ++i) crc = epn_crc32_update(crc, data[i]);
    return epn_crc32_final(crc);
}

#endif /* EPROMNATOR_PROTOCOL_H */
