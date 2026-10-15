// =============================================================================
//  main.cpp — ponto de entrada do firmware EPROMnator
// =============================================================================
#include <Arduino.h>

#include "Bus.h"
#include "DebugShell.h"

void setup() {
    // Primeira coisa a fazer: colocar os sinais da EEPROM em estado seguro.
    Bus::begin();

    Serial.begin(115200);
    DebugShell::begin();
}

void loop() {
    DebugShell::poll();
}
