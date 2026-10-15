// =============================================================================
//  epromnator-cli — interface de linha de comando do gravador
// =============================================================================
//
//  Exemplos:
//    epromnator-cli list-ports
//    epromnator-cli -p /dev/ttyUSB0 -d AT28C256 read dump.bin
//    epromnator-cli -p COM3 write rom.hex --skip-ff
//    epromnator-cli -p emu auto rom.bin          (emulador, sem a placa)
//
//  Códigos de saída:
//    0 sucesso · 1 erro · 2 verificação falhou/chip não vazio · 3 uso incorreto
// =============================================================================
#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "epromnator/Checksum.h"
#include "epromnator/Device.h"
#include "epromnator/Emulator.h"
#include "epromnator/Error.h"
#include "epromnator/FileFormats.h"
#include "epromnator/Programmer.h"
#include "epromnator/SerialPort.h"
#include "epromnator/Version.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif

using namespace epn;

namespace {

// Códigos de saída.
constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitVerifyFailed = 2;
constexpr int kExitUsage = 3;

// Sinalizado por Ctrl+C: a operação em andamento é cancelada de forma
// limpa no próximo bloco (o chip nunca fica no meio de um ciclo).
std::atomic<bool> gCancel{false};

// =============================================================================
//  Opções de linha de comando
// =============================================================================

struct Options {
    std::string operation;          // read, write, verify...
    std::string file;               // arquivo da operação (quando houver)
    std::string port;               // vazio = detectar automaticamente
    unsigned baud = 115200;
    std::string device = "AT28C256";
    std::optional<FileFormat> format;
    int64_t offset = 0;
    Range range;
    bool skipFF = false;
    bool verify = true;
    bool erase = true;              // apenas para "auto"
    bool protectedWrite = false;
    bool quiet = false;
};

void printUsage() {
    std::cout <<
        "Uso: epromnator-cli [opções] <operação> [arquivo]\n"
        "\n"
        "Operações:\n"
        "  read <arquivo>      lê o chip e salva no arquivo\n"
        "  write <arquivo>     grava o arquivo no chip (e verifica)\n"
        "  verify <arquivo>    compara o chip com o arquivo\n"
        "  erase               apaga o chip (preenche com 0xFF)\n"
        "  blank               verifica se o chip está vazio\n"
        "  auto <arquivo>      apagar + verificar vazio + gravar + verificar\n"
        "  sdp-on | sdp-off    habilita/desabilita a proteção SDP\n"
        "  info                mostra informações do gravador\n"
        "  checksum <arquivo>  soma16 e CRC32 do arquivo como imagem do chip\n"
        "  list-devices        lista os chips suportados\n"
        "  list-ports          lista as portas seriais\n"
        "\n"
        "Opções:\n"
        "  -p, --port PORTA    porta serial (/dev/ttyUSB0, COM3...) ou \"emu\"\n"
        "  -b, --baud N        velocidade da serial (padrão 115200)\n"
        "  -d, --device NOME   chip (padrão AT28C256; ver list-devices)\n"
        "  -f, --format FMT    bin | hex | srec (padrão: pela extensão)\n"
        "  -o, --offset N      deslocamento ao carregar o arquivo (pode ser negativo)\n"
        "  -s, --start N       endereço inicial da faixa\n"
        "  -n, --length N      tamanho da faixa (padrão: até o fim do chip)\n"
        "      --skip-ff       não grava páginas inteiras em 0xFF\n"
        "      --no-verify     não verifica após gravar\n"
        "      --no-erase      (auto) não apaga antes de gravar\n"
        "      --protected     escrita protegida (grava chips com SDP ativo)\n"
        "  -q, --quiet         não mostra a barra de progresso\n"
        "  -h, --help          mostra esta ajuda\n"
        "  -V, --version       mostra a versão\n"
        "\n"
        "Números aceitam decimal, hexadecimal (0x) e sufixo k (ex.: 32k).\n";
}

// Converte "0x8000", "32768", "32k", "-0x8000" em número.
int64_t parseNumber(const std::string& text) {
    std::string s = text;
    int64_t multiplier = 1;
    if (!s.empty() && (s.back() == 'k' || s.back() == 'K')) {
        multiplier = 1024;
        s.pop_back();
    }
    size_t used = 0;
    int64_t value = 0;
    try {
        value = std::stoll(s, &used, 0);  // base 0: aceita o prefixo 0x
    } catch (const std::exception&) {
        used = 0;  // vazio, não numérico ou grande demais
    }
    if (used == 0 || used != s.size()) throw std::invalid_argument("número inválido: " + text);
    return value * multiplier;
}

FileFormat parseFormat(const std::string& s) {
    if (s == "bin") return FileFormat::Binary;
    if (s == "hex" || s == "ihex") return FileFormat::IntelHex;
    if (s == "srec" || s == "s19" || s == "mot") return FileFormat::SRecord;
    throw std::invalid_argument("formato desconhecido: " + s);
}

// Interpreta argv. Retorna nullopt (após imprimir a mensagem) se houver erro
// de uso; lança para opções com valores inválidos.
std::optional<Options> parseArguments(int argc, char** argv) {
    Options o;
    std::vector<std::string> positional;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        // Obtém o valor da opção atual (próximo argumento).
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) throw std::invalid_argument("falta o valor de " + arg);
            return argv[++i];
        };

        if (arg == "-h" || arg == "--help") {
            printUsage();
            std::exit(kExitOk);
        } else if (arg == "-V" || arg == "--version") {
            std::cout << kAppName << " " << kAppVersion << "\n";
            std::exit(kExitOk);
        } else if (arg == "-p" || arg == "--port") {
            o.port = value();
        } else if (arg == "-b" || arg == "--baud") {
            o.baud = static_cast<unsigned>(parseNumber(value()));
        } else if (arg == "-d" || arg == "--device") {
            o.device = value();
        } else if (arg == "-f" || arg == "--format") {
            o.format = parseFormat(value());
        } else if (arg == "-o" || arg == "--offset") {
            o.offset = parseNumber(value());
        } else if (arg == "-s" || arg == "--start") {
            o.range.start = static_cast<uint32_t>(parseNumber(value()));
        } else if (arg == "-n" || arg == "--length") {
            o.range.length = static_cast<uint32_t>(parseNumber(value()));
        } else if (arg == "--skip-ff") {
            o.skipFF = true;
        } else if (arg == "--no-verify") {
            o.verify = false;
        } else if (arg == "--no-erase") {
            o.erase = false;
        } else if (arg == "--protected") {
            o.protectedWrite = true;
        } else if (arg == "-q" || arg == "--quiet") {
            o.quiet = true;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "Opção desconhecida: " << arg << "\n";
            return std::nullopt;
        } else {
            positional.push_back(arg);
        }
    }

    if (positional.empty()) {
        printUsage();
        return std::nullopt;
    }
    o.operation = positional[0];
    if (positional.size() > 1) o.file = positional[1];
    if (positional.size() > 2) {
        std::cerr << "Argumentos demais.\n";
        return std::nullopt;
    }

    // Operações que exigem arquivo.
    const bool needsFile = o.operation == "read" || o.operation == "write" ||
                           o.operation == "verify" || o.operation == "auto" ||
                           o.operation == "checksum";
    if (needsFile && o.file.empty()) {
        std::cerr << "A operação '" << o.operation << "' exige um arquivo.\n";
        return std::nullopt;
    }
    return o;
}

