// =============================================================================
//  MainWindow.h — janela principal do EPROMnator
// =============================================================================
//
//  Layout no padrão dos gravadores comerciais (XGPro/TL866, Batronix):
//
//   ┌ Menus: Arquivo | Dispositivo | Operações | Buffer | Ajuda ───────────┐
//   ├ Barra: Abrir Salvar | Chip | Ler Gravar Verificar Apagar Vazio AUTO │
//   │        | Porta [▼] ⟳ [Conectar]                                      │
//   ├──────────────────────────────┬────────────────────────────────────────┤
//   │ Editor hexadecimal (buffer)  │ Painel: dispositivo, buffer, opções,   │
//   │                              │ faixa de endereços                     │
//   ├──────────────────────────────┴────────────────────────────────────────┤
//   │ Log                                                                  │
//   ├ Status: mensagem | conexão | progresso | Cancelar ───────────────────┤
//
//  Toda comunicação com o gravador passa pelo ProgrammerWorker, que roda em
//  outra thread; a janela apenas envia pedidos e reage aos sinais.
// =============================================================================
#pragma once

#include <QMainWindow>
#include <QThread>

#include <optional>

#include "ProgrammerWorker.h"
#include "epromnator/Device.h"

class HexView;
class QAction;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QMenu;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSplitter;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // Abre um arquivo no buffer (também usado para arquivos passados na
    // linha de comando e arrastados para a janela).
    bool openFile(const QString& path);

protected:
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    // --- Montagem da interface ---------------------------------------------
    void createActions();
    void createMenus();
    void createToolBar();
    void createCentralWidget();
    void createStatusBar();
    void loadSettings();
    void saveSettings();

    // --- Arquivo -----------------------------------------------------------
    void openFileDialog();
    bool save();
    bool saveAs();
    bool saveTo(const QString& path);
    bool maybeSave();
    void addRecentFile(const QString& path);
    void updateRecentMenu();

    // --- Dispositivo e conexão ---------------------------------------------
    void chooseDevice();
    void applyDevice(const epn::Device& device);
    void refreshPorts();
    void toggleConnection();
    void sendConfiguration();

    // --- Operações ---------------------------------------------------------
    void startOperation(Operation operation);
    void onOperationFinished(const OperationResult& result);
    void onProgress(const QString& stage, int percent);
    void onWorkerDisconnected();

    // --- Buffer ------------------------------------------------------------
    void goToAddress();
    void fillBuffer();
    void clearBuffer();
    void compareWithFile();
    void showChecksum();

    // --- Estado e utilidades -----------------------------------------------
    std::optional<epn::Range> currentRange();  // nullopt = faixa inválida
    void setBusy(bool busy);
    void updateActions();
    void updateTitle();
    void updateBufferInfo();
    void updateDeviceInfo();
    void appendLog(epn::LogLevel level, const QString& message);
    void setModified(bool modified);
    void showDifferences(const std::vector<epn::Difference>& diffs);

    // --- Thread de comunicação --------------------------------------------
    QThread workerThread_;
    ProgrammerWorker* worker_ = nullptr;

    // --- Estado ------------------------------------------------------------
    epn::Device device_;
    QString filePath_;            // arquivo do buffer ("" = sem nome)
    bool modified_ = false;       // buffer difere do arquivo em disco
    bool connected_ = false;
    bool busy_ = false;
    QString connectedPort_;
    std::optional<Operation> pendingOperation_;  // executar após conectar
    QStringList recentFiles_;

    // --- Widgets -----------------------------------------------------------
    HexView* hex_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
    QSplitter* mainSplitter_ = nullptr;
    QSplitter* topSplitter_ = nullptr;

    QComboBox* portCombo_ = nullptr;
    QPushButton* connectButton_ = nullptr;

    QLabel* deviceName_ = nullptr;
    QLabel* deviceDetails_ = nullptr;
    QLabel* deviceNotes_ = nullptr;
    QLabel* bufferFile_ = nullptr;
    QLabel* bufferSum_ = nullptr;
    QLabel* bufferCrc_ = nullptr;
    QLabel* cursorInfo_ = nullptr;

    QCheckBox* optVerify_ = nullptr;
    QCheckBox* optSkipFF_ = nullptr;
    QCheckBox* optProtected_ = nullptr;
    QCheckBox* optAutoErase_ = nullptr;
    QCheckBox* wholeChip_ = nullptr;
    QLineEdit* rangeStart_ = nullptr;
    QLineEdit* rangeEnd_ = nullptr;

    QLabel* connectionLabel_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
    QPushButton* cancelButton_ = nullptr;

    // --- Ações -------------------------------------------------------------
    QAction* actOpen_ = nullptr;
    QAction* actSave_ = nullptr;
    QAction* actSaveAs_ = nullptr;
    QAction* actQuit_ = nullptr;
    QAction* actChooseDevice_ = nullptr;
    QAction* actIdentify_ = nullptr;
    QAction* actConnect_ = nullptr;
    QAction* actRead_ = nullptr;
    QAction* actProgram_ = nullptr;
    QAction* actVerify_ = nullptr;
    QAction* actErase_ = nullptr;
    QAction* actBlank_ = nullptr;
    QAction* actAuto_ = nullptr;
    QAction* actSdpOn_ = nullptr;
    QAction* actSdpOff_ = nullptr;
    QAction* actCancel_ = nullptr;
    QAction* actGoTo_ = nullptr;
    QAction* actFill_ = nullptr;
    QAction* actClear_ = nullptr;
    QAction* actCompare_ = nullptr;
    QAction* actChecksum_ = nullptr;
    QAction* actClearMarks_ = nullptr;
    QAction* actAbout_ = nullptr;
    QMenu* recentMenu_ = nullptr;
};
