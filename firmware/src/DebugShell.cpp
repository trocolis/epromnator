// =============================================================================
//  DebugShell.cpp — implementação do shell de depuração em modo texto
// =============================================================================
//
//  Comandos (números sempre em hexadecimal):
//
//    h | ?                 mostra a ajuda
//    a <end>               coloca <end> no barramento de endereços e para
//                          (útil para medir A0..A14 com multímetro)
//    d <byte>              dirige <byte> no barramento de dados (~OE alto)
//    z                     solta o barramento de dados (alta impedância)
//    r <end> [qtd]         lê [qtd] bytes (padrão 16, máx. 256) a partir de <end>
//    w <end> <b0> [b1...]  grava bytes a partir de <end>, um a um
// =============================================================================
#include "DebugShell.h"

#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

#include "Bus.h"

namespace DebugShell {

namespace {

// Tamanho máximo de uma linha de comando (inclui o terminador '\0').
constexpr uint8_t LINE_MAX = 96;

// Buffer da linha que está sendo digitada e quantidade de caracteres nele.
char lineBuffer[LINE_MAX];
uint8_t lineLength = 0;

// ---------------------------------------------------------------------------
//  Utilitários de impressão
// ---------------------------------------------------------------------------

// Imprime um byte como dois dígitos hexadecimais (com zero à esquerda).
void printHex8(uint8_t value) {
    if (value < 0x10) Serial.print('0');
    Serial.print(value, HEX);
}

// Imprime um endereço como quatro dígitos hexadecimais.
void printHex16(uint16_t value) {
    printHex8(value >> 8);
    printHex8(value & 0xFF);
}

void printHelp() {
    Serial.println(F("Comandos (valores em hexadecimal):"));
    Serial.println(F("  h | ?                 ajuda"));
    Serial.println(F("  a <end>               fixa o endereco no barramento"));
    Serial.println(F("  d <byte>              dirige o barramento de dados"));
    Serial.println(F("  z                     barramento de dados em alta impedancia"));
    Serial.println(F("  r <end> [qtd]         le bytes (padrao 16, max 256)"));
    Serial.println(F("  w <end> <b0> [b1...]  grava bytes um a um"));
}

// ---------------------------------------------------------------------------
//  Acesso à EEPROM em modo "byte a byte" (sem driver ainda)
// ---------------------------------------------------------------------------

// Lê um byte: endereço → ~CE baixo → ~OE baixo → aguarda tACC → lê.
uint8_t readByte(uint16_t address) {
    Bus::dataAsInput();
    Bus::setAddress(address);
    Bus::ceLow();
    Bus::oeLow();
    Bus::accessDelay();
    const uint8_t value = Bus::readData();
    Bus::oeHigh();
    Bus::ceHigh();
    return value;
}

// Grava um byte com um único pulso em ~WE e espera o tempo máximo do ciclo
// de gravação (tWC = 10 ms). Versão simples para teste da placa; o driver
// definitivo usa "data polling" para não esperar mais que o necessário.
void writeByte(uint16_t address, uint8_t value) {
    Bus::oeHigh();              // a EEPROM não pode dirigir o barramento
    Bus::setAddress(address);
    Bus::dataAsOutput();
    Bus::writeData(value);
    Bus::ceLow();
    Bus::weLow();               // endereço é travado na borda de descida
    Bus::writePulseDelay();
    Bus::weHigh();              // dado é travado na borda de subida
    Bus::ceHigh();
    Bus::dataAsInput();
    delay(10);
}

// ---------------------------------------------------------------------------
//  Interpretação dos comandos
// ---------------------------------------------------------------------------

// Converte o próximo token da linha (separado por espaços) em número
// hexadecimal. Retorna false se não houver mais tokens ou se for inválido.
bool nextHex(char*& cursor, uint32_t& out) {
    while (*cursor == ' ') ++cursor;
    if (*cursor == '\0') return false;
    char* end = nullptr;
    out = strtoul(cursor, &end, 16);
    if (end == cursor) return false;  // nenhum dígito válido
    cursor = end;
    return true;
}

// Dump hexadecimal com 16 bytes por linha, no formato "EEEE: bb bb ...".
void commandRead(char* args) {
    uint32_t address = 0, count = 16;
    if (!nextHex(args, address)) {
        Serial.println(F("uso: r <end> [qtd]"));
        return;
    }
    nextHex(args, count);  // opcional
    if (count == 0 || count > 256) count = 16;

    for (uint32_t i = 0; i < count; ++i) {
        const uint16_t a = (uint16_t)(address + i);
        if (i % 16 == 0) {
            if (i) Serial.println();
            printHex16(a);
            Serial.print(':');
        }
        Serial.print(' ');
        printHex8(readByte(a));
    }
    Serial.println();
}

// Grava uma sequência de bytes a partir do endereço informado e confere
// cada um por releitura.
void commandWrite(char* args) {
    uint32_t address = 0, value = 0;
    if (!nextHex(args, address) || !nextHex(args, value)) {
        Serial.println(F("uso: w <end> <b0> [b1...]"));
        return;
    }
    uint16_t a = (uint16_t)address;
    uint8_t errors = 0;
    do {
        writeByte(a, (uint8_t)value);
        if (readByte(a) != (uint8_t)value) {
            Serial.print(F("ERRO de verificacao em "));
            printHex16(a);
            Serial.println();
            ++errors;
        }
        ++a;
    } while (nextHex(args, value));
    Serial.println(errors ? F("concluido com erros") : F("ok"));
}

// Executa a linha completa que está em lineBuffer.
void execute() {
    char* args = lineBuffer + 1;  // tudo após a letra do comando
    uint32_t value = 0;

    switch (lineBuffer[0]) {
        case '\0':
            break;  // linha vazia: ignora

        case 'h':
        case '?':
            printHelp();
            break;

        case 'a':
            if (nextHex(args, value)) {
                Bus::setAddress((uint16_t)value);
                Serial.print(F("endereco = "));
                printHex16((uint16_t)value);
                Serial.println();
            } else {
                Serial.println(F("uso: a <end>"));
            }
            break;

        case 'd':
            if (nextHex(args, value)) {
                Bus::oeHigh();  // garante que a EEPROM não dirige o barramento
                Bus::dataAsOutput();
                Bus::writeData((uint8_t)value);
                Serial.print(F("dados = "));
                printHex8((uint8_t)value);
                Serial.println();
            } else {
                Serial.println(F("uso: d <byte>"));
            }
            break;

        case 'z':
            Bus::dataAsInput();
            Serial.println(F("barramento de dados em alta impedancia"));
            break;

        case 'r':
            commandRead(args);
            break;

        case 'w':
            commandWrite(args);
            break;

        default:
            Serial.println(F("comando desconhecido (h = ajuda)"));
            break;
    }
}

}  // namespace

void begin() {
    Serial.println();
    Serial.println(F("EPROMnator - shell de depuracao"));
    printHelp();
    Serial.print(F("> "));
}

void poll() {
    while (Serial.available()) {
        const char c = (char)Serial.read();

        if (c == '\r' || c == '\n') {
            // Fim de linha: executa o comando (se houver algo digitado).
            // Ignora o '\n' que vem logo após um '\r' (terminais CR+LF).
            if (lineLength == 0 && c == '\n') continue;
            Serial.println();
            lineBuffer[lineLength] = '\0';
            execute();
            lineLength = 0;
            Serial.print(F("> "));
        } else if (c == 0x08 || c == 0x7F) {
            // Backspace/Delete: apaga o último caractere.
            if (lineLength) {
                --lineLength;
                Serial.print(F("\b \b"));
            }
        } else if (lineLength < LINE_MAX - 1) {
            // Caractere comum: guarda em minúsculo e ecoa para o terminal.
            lineBuffer[lineLength++] = (char)tolower(c);
            Serial.print(c);
        }
    }
}

}  // namespace DebugShell