// =============================================================================
//  Saída no terminal
// =============================================================================

// Barra de progresso em uma única linha (stderr), redesenhada no lugar.
void drawProgress(const std::string& stage, uint64_t done, uint64_t total) {
    constexpr int kWidth = 30;
    const int percent = total ? static_cast<int>(done * 100 / total) : 100;
    const int filled = percent * kWidth / 100;
    std::fprintf(stderr, "\r%-18s [%s%s] %3d%%", stage.c_str(), std::string(filled, '#').c_str(),
                 std::string(kWidth - filled, '.').c_str(), percent);
    if (done >= total) std::fputc('\n', stderr);
    std::fflush(stderr);
}

void printLog(LogLevel level, const std::string& message) {
    switch (level) {
        case LogLevel::Info:    std::cout << message << "\n"; break;
        case LogLevel::Warning: std::cout << "AVISO: " << message << "\n"; break;
        case LogLevel::Error:   std::cout << "ERRO: " << message << "\n"; break;
    }
}

// Completa o texto com espaços até `width` CARACTERES (não bytes): o printf
// conta bytes e desalinha colunas com acentos em UTF-8 ("Genérico").
std::string padRight(const std::string& text, size_t width) {
    size_t chars = 0;
    for (unsigned char c : text) {
        if ((c & 0xC0) != 0x80) ++chars;  // ignora bytes de continuação UTF-8
    }
    return chars >= width ? text : text + std::string(width - chars, ' ');
}

