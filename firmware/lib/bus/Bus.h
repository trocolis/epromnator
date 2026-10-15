// =============================================================================
//  Bus.h — camada de abstração de hardware (HAL) do gravador
// =============================================================================
//
//  Este módulo conhece a ligação física entre o Arduino Nano e a EEPROM e
//  oferece primitivas de baixo nível: colocar um endereço no barramento,
//  escrever/ler o barramento de dados e acionar os sinais de controle.
//
//  IMPORTANTE — por que manipulação direta de porta?
//  -------------------------------------------------
//  As funções do Arduino (digitalWrite, shiftOut) gastam vários microssegundos
//  por chamada. Na escrita em modo página do 28C256, o intervalo entre a carga
//  de um byte e a do próximo (tBLC) NÃO pode passar de 150 µs; caso contrário o
//  chip encerra a página e inicia o ciclo de gravação. Deslocar 16 bits com
//  shiftOut + digitalWrite chega perto desse limite, então aqui usamos acesso
//  direto aos registradores PORTx/DDRx/PINx, que compila para instruções
//  sbi/cbi de 2 ciclos (125 ns a 16 MHz).
//
//  Mapa de pinos (conferido na netlist do esquema):
//
//    Sinal           Pino Nano   Porta AVR
//    --------------  ---------   ---------
//    EEPROM D0..D5   D2..D7      PD2..PD7
//    EEPROM D6..D7   D8..D9      PB0..PB1
//    ~CE             D10         PB2        (pull-up 10k na placa)
//    ~OE             D11         PB3        (pull-up 10k na placa)
//    ~WE             D12         PB4        (pull-up 10k na placa)
//    LED "ocupado"   D13         PB5        (LED onboard do Nano)
//    SER   (595)     A3          PC3
//    SRCLK (595)     A4          PC4
//    RCLK  (595)     A5          PC5
//
//  PD0/PD1 são RX/TX da serial e NUNCA podem ser alterados por este módulo.
// =============================================================================
#pragma once

#include <stdint.h>
#include <avr/io.h>

// Força a expansão em linha: as funções abaixo ficam em caminhos críticos de
// tempo e não podem pagar o custo de uma chamada de função.
#define BUS_INLINE static inline __attribute__((always_inline))

namespace Bus {

// ---------------------------------------------------------------------------
//  Máscaras de bits de cada sinal dentro da sua porta
// ---------------------------------------------------------------------------

// Barramento de dados: 6 bits altos da PORTD + 2 bits baixos da PORTB.
constexpr uint8_t DATA_MASK_D = 0xFC;  // PD2..PD7 → D0..D5
constexpr uint8_t DATA_MASK_B = 0x03;  // PB0..PB1 → D6..D7

// Sinais de controle da EEPROM (todos ativos em nível baixo).
constexpr uint8_t PIN_CE = _BV(PB2);
constexpr uint8_t PIN_OE = _BV(PB3);
constexpr uint8_t PIN_WE = _BV(PB4);

// LED onboard do Nano, usado como indicador de "operação em andamento".
constexpr uint8_t PIN_LED = _BV(PB5);

// Sinais dos registradores de deslocamento 74HC595.
constexpr uint8_t PIN_SER   = _BV(PC3);
constexpr uint8_t PIN_SRCLK = _BV(PC4);
constexpr uint8_t PIN_RCLK  = _BV(PC5);

// ---------------------------------------------------------------------------
//  Inicialização e endereçamento (implementados em Bus.cpp)
// ---------------------------------------------------------------------------

// Coloca todos os pinos em estado seguro: controles inativos (nível alto),
// barramento de dados como entrada e endereço 0x0000.
void begin();

// Desloca o endereço de 16 bits para os dois 74HC595 e o transfere para as
// saídas com um pulso em RCLK. Leva cerca de 6 µs.
void setAddress(uint16_t address);

// ---------------------------------------------------------------------------
//  Pequenos atrasos
// ---------------------------------------------------------------------------

// Atraso de ~62,5 ns (1 ciclo a 16 MHz) por NOP.
BUS_INLINE void nop() { __asm__ __volatile__("nop"); }

// Atraso para cobrir o tempo de acesso de leitura da EEPROM (tACC/tOE, até
// 150 ns no 28C256-15) somado ao atraso RC dos resistores série de 220R.
// 6 NOPs ≈ 375 ns, com margem confortável.
BUS_INLINE void accessDelay() {
    nop(); nop(); nop(); nop(); nop(); nop();
}

// Largura mínima do pulso de ~WE (tWP ≥ 100 ns). 3 NOPs ≈ 190 ns.
BUS_INLINE void writePulseDelay() {
    nop(); nop(); nop();
}

// ---------------------------------------------------------------------------
//  Barramento de dados
// ---------------------------------------------------------------------------

// Configura os 8 pinos de dados como ENTRADA (alta impedância), sem pull-up.
// Deve ser chamada ANTES de baixar ~OE, para que o Arduino e a EEPROM nunca
// tentem dirigir o barramento ao mesmo tempo (os 220R limitam a corrente,
// mas é melhor não depender deles).
BUS_INLINE void dataAsInput() {
    DDRD  &= ~DATA_MASK_D;
    DDRB  &= ~DATA_MASK_B;
    PORTD &= ~DATA_MASK_D;  // desliga os pull-ups internos
    PORTB &= ~DATA_MASK_B;
}

// Configura os 8 pinos de dados como SAÍDA. Só pode ser chamada com ~OE em
// nível alto (EEPROM com as saídas desligadas).
BUS_INLINE void dataAsOutput() {
    DDRD |= DATA_MASK_D;
    DDRB |= DATA_MASK_B;
}

// Coloca um byte no barramento de dados. Os bits 0..5 vão para PD2..PD7 e os
// bits 6..7 vão para PB0..PB1. PD0/PD1 (serial) e PB2..PB7 (controles) são
// preservados.
BUS_INLINE void writeData(uint8_t value) {
    PORTD = (PORTD & ~DATA_MASK_D) | (uint8_t)(value << 2);
    PORTB = (PORTB & ~DATA_MASK_B) | (uint8_t)(value >> 6);
}

// Lê o byte presente no barramento de dados (operação inversa de writeData).
BUS_INLINE uint8_t readData() {
    return (uint8_t)((PIND >> 2) | (PINB << 6));
}

// ---------------------------------------------------------------------------
//  Sinais de controle (ativos em nível baixo)
// ---------------------------------------------------------------------------

BUS_INLINE void ceLow()  { PORTB &= ~PIN_CE; }  // seleciona o chip
BUS_INLINE void ceHigh() { PORTB |=  PIN_CE; }  // desseleciona o chip
BUS_INLINE void oeLow()  { PORTB &= ~PIN_OE; }  // habilita as saídas da EEPROM
BUS_INLINE void oeHigh() { PORTB |=  PIN_OE; }  // desabilita as saídas
BUS_INLINE void weLow()  { PORTB &= ~PIN_WE; }  // início do pulso de escrita
BUS_INLINE void weHigh() { PORTB |=  PIN_WE; }  // fim do pulso (dado é travado)

// Liga/desliga o LED de "ocupado" (D13).
BUS_INLINE void busyLed(bool on) {
    if (on) PORTB |= PIN_LED; else PORTB &= ~PIN_LED;
}

}  // namespace Bus
