// =============================================================================
//  Programmer.cpp
// =============================================================================
#include "epromnator/Programmer.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "epromnator/Checksum.h"
#include "epromnator/Error.h"
#include "epromnator_protocol.h"

namespace epn {

using namespace std::chrono_literals;

namespace {

// Tamanho do bloco usado na verificação por CRC: pequeno o bastante para
// localizar diferenças rapidamente, grande o bastante para poucas requisições.
constexpr uint32_t kVerifyBlock = 1024;

// Bloco de apagamento: 16 páginas de 64 bytes por comando (permite mostrar
// progresso e mantém cada comando abaixo de ~0,5 s).
constexpr uint32_t kEraseBlock = 1024;

// Bloco da verificação de vazio.
constexpr uint32_t kBlankBlock = 4096;

// Mede o tempo de uma operação para o log.
class Stopwatch {
public:
    Stopwatch() : start_(std::chrono::steady_clock::now()) {}
    std::string elapsed() const {
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f s", s);
        return buf;
    }

private:
    std::chrono::steady_clock::time_point start_;
};

}  // namespace

std::string formatAddress(uint32_t address) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%04X", address);
    return buf;
}

// =============================================================================
//  Construção e configuração
// =============================================================================

Programmer::Programmer(std::unique_ptr<Transport> transport)
    : transport_(std::move(transport)), client_(*transport_), device_(deviceDatabase().front()) {}

Programmer::~Programmer() = default;

FirmwareInfo Programmer::connect() {
    log(LogLevel::Info, "Conectando em " + transport_->name() + "...");
    info_ = client_.handshake();
    connected_ = true;
    log(LogLevel::Info, "Gravador encontrado: " + info_.description());
    sendConfig();
    return info_;
}

void Programmer::selectDevice(const Device& device) {
    device_ = device;
    if (connected_) sendConfig();
}

void Programmer::setProtectedWrite(bool enabled) {
    protectedWrite_ = enabled;
    if (connected_) sendConfig();
}

void Programmer::sendConfig() {
    ChipConfig config;
    config.size = device_.size;
    config.pageSize = device_.pageSize;
    config.writeTimeoutMs = device_.writeTimeoutMs;
    config.protectedWrite = protectedWrite_ && device_.supportsSdp;
    client_.configure(config);
}

// =============================================================================
//  Utilitários
// =============================================================================

Range Programmer::resolve(Range range) const {
    if (range.length == 0) {
        if (range.start >= device_.size) throw Error("Endereço inicial fora do chip");
        range.length = device_.size - range.start;
    }
    if (range.start >= device_.size || range.length > device_.size - range.start) {
        throw Error("Faixa " + formatAddress(range.start) + "+" + std::to_string(range.length) +
                    " excede a capacidade do " + device_.name);
    }
    return range;
}

void Programmer::checkImage(std::span<const uint8_t> image, Range range) const {
    if (image.size() < static_cast<size_t>(range.start) + range.length) {
        throw Error("O buffer é menor que a faixa a gravar/verificar");
    }
}

void Programmer::report(const std::string& stage, uint64_t done, uint64_t total) {
    if (progress_) progress_(stage, done, total);
}

void Programmer::log(LogLevel level, const std::string& message) {
    if (log_) log_(level, message);
}

void Programmer::checkCancel() const {
    if (cancel_ && cancel_->load()) throw CancelledError();
}

// =============================================================================
//  Leitura
// =============================================================================

std::vector<uint8_t> Programmer::read(Range range) {
    range = resolve(range);
    const Stopwatch sw;
    const uint32_t chunk = std::min<uint32_t>(info_.maxReadChunk ? info_.maxReadChunk : 256,
                                              EPN_MAX_READ_CHUNK);
    std::vector<uint8_t> data;
    data.reserve(range.length);

    for (uint32_t done = 0; done < range.length;) {
        checkCancel();
        const uint32_t n = std::min(chunk, range.length - done);
        const auto part = client_.read(range.start + done, static_cast<uint16_t>(n));
        data.insert(data.end(), part.begin(), part.end());
        done += n;
        report("Lendo", done, range.length);
    }
    log(LogLevel::Info, "Leitura concluída: " + std::to_string(range.length) + " bytes em " +
                            sw.elapsed() + ".");
    return data;
}

// =============================================================================
//  Gravação
// =============================================================================

std::vector<Difference> Programmer::program(std::span<const uint8_t> image, Range range,
                                            ProgramOptions options) {
    range = resolve(range);
    checkImage(image, range);
    const Stopwatch sw;

    // Tamanho de cada comando WRITE: uma página (modo página) ou um lote de
    // bytes que o firmware grava um a um (modo byte).
    const bool pageMode = device_.pageSize > 1;
    const uint32_t maxWrite = info_.maxWriteChunk ? info_.maxWriteChunk : EPN_MAX_WRITE_CHUNK;
    const uint32_t step = pageMode ? std::min<uint32_t>(device_.pageSize, maxWrite) : maxWrite;

    const uint32_t end = range.start + range.length;
    uint32_t skipped = 0;
    uint32_t written = 0;

    for (uint32_t address = range.start; address < end;) {
        checkCancel();

        // Não atravessa o limite de página (exigência do chip).
        uint32_t n = pageMode ? step - (address % step) : step;
        n = std::min(n, end - address);
        const auto data = image.subspan(address, n);

        const bool blank = std::all_of(data.begin(), data.end(), [](uint8_t b) { return b == 0xFF; });
        if (options.skipBlankPages && blank) {
            skipped += n;
        } else {
            // Tempo limite: um ciclo por página, ou um por byte no modo byte.
            const uint32_t cycles = pageMode ? 1 : n;
            const auto timeout = 500ms + std::chrono::milliseconds(cycles * device_.writeTimeoutMs * 2u);
            client_.write(address, data, timeout);
            written += n;
        }
        address += n;
        report("Gravando", address - range.start, range.length);
    }

    std::string msg = "Gravação concluída: " + std::to_string(written) + " bytes em " + sw.elapsed();
    if (skipped) msg += " (" + std::to_string(skipped) + " bytes 0xFF pulados)";
    log(LogLevel::Info, msg + ".");

    if (!options.verify) return {};
    return verify(image, range);
}

