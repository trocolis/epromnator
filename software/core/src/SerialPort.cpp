// =============================================================================
//  SerialPort.cpp — implementação POSIX e Win32 da porta serial
// =============================================================================
#include "epromnator/SerialPort.h"

#include <algorithm>

#include "epromnator/Error.h"

#ifdef _WIN32
// -----------------------------------------------------------------------------
//  Windows
// -----------------------------------------------------------------------------
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace epn {

namespace {

// Mensagem de erro do Windows para a última falha.
std::string lastErrorMessage() {
    const DWORD code = GetLastError();
    char* text = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<LPSTR>(&text), 0, nullptr);
    std::string msg = text ? text : "erro desconhecido";
    if (text) LocalFree(text);
    // Remove a quebra de linha que o Windows coloca no fim.
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
    return msg;
}

}  // namespace

struct SerialPort::Impl {
    HANDLE handle = INVALID_HANDLE_VALUE;
    DWORD currentTimeoutMs = MAXDWORD;  // último timeout aplicado

    // Ajusta o timeout de leitura somente quando ele muda (a chamada é cara).
    void setReadTimeout(DWORD ms) {
        if (ms == currentTimeoutMs) return;
        COMMTIMEOUTS t{};
        // Combinação que faz ReadFile retornar assim que houver ao menos um
        // byte, ou após `ms` milissegundos sem nenhum byte.
        t.ReadIntervalTimeout = MAXDWORD;
        t.ReadTotalTimeoutMultiplier = MAXDWORD;
        t.ReadTotalTimeoutConstant = ms ? ms : 1;
        t.WriteTotalTimeoutConstant = 2000;
        SetCommTimeouts(handle, &t);
        currentTimeoutMs = ms;
    }
};

SerialPort::SerialPort(const std::string& path, unsigned baud)
    : impl_(std::make_unique<Impl>()), path_(path) {
    // O prefixo \\.\ é obrigatório para COM10 em diante.
    const std::string device = path.rfind("\\\\.\\", 0) == 0 ? path : "\\\\.\\" + path;
    impl_->handle = CreateFileA(device.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_EXISTING, 0, nullptr);
    if (impl_->handle == INVALID_HANDLE_VALUE) {
        throw TransportError("Não foi possível abrir " + path + ": " + lastErrorMessage());
    }

    // 8N1, sem controle de fluxo, DTR ativo (reinicia o Nano).
    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    GetCommState(impl_->handle, &dcb);
    dcb.BaudRate = baud;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    if (!SetCommState(impl_->handle, &dcb)) {
        const std::string msg = lastErrorMessage();
        CloseHandle(impl_->handle);
        throw TransportError("Não foi possível configurar " + path + ": " + msg);
    }
    impl_->setReadTimeout(100);
    PurgeComm(impl_->handle, PURGE_RXCLEAR | PURGE_TXCLEAR);
}

SerialPort::~SerialPort() {
    if (impl_ && impl_->handle != INVALID_HANDLE_VALUE) CloseHandle(impl_->handle);
}

void SerialPort::write(std::span<const uint8_t> data) {
    size_t done = 0;
    while (done < data.size()) {
        DWORD written = 0;
        if (!WriteFile(impl_->handle, data.data() + done,
                       static_cast<DWORD>(data.size() - done), &written, nullptr) ||
            written == 0) {
            throw TransportError("Falha ao escrever na porta " + path_ + ": " +
                                 lastErrorMessage());
        }
        done += written;
    }
}

size_t SerialPort::read(std::span<uint8_t> buffer, std::chrono::milliseconds timeout) {
    impl_->setReadTimeout(static_cast<DWORD>(std::max<long long>(timeout.count(), 0)));
    DWORD got = 0;
    if (!ReadFile(impl_->handle, buffer.data(), static_cast<DWORD>(buffer.size()), &got,
                  nullptr)) {
        throw TransportError("Falha ao ler da porta " + path_ + ": " + lastErrorMessage());
    }
    return got;
}

void SerialPort::flushInput() { PurgeComm(impl_->handle, PURGE_RXCLEAR); }

std::vector<PortInfo> SerialPort::listPorts() {
    // QueryDosDevice informa se COMn existe, sem precisar abrir a porta.
    std::vector<PortInfo> ports;
    char target[512];
    for (int i = 1; i <= 256; ++i) {
        const std::string name = "COM" + std::to_string(i);
        if (QueryDosDeviceA(name.c_str(), target, sizeof(target)) != 0) {
            // O alvo indica o driver, ex.: "\Device\Serial0" ou "\Device\USBSER000".
            ports.push_back({name, target});
        }
    }
    return ports;
}

}  // namespace epn

#else
// -----------------------------------------------------------------------------
//  POSIX (Linux e macOS)
// -----------------------------------------------------------------------------
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>

