// =============================================================================
//  FrameParser.cpp — implementação da máquina de estados de recepção
// =============================================================================
#include "FrameParser.h"

void FrameParser::reset() {
    state_ = State::WaitSof;
    command_ = 0;
    sequence_ = 0;
    length_ = 0;
    received_ = 0;
    crc_ = EPN_CRC16_INIT;
    receivedCrc_ = 0;
    lastByteMs_ = 0;
}

FrameParser::Event FrameParser::feed(uint8_t byte, uint32_t nowMs) {
    // Se um quadro ficou parado no meio por tempo demais (ex.: o PC foi
    // interrompido ou bytes se perderam), descartamos o que havia e
    // tratamos este byte como possível início de um novo quadro.
    if (state_ != State::WaitSof &&
        (uint32_t)(nowMs - lastByteMs_) > EPN_FRAME_TIMEOUT_MS) {
        reset();
    }
    lastByteMs_ = nowMs;

    switch (state_) {
        case State::WaitSof:
            // Ignora qualquer byte até encontrar o início de quadro.
            if (byte == EPN_SOF_REQUEST) {
                crc_ = EPN_CRC16_INIT;
                state_ = State::Command;
            }
            return Event::None;

        case State::Command:
            command_ = byte;
            crc_ = epn_crc16_update(crc_, byte);
            state_ = State::Sequence;
            return Event::None;

        case State::Sequence:
            sequence_ = byte;
            crc_ = epn_crc16_update(crc_, byte);
            state_ = State::LengthLow;
            return Event::None;

        case State::LengthLow:
            length_ = byte;
            crc_ = epn_crc16_update(crc_, byte);
            state_ = State::LengthHigh;
            return Event::None;

        case State::LengthHigh:
            length_ |= (uint16_t)byte << 8;
            crc_ = epn_crc16_update(crc_, byte);
            if (length_ > EPN_MAX_REQUEST_PAYLOAD) {
                // Não cabe no buffer: avisa e volta a procurar SOF. Os bytes
                // restantes deste quadro serão descartados como lixo.
                state_ = State::WaitSof;
                return Event::LengthError;
            }
            received_ = 0;
            state_ = length_ ? State::Payload : State::CrcLow;
            return Event::None;

        case State::Payload:
            payload_[received_++] = byte;
            crc_ = epn_crc16_update(crc_, byte);
            if (received_ == length_) state_ = State::CrcLow;
            return Event::None;

        case State::CrcLow:
            receivedCrc_ = byte;
            state_ = State::CrcHigh;
            return Event::None;

        case State::CrcHigh:
            receivedCrc_ |= (uint16_t)byte << 8;
            state_ = State::WaitSof;
            return (receivedCrc_ == crc_) ? Event::Frame : Event::CrcError;
    }
    return Event::None;  // inalcançável
}