void printDifferences(const std::vector<Difference>& diffs) {
    constexpr size_t kMaxShown = 20;
    for (size_t i = 0; i < diffs.size() && i < kMaxShown; ++i) {
        std::printf("  %s: esperado %02X, lido %02X\n", formatAddress(diffs[i].address).c_str(),
                    diffs[i].expected, diffs[i].actual);
    }
    if (diffs.size() > kMaxShown) {
        std::printf("  ... e mais %zu diferença(s)\n", diffs.size() - kMaxShown);
    }
}

// =============================================================================
//  Operações
// =============================================================================

// Carrega o arquivo em uma imagem do tamanho do chip (pré-preenchida com 0xFF).
std::vector<uint8_t> loadImage(const Options& o, const Device& device) {
    std::vector<uint8_t> image(device.size, 0xFF);
    const FileFormat format = o.format.value_or(detectFormat(o.file));
    const LoadResult r = loadFile(o.file, format, image, o.offset);
    std::cout << "Arquivo " << o.file << " (" << formatName(format) << "): " << r.bytesLoaded
              << " bytes em " << formatAddress(r.lowestAddress) << ".."
              << formatAddress(r.highestAddress) << "\n";
    if (r.bytesIgnored) {
        std::cout << "AVISO: " << r.bytesIgnored << " bytes fora da capacidade do " << device.name
                  << " foram ignorados (use --offset?)\n";
    }
    return image;
}

// Abre a porta pedida, ou detecta automaticamente se houver só uma.
std::unique_ptr<Transport> openTransport(const Options& o, const Device& device) {
    if (o.port == "emu") return std::make_unique<EmulatorTransport>(device.size);

    std::string port = o.port;
    if (port.empty()) {
        const auto ports = SerialPort::listPorts();
        if (ports.size() != 1) {
            throw Error(ports.empty() ? "Nenhuma porta serial encontrada; informe com -p"
                                      : "Várias portas encontradas; escolha uma com -p "
                                        "(ver list-ports)");
        }
        port = ports.front().path;
    }
    return std::make_unique<SerialPort>(port, o.baud);
}

