// =============================================================================
//  CommandHandler.h — atendimento do protocolo binário
// =============================================================================
//
//  Recebe quadros da serial (FrameParser), executa o comando correspondente
//  no driver da EEPROM e devolve a resposta (ResponseWriter).
//
//  Veja protocol/epromnator_protocol.h para a descrição de cada comando.
// =============================================================================
#pragma once

namespace CommandHandler {

// Envia o quadro HELLO anunciando que o firmware está pronto.
void begin();

// Deve ser chamada continuamente no loop(): processa os bytes recebidos.
void poll();

}  // namespace CommandHandler
