// =============================================================================
//  ProgrammerWorker.cpp
// =============================================================================
#include "ProgrammerWorker.h"

#include "epromnator/Emulator.h"
#include "epromnator/Error.h"
#include "epromnator/SerialPort.h"

ProgrammerWorker::ProgrammerWorker(QObject* parent) : QObject(parent) {}

ProgrammerWorker::~ProgrammerWorker() = default;

// =============================================================================
//  Agendamento (chamado da thread da GUI)
// =============================================================================
//
//  invokeMethod com um lambda e `this` como contexto executa o lambda na
//  thread à qual este objeto pertence (a do worker).

void ProgrammerWorker::requestConnect(const QString& port, int baud, const epn::Device& device,
                                      bool protectedWrite) {
    QMetaObject::invokeMethod(this, [=, this] { doConnect(port, baud, device, protectedWrite); });
}

void ProgrammerWorker::requestDisconnect() {
    QMetaObject::invokeMethod(this, [this] {
        programmer_.reset();  // fecha a porta
        emit disconnected();
    });
}

void ProgrammerWorker::requestConfigure(const epn::Device& device, bool protectedWrite) {
    QMetaObject::invokeMethod(this, [=, this] {
        if (!programmer_) return;
        try {
            programmer_->selectDevice(device);
            programmer_->setProtectedWrite(protectedWrite);
        } catch (const std::exception& e) {
            emit logMessage(static_cast<int>(epn::LogLevel::Error), QString::fromUtf8(e.what()));
        }
    });
}

void ProgrammerWorker::requestOperation(OperationRequest request) {
    // O cancelamento vale a partir de agora (zerado aqui, na thread da GUI,
    // para que um clique em "Cancelar" logo após o pedido não se perca).
    cancel_ = false;
    QMetaObject::invokeMethod(this, [this, request = std::move(request)] { doOperation(request); });
}

// =============================================================================
//  Execução (thread do worker)
// =============================================================================

void ProgrammerWorker::doConnect(const QString& port, int baud, const epn::Device& device,
                                 bool protectedWrite) {
    OperationResult result;
    result.operation = Operation::Connect;
    programmer_.reset();

    try {
        // Porta vazia = emulador (demonstração e testes sem a placa).
        std::unique_ptr<epn::Transport> transport;
        if (port.isEmpty()) {
            transport = std::make_unique<epn::EmulatorTransport>(device.size);
        } else {
            transport = std::make_unique<epn::SerialPort>(port.toStdString(), static_cast<unsigned>(baud));
        }

        auto programmer = std::make_unique<epn::Programmer>(std::move(transport));

        // Os callbacks rodam nesta thread; os sinais chegam à GUI enfileirados.
        programmer->setLogCallback([this](epn::LogLevel level, const std::string& message) {
            emit logMessage(static_cast<int>(level), QString::fromStdString(message));
        });
        programmer->setProgressCallback([this](const std::string& stage, uint64_t done, uint64_t total) {
            const int percent = total ? static_cast<int>(done * 100 / total) : 100;
            if (percent != lastPercent_) {
                lastPercent_ = percent;
                emit progress(QString::fromStdString(stage), percent);
            }
        });
        programmer->setCancelFlag(&cancel_);
        programmer->selectDevice(device);
        programmer->setProtectedWrite(protectedWrite);

        const epn::FirmwareInfo info = programmer->connect();
        result.firmware = QString::fromStdString(info.description());
        result.success = true;
        programmer_ = std::move(programmer);
    } catch (const std::exception& e) {
        result.message = QString::fromUtf8(e.what());
    }
    emit operationFinished(result);
}

void ProgrammerWorker::doOperation(const OperationRequest& request) {
    OperationResult result;
    result.operation = request.operation;
    result.range = request.range;
    lastPercent_ = -1;

    if (!programmer_) {
        result.message = tr("Gravador não conectado.");
        emit operationFinished(result);
        return;
    }

    try {
        epn::Programmer& p = *programmer_;
        switch (request.operation) {
            case Operation::Connect:
                break;  // tratado em doConnect
            case Operation::Read:
                result.data = p.read(request.range);
                break;
            case Operation::Program:
                result.diffs = p.program(request.image, request.range, request.programOptions);
                break;
            case Operation::Verify:
                result.diffs = p.verify(request.image, request.range);
                break;
            case Operation::Erase:
                p.erase(request.range);
                break;
            case Operation::BlankCheck:
                result.blank = p.blankCheck(request.range);
                break;
            case Operation::Auto:
                result.diffs = p.autoProgram(request.image, request.range, request.autoOptions);
                break;
            case Operation::SdpEnable:
                p.setSdp(true);
                break;
            case Operation::SdpDisable:
                p.setSdp(false);
                break;
        }
        // Sucesso = sem diferenças na verificação e chip vazio no blank check.
        result.success = result.diffs.empty() && result.blank.blank;
    } catch (const epn::CancelledError& e) {
        result.cancelled = true;
        result.message = QString::fromUtf8(e.what());
    } catch (const epn::TransportError& e) {
        // Falha de comunicação: provavelmente a placa foi desconectada.
        result.message = QString::fromUtf8(e.what());
        programmer_.reset();
        emit disconnected();
    } catch (const std::exception& e) {
        result.message = QString::fromUtf8(e.what());
    }
    emit operationFinished(result);
}
