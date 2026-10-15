// =============================================================================
//  main.cpp — ponto de entrada do firmware EPROMnator
// =============================================================================
//
//  Dois modos de operação, escolhidos na compilação:
//
//   * padrão (env:nano)            → protocolo binário usado pelo software
//                                    do PC (CLI e GUI);
//   * EPN_DEBUG_SHELL (env:nano_debug) → shell de texto para testar a placa
//                                    pelo monitor serial.
// =============================================================================
#include <Arduino.h>

#include "Bus.h"
#include "epromnator_protocol.h"

#ifdef EPN_DEBUG_SHELL
#include "DebugShell.h"
#else
#include "CommandHandler.h"
#endif

void setup() {
    // Primeira coisa a fazer: colocar os sinais da EEPROM em estado seguro.
    Bus::begin();

    Serial.begin(EPN_DEFAULT_BAUD);

#ifdef EPN_DEBUG_SHELL
    DebugShell::begin();
#else
    CommandHandler::begin();
#endif
}

void loop() {
#ifdef EPN_DEBUG_SHELL
    DebugShell::poll();
#else
    CommandHandler::poll();
#endif
}
