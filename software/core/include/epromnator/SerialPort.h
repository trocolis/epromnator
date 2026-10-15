// =============================================================================
//  SerialPort.h — porta serial multiplataforma (Linux, macOS e Windows)
// =============================================================================
//
//  Implementação própria e enxuta, sem dependências externas:
//    * POSIX (Linux/macOS): termios + poll();
//    * Windows: API Win32 (CreateFile/ReadFile/WriteFile + COMMTIMEOUTS).
//
//  Configuração fixa em 8N1, sem controle de fluxo. Ao abrir a porta o sinal
//  DTR é ativado, o que reinicia o Arduino Nano (comportamento esperado: o
//  firmware responde com HELLO quando termina o boot).
// =============================================================================
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "epromnator/Transport.h"

namespace epn {

// Informações de uma porta serial disponível no sistema.
struct PortInfo {
    std::string path;         // "/dev/ttyUSB0", "COM3", ...
    std::string description;  // ex.: "USB2.0-Serial (1a86:7523)", pode ser vazia
};

class SerialPort final : public Transport {
public:
    // Abre a porta. Lança TransportError se não conseguir.
    SerialPort(const std::string& path, unsigned baud);
    ~SerialPort() override;

    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;

    void write(std::span<const uint8_t> data) override;
    size_t read(std::span<uint8_t> buffer, std::chrono::milliseconds timeout) override;
    void flushInput() override;
    std::string name() const override { return path_; }

    // Lista as portas seriais prováveis de serem o gravador
    // (USB-serial: CH340, FTDI, CP210x, CDC-ACM...).
    static std::vector<PortInfo> listPorts();

private:
    // Detalhes de cada sistema operacional ficam escondidos no .cpp.
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string path_;
};

}  // namespace epn
