// =============================================================================
//  DebugShell.h — shell de depuração em modo texto
// =============================================================================
//
//  Interface de linha de comando simples, usada pelo monitor serial
//  (`pio device monitor`) durante a montagem e os testes da placa. Permite
//  conferir com um multímetro cada linha de endereço e de dados, além de
//  ler e gravar bytes isolados na EEPROM.
//
//  Não é usada pelo software do PC, que conversa com o firmware através do
//  protocolo binário (ver protocol/epromnator_protocol.h).
// =============================================================================
#pragma once

namespace DebugShell {

// Imprime o banner e a ajuda.
void begin();

// Deve ser chamada continuamente no loop(): consome caracteres da serial e
// executa um comando a cada linha completa.
void poll();

}  // namespace DebugShell
