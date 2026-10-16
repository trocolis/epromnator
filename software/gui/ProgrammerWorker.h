// =============================================================================
//  ProgrammerWorker.h — executa o Programmer em uma thread separada
// =============================================================================
//
//  As operações do gravador levam segundos; se rodassem na thread da GUI, a
//  janela congelaria. Este objeto vive em uma QThread própria:
//
//    GUI ──request*()──▶ (fila de eventos da thread do worker) ──▶ Programmer
//    GUI ◀──sinais (progress, logMessage, operationFinished)──────┘
//
//  Os métodos request*() podem ser chamados da thread da GUI: eles apenas
//  agendam o trabalho com QMetaObject::invokeMethod. O Programmer e a porta
//  serial são criados, usados e destruídos SOMENTE na thread do worker.
// =============================================================================
#pragma once

#include <QObject>
#include <QString>

#include <atomic>
#include <memory>
#include <vector>

#include "epromnator/Device.h"
#include "epromnator/Programmer.h"

// Operações disponíveis na interface.
enum class Operation {
    Connect,
    Read,
    Program,
    Verify,
    Erase,
    BlankCheck,
    Auto,
    SdpEnable,
    SdpDisable,
    Identify,
};

// Parâmetros de uma operação.
struct OperationRequest {
    Operation operation = Operation::Read;
    std::vector<uint8_t> image;  // cópia do buffer (Program/Verify/Auto)
    epn::Range range;
    epn::ProgramOptions programOptions;
    epn::AutoOptions autoOptions;
};

// Resultado entregue à GUI ao final de uma operação.
struct OperationResult {
    Operation operation = Operation::Read;
    bool success = false;
    bool cancelled = false;
    QString message;                       // erro (se !success)
    std::vector<uint8_t> data;             // dados lidos (Read)
    epn::Range range;                      // faixa efetivamente usada
    std::vector<epn::Difference> diffs;    // Verify/Program/Auto
    epn::BlankCheckResult blank;           // BlankCheck
    epn::Identification identification;    // Identify
    QString firmware;                      // descrição do firmware (Connect)
};
Q_DECLARE_METATYPE(OperationResult)

class ProgrammerWorker : public QObject {
    Q_OBJECT

public:
    explicit ProgrammerWorker(QObject* parent = nullptr);
    ~ProgrammerWorker() override;

    // Abre a porta (ou o emulador, se `port` for vazio) e faz o handshake.
    // Resultado em operationFinished(Operation::Connect).
    void requestConnect(const QString& port, int baud, const epn::Device& device,
                        bool protectedWrite);

    // Fecha a porta.
    void requestDisconnect();

    // Atualiza o chip e a opção de escrita protegida (envia CONFIG).
    void requestConfigure(const epn::Device& device, bool protectedWrite);

    // Executa uma operação no gravador.
    void requestOperation(OperationRequest request);

    // Cancela a operação em andamento (seguro chamar de qualquer thread).
    void cancel() { cancel_ = true; }

signals:
    void progress(const QString& stage, int percent);
    void logMessage(int level, const QString& message);  // level = epn::LogLevel
    void operationFinished(const OperationResult& result);
    void disconnected();

private:
    void doConnect(const QString& port, int baud, const epn::Device& device, bool protectedWrite);
    void doOperation(const OperationRequest& request);

    std::unique_ptr<epn::Programmer> programmer_;
    std::atomic<bool> cancel_{false};
    int lastPercent_ = -1;  // evita inundar a GUI com o mesmo percentual
};
