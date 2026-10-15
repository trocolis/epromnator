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
//    w <end> <b0> [b1...]  grava bytes a partir de <end> (respeitando páginas)
//    b                     verifica se o chip inteiro está vazio (0xFF)
//    c                     calcula o CRC32 do chip inteiro
//    e                     apaga o chip inteiro (preenche com 0xFF)
//    s <0|1>               desabilita (0) ou habilita (1) o SDP
//    t !                   AUTOTESTE DESTRUTIVO: grava um padrão no chip
//                          inteiro, relê e informa erros e tempos
// =============================================================================
#include "DebugShell.h"

#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

#include "Bus.h"
#include "Eeprom28c.h"

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
    Serial.println(F("  w <end> <b0> [b1...]  grava bytes"));
    Serial.println(F("  b                     verifica se o chip esta vazio"));
    Serial.println(F("  c                     CRC32 do chip inteiro"));
    Serial.println(F("  e                     apaga o chip inteiro"));
    Serial.println(F("  s <0|1>               desabilita/habilita SDP"));
    Serial.println(F("  t !                   autoteste DESTRUTIVO do chip inteiro"));
}

// Imprime o resultado de uma operação de gravação do driver.
void printResult(Eeprom::Result result, uint16_t failAddress) {
    switch (result) {
        case Eeprom::Result::Ok:
            Serial.println(F("ok"));
            return;
        case Eeprom::Result::Timeout:
            Serial.print(F("ERRO: tempo esgotado (SDP ativo?) em "));
            break;
        case Eeprom::Result::VerifyError:
            Serial.print(F("ERRO: verificacao falhou em "));
            break;
        case Eeprom::Result::RangeError:
            Serial.print(F("ERRO: faixa invalida em "));
            break;
    }
    printHex16(failAddress);
    Serial.println();
}

// Padrão usado pelo autoteste: depende dos dois bytes do endereço, para que
// erros de linhas de endereço (curto, aberto, troca) sejam detectados.
uint8_t testPattern(uint16_t address) {
    return (uint8_t)((address & 0xFF) ^ (address >> 8) ^ 0x5A);
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
        printHex8(Eeprom::readByte(a));
    }
    Serial.println();
}

// Grava uma sequência de bytes a partir do endereço informado. Os bytes
// são agrupados por página e o driver confere cada um por releitura.
void commandWrite(char* args) {
    uint32_t address = 0, value = 0;
    uint8_t data[24];  // uma linha de 96 caracteres cabe com folga
    uint8_t count = 0;

    if (!nextHex(args, address)) {
        Serial.println(F("uso: w <end> <b0> [b1...]"));
        return;
    }
    while (count < sizeof(data) && nextHex(args, value)) {
        data[count++] = (uint8_t)value;
    }
    if (count == 0) {
        Serial.println(F("uso: w <end> <b0> [b1...]"));
        return;
    }

    // Divide a sequência nos limites de página do chip configurado.
    const uint8_t page = Eeprom::config().pageSize > 1 ? Eeprom::config().pageSize : 1;
    uint8_t done = 0;
    uint16_t failAddress = 0;
    while (done < count) {
        const uint16_t a = (uint16_t)(address + done);
        uint8_t chunk = page - (a % page);
        if (chunk > count - done) chunk = count - done;
        const Eeprom::Result r = Eeprom::write(a, &data[done], chunk, &failAddress);
        if (r != Eeprom::Result::Ok) {
            printResult(r, failAddress);
            return;
        }
        done += chunk;
    }
    Serial.println(F("ok"));
}

// Verificação de vazio do chip inteiro.
void commandBlank() {
    uint16_t first = 0;
    uint8_t value = 0;
    if (Eeprom::isBlank(0, Eeprom::config().size, &first, &value)) {
        Serial.println(F("chip vazio"));
    } else {
        Serial.print(F("NAO vazio: "));
        printHex16(first);
        Serial.print(F(" = "));
        printHex8(value);
        Serial.println();
    }
}

// Autoteste destrutivo: grava o padrão em todas as páginas, relê tudo e
// mostra o tempo de cada fase.
void commandSelfTest(char* args) {
    while (*args == ' ') ++args;
    if (*args != '!') {
        Serial.println(F("ATENCAO: apaga todo o conteudo. Confirme com: t !"));
        return;
    }

    const uint32_t size = Eeprom::config().size;
    const uint8_t page = Eeprom::config().pageSize > 1 ? Eeprom::config().pageSize : 1;
    uint8_t buffer[Eeprom::MAX_PAGE_SIZE];
    uint16_t failAddress = 0;

    // Fase 1: gravação página a página.
    uint32_t t0 = millis();
    for (uint32_t a = 0; a < size; a += page) {
        for (uint8_t i = 0; i < page; ++i) buffer[i] = testPattern((uint16_t)(a + i));
        const Eeprom::Result r = Eeprom::write((uint16_t)a, buffer, page, &failAddress);
        if (r != Eeprom::Result::Ok) {
            printResult(r, failAddress);
            return;
        }
        if ((a % 4096) == 0) Serial.print('.');  // sinal de vida a cada 4 KB
    }
    Serial.println();
    Serial.print(F("gravacao: "));
    Serial.print(millis() - t0);
    Serial.println(F(" ms"));

    // Fase 2: leitura completa e comparação.
    t0 = millis();
    uint32_t errors = 0;
    for (uint32_t a = 0; a < size; a += page) {
        Eeprom::readBlock((uint16_t)a, buffer, page);
        for (uint8_t i = 0; i < page; ++i) {
            if (buffer[i] != testPattern((uint16_t)(a + i))) {
                if (errors < 8) {  // mostra só os primeiros erros
                    Serial.print(F("erro em "));
                    printHex16((uint16_t)(a + i));
                    Serial.print(F(": lido "));
                    printHex8(buffer[i]);
                    Serial.print(F(" esperado "));
                    printHex8(testPattern((uint16_t)(a + i)));
                    Serial.println();
                }
                ++errors;
            }
        }
    }
    Serial.print(F("leitura: "));
    Serial.print(millis() - t0);
    Serial.println(F(" ms"));
    Serial.print(F("erros: "));
    Serial.println(errors);
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

        case 'b':
            commandBlank();
            break;

        case 'c':
            Serial.print(F("CRC32 = "));
            Serial.println(Eeprom::crc32(0, Eeprom::config().size), HEX);
            break;

        case 'e': {
            uint16_t failAddress = 0;
            printResult(Eeprom::erase(0, Eeprom::config().size, &failAddress),
                        failAddress);
            break;
        }

        case 's':
            if (nextHex(args, value)) {
                if (value) Eeprom::sdpEnable(); else Eeprom::sdpDisable();
                Serial.println(value ? F("SDP habilitado") : F("SDP desabilitado"));
            } else {
                Serial.println(F("uso: s <0|1>"));
            }
            break;

        case 't':
            commandSelfTest(args);
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