int run(const Options& o) {
    // --- Operações que não usam o gravador -------------------------------
    if (o.operation == "list-ports") {
        const auto ports = SerialPort::listPorts();
        if (ports.empty()) std::cout << "Nenhuma porta serial USB encontrada.\n";
        for (const auto& p : ports) {
            std::cout << p.path << (p.description.empty() ? "" : "  -  " + p.description) << "\n";
        }
        return kExitOk;
    }
    if (o.operation == "list-devices") {
        std::cout << padRight("Chip", 20) << padRight("Fabricante", 18) << padRight("Tamanho", 9)
                  << padRight("Página", 8) << "SDP\n";
        for (const Device& d : deviceDatabase()) {
            std::cout << padRight(d.name, 20) << padRight(d.manufacturer, 18)
                      << padRight(std::to_string(d.size), 9)
                      << padRight(d.pageSize > 1 ? std::to_string(d.pageSize) : "byte", 8)
                      << (d.supportsSdp ? "sim" : "não") << "\n";
        }
        return kExitOk;
    }

    const Device* device = findDevice(o.device);
    if (!device) {
        std::cerr << "Chip desconhecido: " << o.device << " (ver list-devices)\n";
        return kExitUsage;
    }

    if (o.operation == "checksum") {
        const auto image = loadImage(o, *device);
        std::printf("Soma16: 0x%04X   CRC32: 0x%08X\n", sum16(image), crc32(image));
        return kExitOk;
    }

    // Valida a operação antes de abrir a porta (que reinicia o Nano).
    static const char* const kDeviceOps[] = {"read",  "write", "verify",  "erase", "blank",
                                             "auto",  "sdp-on", "sdp-off", "info"};
    if (std::none_of(std::begin(kDeviceOps), std::end(kDeviceOps),
                     [&](const char* op) { return o.operation == op; })) {
        std::cerr << "Operação desconhecida: " << o.operation << "\n";
        return kExitUsage;
    }

    // Carrega o arquivo ANTES de conectar: erros de arquivo aparecem logo.
    std::vector<uint8_t> image;
    if (o.operation == "write" || o.operation == "verify" || o.operation == "auto") {
        image = loadImage(o, *device);
    }

    // --- Conexão --------------------------------------------------------
    Programmer programmer(openTransport(o, *device));
    programmer.setLogCallback(printLog);
    programmer.setCancelFlag(&gCancel);
    if (!o.quiet) programmer.setProgressCallback(drawProgress);
    programmer.selectDevice(*device);
    programmer.setProtectedWrite(o.protectedWrite);
    const FirmwareInfo info = programmer.connect();
    std::cout << "Chip: " << device->name << " (" << device->size << " bytes)\n";

    // --- Operações com o gravador ----------------------------------------
    if (o.operation == "info") {
        std::printf("Firmware: %s, protocolo v%u\n", info.description().c_str(), info.protocolVersion);
        std::printf("READ máx.: %u bytes, WRITE máx.: %u bytes\n", info.maxReadChunk, info.maxWriteChunk);
        return kExitOk;
    }

    if (o.operation == "read") {
        const auto data = programmer.read(o.range);
        const FileFormat format = o.format.value_or(detectFormat(o.file));
        saveFile(o.file, format, data, o.range.start);
        std::printf("Salvo em %s (%s). Soma16: 0x%04X  CRC32: 0x%08X\n", o.file.c_str(),
                    formatName(format).c_str(), sum16(data), crc32(data));
        return kExitOk;
    }

    if (o.operation == "write" || o.operation == "auto") {
        std::vector<Difference> diffs;
        if (o.operation == "write") {
            ProgramOptions po;
            po.skipBlankPages = o.skipFF;
            po.verify = o.verify;
            diffs = programmer.program(image, o.range, po);
        } else {
            AutoOptions ao;
            ao.erase = o.erase;
            ao.verify = o.verify;
            diffs = programmer.autoProgram(image, o.range, ao);
        }
        printDifferences(diffs);
        return diffs.empty() ? kExitOk : kExitVerifyFailed;
    }

    if (o.operation == "verify") {
        const auto diffs = programmer.verify(image, o.range);
        printDifferences(diffs);
        return diffs.empty() ? kExitOk : kExitVerifyFailed;
    }

    if (o.operation == "erase") {
        programmer.erase(o.range);
        return kExitOk;
    }

    if (o.operation == "blank") {
        return programmer.blankCheck(o.range).blank ? kExitOk : kExitVerifyFailed;
    }

    // sdp-on / sdp-off
    programmer.setSdp(o.operation == "sdp-on");
    return kExitOk;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // Acentos corretos no console do Windows.
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::signal(SIGINT, [](int) { gCancel = true; });

    try {
        const auto options = parseArguments(argc, argv);
        if (!options) return kExitUsage;
        // Sem terminal (saída redirecionada), a barra de progresso só polui.
        Options o = *options;
        if (!isatty(fileno(stderr))) o.quiet = true;
        return run(o);
    } catch (const CancelledError& e) {
        std::fprintf(stderr, "\n%s\n", e.what());
        return kExitError;
    } catch (const std::invalid_argument& e) {
        std::cerr << "Argumento inválido: " << e.what() << "\n";
        return kExitUsage;
    } catch (const std::exception& e) {
        std::cerr << "\nERRO: " << e.what() << "\n";
        return kExitError;
    }
}