namespace epn {

namespace {

// Converte a velocidade numérica para a constante do termios.
speed_t toSpeed(unsigned baud) {
    switch (baud) {
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
#ifdef B500000
        case 500000: return B500000;
#endif
#ifdef B1000000
        case 1000000: return B1000000;
#endif
        default:
            throw TransportError("Velocidade não suportada: " + std::to_string(baud));
    }
}

std::string errnoMessage() { return std::strerror(errno); }

#ifdef __linux__
// Lê a primeira linha de um arquivo do sysfs (ou "" se não existir).
std::string readSysfs(const std::string& path) {
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    return line;
}

// Monta a descrição de uma porta USB a partir do sysfs, ex.:
// "USB2.0-Serial (1a86:7523)". O diretório do dispositivo USB fica dois ou
// três níveis acima de /sys/class/tty/<nome>/device, dependendo do driver.
std::string describeLinuxPort(const std::string& name) {
    const std::string base = "/sys/class/tty/" + name + "/device";
    for (const char* up : {"/..", "/../.."}) {
        const std::string dir = base + up;
        const std::string vid = readSysfs(dir + "/idVendor");
        if (!vid.empty()) {
            const std::string pid = readSysfs(dir + "/idProduct");
            std::string product = readSysfs(dir + "/product");
            if (product.empty()) product = "USB serial";
            return product + " (" + vid + ":" + pid + ")";
        }
    }
    return {};
}
#endif

}  // namespace

struct SerialPort::Impl {
    int fd = -1;
};

SerialPort::SerialPort(const std::string& path, unsigned baud)
    : impl_(std::make_unique<Impl>()), path_(path) {
    // O_NONBLOCK: open() não fica preso esperando a linha DCD; as leituras
    // usam poll() para respeitar o timeout.
    impl_->fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (impl_->fd < 0) {
        std::string hint;
#ifdef __linux__
        if (errno == EACCES) {
            hint = " (adicione o usuário ao grupo 'uucp' ou 'dialout')";
        }
#endif
        throw TransportError("Não foi possível abrir " + path + ": " + errnoMessage() + hint);
    }

    termios tio{};
    if (tcgetattr(impl_->fd, &tio) != 0) {
        const std::string msg = errnoMessage();
        ::close(impl_->fd);
        throw TransportError("Não é uma porta serial: " + path + " (" + msg + ")");
    }

    // Modo "cru": sem eco, sem tradução de CR/LF, sem caracteres especiais.
    cfmakeraw(&tio);
    tio.c_cflag |= CLOCAL | CREAD;  // ignora sinais de modem, habilita leitura
    tio.c_cflag &= ~CSTOPB;         // 1 stop bit
    tio.c_cflag &= ~CRTSCTS;        // sem controle de fluxo por hardware
    tio.c_cflag |= HUPCL;           // DTR cai ao fechar (e sobe ao abrir)
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;

    try {
        const speed_t speed = toSpeed(baud);
        cfsetispeed(&tio, speed);
        cfsetospeed(&tio, speed);
    } catch (...) {
        ::close(impl_->fd);
        throw;
    }

    if (tcsetattr(impl_->fd, TCSANOW, &tio) != 0) {
        const std::string msg = errnoMessage();
        ::close(impl_->fd);
        throw TransportError("Não foi possível configurar " + path + ": " + msg);
    }

    // Garante DTR/RTS ativos (reset do Nano) e descarta lixo antigo.
    int bits = TIOCM_DTR | TIOCM_RTS;
    ioctl(impl_->fd, TIOCMBIS, &bits);
    tcflush(impl_->fd, TCIOFLUSH);
}

SerialPort::~SerialPort() {
    if (impl_ && impl_->fd >= 0) ::close(impl_->fd);
}

void SerialPort::write(std::span<const uint8_t> data) {
    size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = ::write(impl_->fd, data.data() + done, data.size() - done);
        if (n > 0) {
            done += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno != EAGAIN && errno != EINTR) {
            throw TransportError("Falha ao escrever em " + path_ + ": " + errnoMessage());
        }
        // Buffer de saída cheio: espera ficar livre (até 2 s).
        pollfd pfd{impl_->fd, POLLOUT, 0};
        if (::poll(&pfd, 1, 2000) <= 0) {
            throw TransportError("Tempo esgotado escrevendo em " + path_);
        }
    }
}

size_t SerialPort::read(std::span<uint8_t> buffer, std::chrono::milliseconds timeout) {
    pollfd pfd{impl_->fd, POLLIN, 0};
    const int ready = ::poll(&pfd, 1, static_cast<int>(std::max<long long>(timeout.count(), 0)));
    if (ready < 0) {
        if (errno == EINTR) return 0;
        throw TransportError("Falha ao aguardar dados de " + path_ + ": " + errnoMessage());
    }
    if (ready == 0) return 0;  // timeout
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        throw TransportError("A porta " + path_ + " foi desconectada");
    }
    const ssize_t n = ::read(impl_->fd, buffer.data(), buffer.size());
    if (n < 0) {
        if (errno == EAGAIN || errno == EINTR) return 0;
        throw TransportError("Falha ao ler de " + path_ + ": " + errnoMessage());
    }
    return static_cast<size_t>(n);
}

void SerialPort::flushInput() { tcflush(impl_->fd, TCIFLUSH); }

std::vector<PortInfo> SerialPort::listPorts() {
    std::vector<PortInfo> ports;
    DIR* dir = opendir("/dev");
    if (!dir) return ports;

    while (dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
#ifdef __APPLE__
        // No macOS usamos os dispositivos "cu." (call-up), que não esperam DCD.
        const bool match = name.rfind("cu.usbserial", 0) == 0 ||
                           name.rfind("cu.wchusbserial", 0) == 0 ||
                           name.rfind("cu.usbmodem", 0) == 0 ||
                           name.rfind("cu.SLAB_USBtoUART", 0) == 0;
        if (match) ports.push_back({"/dev/" + name, {}});
#else
        // Linux: conversores USB-serial (CH340, FTDI, CP210x) e CDC-ACM.
        if (name.rfind("ttyUSB", 0) == 0 || name.rfind("ttyACM", 0) == 0) {
            ports.push_back({"/dev/" + name, describeLinuxPort(name)});
        }
#endif
    }
    closedir(dir);

    std::sort(ports.begin(), ports.end(),
              [](const PortInfo& a, const PortInfo& b) { return a.path < b.path; });
    return ports;
}

}  // namespace epn

#endif
