// =============================================================================
//  Bus.cpp — implementação das rotinas não-inline da HAL
// =============================================================================
#include "Bus.h"

namespace Bus {

void begin() {
    // 1) Controles: primeiro escrevemos nível ALTO no registrador PORTB e só
    //    depois transformamos os pinos em saída. Dessa forma não existe nenhum
    //    instante em que ~CE/~OE/~WE fiquem em nível baixo (o que poderia
    //    disparar uma escrita espúria na EEPROM durante o boot).
    PORTB |= PIN_CE | PIN_OE | PIN_WE;
    PORTB &= ~PIN_LED;
    DDRB  |= PIN_CE | PIN_OE | PIN_WE | PIN_LED;

    // 2) Linhas dos 74HC595 como saída, começando em nível baixo.
    PORTC &= ~(PIN_SER | PIN_SRCLK | PIN_RCLK);
    DDRC  |= PIN_SER | PIN_SRCLK | PIN_RCLK;

    // 3) Barramento de dados em alta impedância.
    dataAsInput();

    // 4) Endereço conhecido nas saídas dos 595 (ao ligar, o conteúdo deles
    //    é aleatório, pois ~SRCLR está fixo em +5V na placa).
    setAddress(0);
}

void setAddress(uint16_t address) {
    // Os dois 595 estão em cascata: U1 recebe SER do Arduino e o seu QH'
    // alimenta o SER de U2. Após 16 pulsos de clock, o PRIMEIRO bit enviado
    // termina na saída mais distante (U2.QH = A15) e o ÚLTIMO bit fica em
    // U1.QA = A0. Por isso enviamos do bit 15 para o bit 0 (MSB primeiro).
    for (uint8_t i = 0; i < 16; ++i) {
        if (address & 0x8000) {
            PORTC |= PIN_SER;
        } else {
            PORTC &= ~PIN_SER;
        }
        // Pulso de clock do registrador de deslocamento (borda de subida).
        // Cada sbi/cbi leva 125 ns, bem acima do mínimo de ~20 ns do 74HC595.
        PORTC |= PIN_SRCLK;
        PORTC &= ~PIN_SRCLK;
        address <<= 1;
    }
    // Pulso em RCLK: copia o registrador de deslocamento para as saídas.
    // Como ~OE dos 595 está fixo em GND, o endereço aparece imediatamente
    // nos pinos A0..A14 da EEPROM.
    PORTC |= PIN_RCLK;
    PORTC &= ~PIN_RCLK;
}

}  // namespace Bus
