// =============================================================================
//  Programmer.h — operações de alto nível do gravador
// =============================================================================
//
//  Camada usada diretamente pela CLI e pela GUI. Divide o chip em blocos,
//  reporta progresso, permite cancelamento e implementa as operações
//  padrão dos gravadores comerciais:
//
//    Ler · Gravar · Verificar · Apagar · Verificar vazio · SDP · AUTO
//
//  Sobre o buffer ("imagem"): todas as operações recebem a imagem do chip
//  inteiro (tamanho = capacidade do dispositivo) e uma faixa de endereços
//  opcional. Isso espelha o editor hexadecimal da GUI.
//
//  Thread: um Programmer deve ser usado por uma única thread por vez. A GUI
//  o executa em uma thread de trabalho e cancela via `setCancelFlag`.
// =============================================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "epromnator/Device.h"
#include "epromnator/ProtocolClient.h"
#include "epromnator/Transport.h"

namespace epn {

enum class LogLevel { Info, Warning, Error };

// Faixa de endereços [start, start + length). length == 0 → chip inteiro.
struct Range {
    uint32_t start = 0;
    uint32_t length = 0;
};

// Diferença encontrada na verificação.
struct Difference {
    uint32_t address;
    uint8_t expected;  // valor da imagem
    uint8_t actual;    // valor lido do chip
};

struct ProgramOptions {
    // Não grava páginas cujo conteúdo é todo 0xFF (pressupõe chip apagado).
    bool skipBlankPages = false;
    // Verifica o conteúdo após gravar.
    bool verify = true;
};

struct AutoOptions {
    bool erase = true;            // 1) apaga
    bool blankCheck = true;       // 2) confere se ficou vazio
    bool skipBlankPages = true;   // 3) grava (pulando páginas 0xFF)
    bool verify = true;           // 4) verifica
};

class Programmer {
public:
    // Função de progresso: etapa ("Lendo", "Gravando"...), feito e total.
    using ProgressFn = std::function<void(const std::string& stage, uint64_t done, uint64_t total)>;
    // Função de log: nível e mensagem em português.
    using LogFn = std::function<void(LogLevel level, const std::string& message)>;

    // Assume a posse do transporte (porta serial ou emulador).
    explicit Programmer(std::unique_ptr<Transport> transport);
    ~Programmer();

    Programmer(const Programmer&) = delete;
    Programmer& operator=(const Programmer&) = delete;

    // --- Configuração -----------------------------------------------------

    void setProgressCallback(ProgressFn fn) { progress_ = std::move(fn); }
    void setLogCallback(LogFn fn) { log_ = std::move(fn); }

    // Se apontar para `true`, a operação em andamento é interrompida com
    // CancelledError no próximo bloco.
    void setCancelFlag(const std::atomic<bool>* flag) { cancel_ = flag; }

    // Handshake com o firmware e envio da configuração do chip atual.
    FirmwareInfo connect();
    const FirmwareInfo& firmwareInfo() const { return info_; }

    // Seleciona o chip (envia CONFIG se já conectado).
    void selectDevice(const Device& device);
    const Device& device() const { return device_; }

    // Escrita protegida: precede cada ciclo com a sequência SDP, permitindo
    // gravar chips com SDP ativo sem desativá-lo.
    void setProtectedWrite(bool enabled);
    bool protectedWrite() const { return protectedWrite_; }

    // Nome do transporte (porta) em uso.
    std::string portName() const { return transport_->name(); }

    // --- Operações --------------------------------------------------------

    // Lê a faixa e devolve os bytes lidos.
    std::vector<uint8_t> read(Range range = {});

    // Grava a faixa da imagem no chip. Se `options.verify`, devolve as
    // diferências encontradas (vazio = sucesso).
    std::vector<Difference> program(std::span<const uint8_t> image, Range range = {},
                                    ProgramOptions options = {});

    // Compara a faixa do chip com a imagem (CRC por bloco; leitura byte a
    // byte só nos blocos divergentes). Devolve até `maxDifferences` itens.
    std::vector<Difference> verify(std::span<const uint8_t> image, Range range = {},
                                   size_t maxDifferences = 4096);

    // Confere se a faixa está toda em 0xFF.
    BlankCheckResult blankCheck(Range range = {});

    // Preenche a faixa com 0xFF.
    void erase(Range range = {});

    // Habilita/desabilita a proteção de dados por software.
    void setSdp(bool enabled);

    // Sequência AUTO: apagar → verificar vazio → gravar → verificar.
    std::vector<Difference> autoProgram(std::span<const uint8_t> image, Range range = {},
                                        AutoOptions options = {});

private:
    // Converte "length == 0" em chip inteiro e valida os limites.
    Range resolve(Range range) const;
    // Garante que a imagem cobre a faixa.
    void checkImage(std::span<const uint8_t> image, Range range) const;

    void sendConfig();
    void report(const std::string& stage, uint64_t done, uint64_t total);
    void log(LogLevel level, const std::string& message);
    void checkCancel() const;

    std::unique_ptr<Transport> transport_;
    ProtocolClient client_;
    FirmwareInfo info_;
    bool connected_ = false;
    Device device_;
    bool protectedWrite_ = false;

    ProgressFn progress_;
    LogFn log_;
    const std::atomic<bool>* cancel_ = nullptr;
};

// Formata um endereço como "0x1A2B".
std::string formatAddress(uint32_t address);

}  // namespace epn