// =============================================================================
//  Verificação
// =============================================================================

std::vector<Difference> Programmer::verify(std::span<const uint8_t> image, Range range,
                                           size_t maxDifferences) {
    range = resolve(range);
    checkImage(image, range);
    const Stopwatch sw;
    std::vector<Difference> diffs;
    const uint32_t end = range.start + range.length;

    for (uint32_t address = range.start; address < end;) {
        checkCancel();
        const uint32_t n = std::min(kVerifyBlock, end - address);
        const auto expected = image.subspan(address, n);

        // Compara o CRC do bloco calculado no firmware com o calculado aqui;
        // apenas blocos divergentes são lidos por completo.
        if (client_.crc32(address, n) != crc32(expected)) {
            uint32_t done = 0;
            while (done < n && diffs.size() < maxDifferences) {
                const uint32_t part = std::min<uint32_t>(EPN_MAX_READ_CHUNK, n - done);
                const auto actual = client_.read(address + done, static_cast<uint16_t>(part));
                for (uint32_t i = 0; i < part && diffs.size() < maxDifferences; ++i) {
                    if (actual[i] != expected[done + i]) {
                        diffs.push_back({address + done + i, expected[done + i], actual[i]});
                    }
                }
                done += part;
            }
        }
        address += n;
        report("Verificando", address - range.start, range.length);
        if (diffs.size() >= maxDifferences) break;
    }

    if (diffs.empty()) {
        log(LogLevel::Info, "Verificação OK em " + sw.elapsed() + ".");
    } else {
        const Difference& d = diffs.front();
        char detail[96];
        std::snprintf(detail, sizeof(detail), " Primeira em %s: esperado 0x%02X, lido 0x%02X.",
                      formatAddress(d.address).c_str(), d.expected, d.actual);
        log(LogLevel::Error, "Verificação FALHOU: " + std::to_string(diffs.size()) +
                                 (diffs.size() >= maxDifferences ? "+" : "") + " diferença(s)." +
                                 detail);
    }
    return diffs;
}

// =============================================================================
//  Verificação de vazio e apagamento
// =============================================================================

BlankCheckResult Programmer::blankCheck(Range range) {
    range = resolve(range);
    const uint32_t end = range.start + range.length;

    for (uint32_t address = range.start; address < end;) {
        checkCancel();
        const uint32_t n = std::min(kBlankBlock, end - address);
        const BlankCheckResult r = client_.blankCheck(address, n);
        address += n;
        report("Verificando vazio", address - range.start, range.length);
        if (!r.blank) {
            char detail[64];
            std::snprintf(detail, sizeof(detail), "%s contém 0x%02X.",
                          formatAddress(r.firstUsedAddress).c_str(), r.value);
            log(LogLevel::Warning, std::string("O chip NÃO está vazio: ") + detail);
            return r;
        }
    }
    log(LogLevel::Info, "O chip está vazio (todos os bytes em 0xFF).");
    return {};
}

void Programmer::erase(Range range) {
    range = resolve(range);
    const Stopwatch sw;
    const uint32_t end = range.start + range.length;
    const uint32_t page = device_.pageSize > 1 ? device_.pageSize : 1;

    for (uint32_t address = range.start; address < end;) {
        checkCancel();
        const uint32_t n = std::min(kEraseBlock, end - address);
        // Pior caso: um ciclo de gravação por página (ou por byte).
        const uint32_t cycles = (n + page - 1) / page;
        client_.erase(address, n, 500ms + std::chrono::milliseconds(cycles * device_.writeTimeoutMs * 2u));
        address += n;
        report("Apagando", address - range.start, range.length);
    }
    log(LogLevel::Info, "Apagamento concluído em " + sw.elapsed() + ".");
}

// =============================================================================
//  SDP
// =============================================================================

void Programmer::setSdp(bool enabled) {
    if (!device_.supportsSdp) {
        throw Error("O dispositivo " + device_.name + " não suporta proteção SDP");
    }
    client_.setSdp(enabled, 500ms + std::chrono::milliseconds(device_.writeTimeoutMs * 2u));
    log(LogLevel::Info, enabled ? "Proteção SDP habilitada." : "Proteção SDP desabilitada.");
}

// =============================================================================
//  AUTO
// =============================================================================

std::vector<Difference> Programmer::autoProgram(std::span<const uint8_t> image, Range range,
                                                AutoOptions options) {
    range = resolve(range);
    checkImage(image, range);
    const Stopwatch sw;

    if (options.erase) erase(range);

    if (options.blankCheck) {
        const BlankCheckResult r = blankCheck(range);
        if (!r.blank) {
            throw Error("Operação AUTO interrompida: o chip não ficou vazio em " +
                        formatAddress(r.firstUsedAddress));
        }
    }

    ProgramOptions programOptions;
    programOptions.skipBlankPages = options.skipBlankPages;
    programOptions.verify = options.verify;
    auto diffs = program(image, range, programOptions);

    log(diffs.empty() ? LogLevel::Info : LogLevel::Error,
        std::string("AUTO ") + (diffs.empty() ? "concluído com sucesso" : "concluído com ERROS") +
            " em " + sw.elapsed() + ".");
    return diffs;
}

}  // namespace epn
