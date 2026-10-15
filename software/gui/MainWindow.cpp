// =============================================================================
//  MainWindow.cpp
// =============================================================================
#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <span>

#include "DeviceDialog.h"
#include "HexView.h"
#include "epromnator/Checksum.h"
#include "epromnator/Error.h"
#include "epromnator/FileFormats.h"
#include "epromnator/SerialPort.h"
#include "epromnator/Version.h"

namespace {

// Velocidade fixa do firmware (EPN_DEFAULT_BAUD).
constexpr int kBaud = 115200;

// Máximo de arquivos recentes no menu.
constexpr int kMaxRecentFiles = 8;

// Filtros dos diálogos de arquivo.
const char* const kOpenFilter =
    "Todos os suportados (*.bin *.rom *.img *.hex *.ihx *.s19 *.s28 *.s37 *.srec *.mot);;"
    "Binário (*.bin *.rom *.img);;Intel HEX (*.hex *.ihx);;"
    "Motorola S-Record (*.s19 *.s28 *.s37 *.srec *.mot);;Todos os arquivos (*)";
const char* const kSaveFilter =
    "Binário (*.bin);;Intel HEX (*.hex);;Motorola S-Record (*.s19);;Todos os arquivos (*)";

// Ícone do tema do sistema com alternativa do estilo do Qt (Windows/macOS
// não têm tema de ícones freedesktop).
QIcon themeIcon(const char* name, QStyle::StandardPixmap fallback) {
    return QIcon::fromTheme(QString::fromLatin1(name), QApplication::style()->standardIcon(fallback));
}

// "0x1A2B"
QString hex4(uint32_t value) {
    return QStringLiteral("0x%1").arg(value, 4, 16, QLatin1Char('0')).toUpper().replace("0X", "0x");
}

// "0xA5"
QString hex2(uint8_t value) {
    return QStringLiteral("0x%1").arg(value, 2, 16, QLatin1Char('0')).toUpper().replace("0X", "0x");
}

// Pergunta Sim/Não com botões em português (independe das traduções do Qt
// estarem instaladas no sistema).
bool askYesNo(QWidget* parent, const QString& title, const QString& text, bool defaultYes = false) {
    QMessageBox box(QMessageBox::Question, title, text, QMessageBox::Yes | QMessageBox::No, parent);
    box.button(QMessageBox::Yes)->setText(QObject::tr("Sim"));
    box.button(QMessageBox::No)->setText(QObject::tr("Não"));
    box.setDefaultButton(defaultYes ? QMessageBox::Yes : QMessageBox::No);
    return box.exec() == QMessageBox::Yes;
}

// Converte texto hexadecimal ("1A2B", "0x1A2B", "$1A2B") em número.
std::optional<uint32_t> parseHex(QString text) {
    text = text.trimmed();
    if (text.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) text.remove(0, 2);
    if (text.startsWith(QLatin1Char('$'))) text.remove(0, 1);
    bool ok = false;
    const uint value = text.toUInt(&ok, 16);
    if (!ok || text.isEmpty()) return std::nullopt;
    return value;
}

// Nome amigável de cada operação (log e diálogos).
QString operationName(Operation op) {
    switch (op) {
        case Operation::Connect:    return QObject::tr("Conectar");
        case Operation::Read:       return QObject::tr("Ler");
        case Operation::Program:    return QObject::tr("Gravar");
        case Operation::Verify:     return QObject::tr("Verificar");
        case Operation::Erase:      return QObject::tr("Apagar");
        case Operation::BlankCheck: return QObject::tr("Verificar vazio");
        case Operation::Auto:       return QObject::tr("AUTO");
        case Operation::SdpEnable:  return QObject::tr("Habilitar SDP");
        case Operation::SdpDisable: return QObject::tr("Desabilitar SDP");
    }
    return {};
}

}  // namespace

// =============================================================================
//  Construção
// =============================================================================

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent), device_(epn::deviceDatabase().front()) {
    // Worker em thread própria; destruído quando a thread termina.
    worker_ = new ProgrammerWorker;
    worker_->moveToThread(&workerThread_);
    connect(&workerThread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &ProgrammerWorker::operationFinished, this, &MainWindow::onOperationFinished);
    connect(worker_, &ProgrammerWorker::progress, this, &MainWindow::onProgress);
    connect(worker_, &ProgrammerWorker::logMessage, this,
            [this](int level, const QString& msg) { appendLog(static_cast<epn::LogLevel>(level), msg); });
    connect(worker_, &ProgrammerWorker::disconnected, this, &MainWindow::onWorkerDisconnected);
    workerThread_.start();

    createActions();
    createMenus();
    createToolBar();
    createCentralWidget();
    createStatusBar();
    setAcceptDrops(true);

    loadSettings();
    refreshPorts();

    // Buffer inicial: chip vazio (0xFF).
    hex_->setData(std::vector<uint8_t>(device_.size, 0xFF));
    updateDeviceInfo();
    updateBufferInfo();
    updateTitle();
    updateActions();

    appendLog(epn::LogLevel::Info,
              tr("%1 %2 pronto. Selecione a porta e o chip.").arg(epn::kAppName, epn::kAppVersion));
}

MainWindow::~MainWindow() {
    // Interrompe uma operação em andamento e espera a thread terminar
    // (o Programmer e a porta são destruídos lá).
    worker_->cancel();
    workerThread_.quit();
    workerThread_.wait();
}

// =============================================================================
//  Ações, menus e barras
// =============================================================================

void MainWindow::createActions() {
    // Cria uma ação com texto, ícone, atalho e dica.
    auto make = [this](const QString& text, const QIcon& icon, const QKeySequence& key,
                       const QString& tip) {
        auto* a = new QAction(icon, text, this);
        if (!key.isEmpty()) a->setShortcut(key);
        a->setStatusTip(tip);
        a->setToolTip(key.isEmpty() ? tip : QStringLiteral("%1 (%2)").arg(tip, key.toString()));
        return a;
    };

    // Arquivo
    actOpen_ = make(tr("&Abrir..."), themeIcon("document-open", QStyle::SP_DialogOpenButton),
                    QKeySequence::Open, tr("Abre um arquivo no buffer"));
    actSave_ = make(tr("&Salvar"), themeIcon("document-save", QStyle::SP_DialogSaveButton),
                    QKeySequence::Save, tr("Salva o buffer"));
    actSaveAs_ = make(tr("Salvar &como..."), themeIcon("document-save-as", QStyle::SP_DialogSaveButton),
                      QKeySequence::SaveAs, tr("Salva o buffer em outro arquivo/formato"));
    actQuit_ = make(tr("Sai&r"), themeIcon("application-exit", QStyle::SP_DialogCloseButton),
                    QKeySequence::Quit, tr("Fecha o programa"));
    connect(actOpen_, &QAction::triggered, this, &MainWindow::openFileDialog);
    connect(actSave_, &QAction::triggered, this, &MainWindow::save);
    connect(actSaveAs_, &QAction::triggered, this, &MainWindow::saveAs);
    connect(actQuit_, &QAction::triggered, this, &QWidget::close);

    // Dispositivo
    actChooseDevice_ = make(tr("Selecionar &chip..."), themeIcon("drive-harddisk", QStyle::SP_DriveHDIcon),
                            QKeySequence(Qt::CTRL | Qt::Key_D), tr("Escolhe o modelo da EEPROM"));
    actConnect_ = make(tr("C&onectar"), themeIcon("network-connect", QStyle::SP_DriveNetIcon),
                       QKeySequence(Qt::CTRL | Qt::Key_K), tr("Conecta/desconecta o gravador"));
    connect(actChooseDevice_, &QAction::triggered, this, &MainWindow::chooseDevice);
    connect(actConnect_, &QAction::triggered, this, &MainWindow::toggleConnection);

    // Operações (atalhos F5..F9 como nos gravadores comerciais)
    actRead_ = make(tr("&Ler"), themeIcon("go-down", QStyle::SP_ArrowDown), Qt::Key_F5,
                    tr("Lê o chip para o buffer"));
    actProgram_ = make(tr("&Gravar"), themeIcon("go-up", QStyle::SP_ArrowUp), Qt::Key_F6,
                       tr("Grava o buffer no chip"));
    actVerify_ = make(tr("&Verificar"), themeIcon("edit-find", QStyle::SP_DialogApplyButton), Qt::Key_F7,
                      tr("Compara o chip com o buffer"));
    actErase_ = make(tr("&Apagar"), themeIcon("edit-clear", QStyle::SP_TrashIcon), Qt::Key_F8,
                     tr("Preenche o chip com 0xFF"));
    actBlank_ = make(tr("Verificar &vazio"), themeIcon("document-new", QStyle::SP_FileIcon),
                     QKeySequence(Qt::CTRL | Qt::Key_B), tr("Confere se o chip está todo em 0xFF"));
    actAuto_ = make(tr("A&UTO"), themeIcon("media-playback-start", QStyle::SP_MediaPlay), Qt::Key_F9,
                    tr("Apagar + verificar vazio + gravar + verificar"));
    actSdpOn_ = make(tr("&Habilitar proteção SDP"), {}, {}, tr("Protege o chip contra escritas acidentais"));
    actSdpOff_ = make(tr("&Desabilitar proteção SDP"), {}, {}, tr("Remove a proteção SDP do chip"));
    actCancel_ = make(tr("&Cancelar"), themeIcon("process-stop", QStyle::SP_BrowserStop), Qt::Key_Escape,
                      tr("Cancela a operação em andamento"));
    connect(actRead_, &QAction::triggered, this, [this] { startOperation(Operation::Read); });
    connect(actProgram_, &QAction::triggered, this, [this] { startOperation(Operation::Program); });
    connect(actVerify_, &QAction::triggered, this, [this] { startOperation(Operation::Verify); });
    connect(actErase_, &QAction::triggered, this, [this] { startOperation(Operation::Erase); });
    connect(actBlank_, &QAction::triggered, this, [this] { startOperation(Operation::BlankCheck); });
    connect(actAuto_, &QAction::triggered, this, [this] { startOperation(Operation::Auto); });
    connect(actSdpOn_, &QAction::triggered, this, [this] { startOperation(Operation::SdpEnable); });
    connect(actSdpOff_, &QAction::triggered, this, [this] { startOperation(Operation::SdpDisable); });
    connect(actCancel_, &QAction::triggered, this, [this] {
        worker_->cancel();
        statusBar()->showMessage(tr("Cancelando..."));
    });

    // Buffer
    actGoTo_ = make(tr("&Ir para endereço..."), {}, QKeySequence(Qt::CTRL | Qt::Key_G),
                    tr("Move o cursor para um endereço"));
    actFill_ = make(tr("&Preencher..."), {}, QKeySequence(Qt::CTRL | Qt::Key_L),
                    tr("Preenche uma faixa do buffer com um valor"));
    actClear_ = make(tr("&Limpar (0xFF)"), {}, {}, tr("Preenche o buffer inteiro com 0xFF"));
    actCompare_ = make(tr("&Comparar com arquivo..."), {}, {}, tr("Destaca as diferenças entre o buffer e um arquivo"));
    actChecksum_ = make(tr("C&hecksum"), {}, {}, tr("Mostra soma16 e CRC32 do buffer e da faixa"));
    actClearMarks_ = make(tr("Limpar &marcações"), {}, {}, tr("Remove os destaques de alterações e diferenças"));
    connect(actGoTo_, &QAction::triggered, this, &MainWindow::goToAddress);
    connect(actFill_, &QAction::triggered, this, &MainWindow::fillBuffer);
    connect(actClear_, &QAction::triggered, this, &MainWindow::clearBuffer);
    connect(actCompare_, &QAction::triggered, this, &MainWindow::compareWithFile);
    connect(actChecksum_, &QAction::triggered, this, &MainWindow::showChecksum);
    connect(actClearMarks_, &QAction::triggered, this, [this] { hex_->clearMarks(); });

    // Ajuda
    actAbout_ = make(tr("&Sobre"), themeIcon("help-about", QStyle::SP_MessageBoxInformation), {},
                     tr("Informações sobre o programa"));
    connect(actAbout_, &QAction::triggered, this, [this] {
        QMessageBox::about(
            this, tr("Sobre o %1").arg(epn::kAppName),
            tr("<h3>%1 %2</h3>"
               "<p>Gravador de EEPROMs paralelas 28Cxxx baseado em Arduino Nano.</p>"
               "<p>Formatos: binário, Intel HEX e Motorola S-Record.</p>"
               "<p>Atalhos: F5 Ler · F6 Gravar · F7 Verificar · F8 Apagar · F9 AUTO · "
               "Esc Cancelar</p>")
                .arg(epn::kAppName, epn::kAppVersion));
    });
}

void MainWindow::createMenus() {
    QMenu* file = menuBar()->addMenu(tr("&Arquivo"));
    file->addAction(actOpen_);
    recentMenu_ = file->addMenu(tr("Abrir &recente"));
    file->addAction(actSave_);
    file->addAction(actSaveAs_);
    file->addSeparator();
    file->addAction(actQuit_);

    QMenu* device = menuBar()->addMenu(tr("&Dispositivo"));
    device->addAction(actChooseDevice_);
    device->addAction(actConnect_);

    QMenu* ops = menuBar()->addMenu(tr("&Operações"));
    ops->addAction(actRead_);
    ops->addAction(actProgram_);
    ops->addAction(actVerify_);
    ops->addAction(actErase_);
    ops->addAction(actBlank_);
    ops->addAction(actAuto_);
    ops->addSeparator();
    ops->addAction(actSdpOn_);
    ops->addAction(actSdpOff_);
    ops->addSeparator();
    ops->addAction(actCancel_);

    QMenu* buffer = menuBar()->addMenu(tr("&Buffer"));
    buffer->addAction(actGoTo_);
    buffer->addAction(actFill_);
    buffer->addAction(actClear_);
    buffer->addSeparator();
    buffer->addAction(actCompare_);
    buffer->addAction(actChecksum_);
    buffer->addAction(actClearMarks_);

    QMenu* help = menuBar()->addMenu(tr("Aj&uda"));
    help->addAction(actAbout_);
}

void MainWindow::createToolBar() {
    QToolBar* bar = addToolBar(tr("Principal"));
    bar->setObjectName(QStringLiteral("mainToolBar"));
    bar->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    bar->setMovable(false);

    bar->addAction(actOpen_);
    bar->addAction(actSave_);
    bar->addSeparator();
    bar->addAction(actChooseDevice_);
    bar->addSeparator();
    bar->addAction(actRead_);
    bar->addAction(actProgram_);
    bar->addAction(actVerify_);
    bar->addAction(actErase_);
    bar->addAction(actBlank_);
    bar->addAction(actAuto_);
    bar->addSeparator();

    // Seleção de porta: combo + atualizar + conectar.
    auto* portBox = new QWidget(bar);
    auto* portLayout = new QHBoxLayout(portBox);
    portLayout->setContentsMargins(6, 0, 6, 0);
    portLayout->addWidget(new QLabel(tr("Porta:"), portBox));
    portCombo_ = new QComboBox(portBox);
    portCombo_->setMinimumContentsLength(18);
    portCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    portLayout->addWidget(portCombo_);
    auto* refresh = new QToolButton(portBox);
    refresh->setIcon(themeIcon("view-refresh", QStyle::SP_BrowserReload));
    refresh->setToolTip(tr("Atualiza a lista de portas"));
    connect(refresh, &QToolButton::clicked, this, &MainWindow::refreshPorts);
    portLayout->addWidget(refresh);
    connectButton_ = new QPushButton(tr("Conectar"), portBox);
    connect(connectButton_, &QPushButton::clicked, this, &MainWindow::toggleConnection);
    portLayout->addWidget(connectButton_);
    bar->addWidget(portBox);
}

void MainWindow::createCentralWidget() {
    // --- Editor hexadecimal --------------------------------------------------
    hex_ = new HexView(this);
    connect(hex_, &HexView::edited, this, [this] {
        setModified(true);
        updateBufferInfo();
    });
    connect(hex_, &HexView::cursorMoved, this, [this](uint32_t address) {
        const auto& data = hex_->data();
        QString text = tr("Cursor: %1 = %2").arg(hex4(address), hex2(address < data.size() ? data[address] : 0));
        if (hex_->hasSelection()) {
            text += tr("\nSeleção: %1..%2 (%3 bytes)")
                        .arg(hex4(hex_->selectionStart()), hex4(hex_->selectionEnd()))
                        .arg(hex_->selectionEnd() - hex_->selectionStart() + 1);
        }
        cursorInfo_->setText(text);
    });

    // --- Painel lateral ------------------------------------------------------
    auto* side = new QWidget(this);
    auto* sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(0, 0, 0, 0);

    // Dispositivo
    auto* deviceBox = new QGroupBox(tr("Dispositivo"), side);
    auto* deviceLayout = new QVBoxLayout(deviceBox);
    deviceName_ = new QLabel(deviceBox);
    QFont bold = deviceName_->font();
    bold.setBold(true);
    bold.setPointSizeF(bold.pointSizeF() * 1.2);
    deviceName_->setFont(bold);
    deviceDetails_ = new QLabel(deviceBox);  // linhas curtas: sem quebra automática
    deviceNotes_ = new QLabel(deviceBox);
    deviceNotes_->setWordWrap(true);
    auto* changeDevice = new QPushButton(tr("Alterar chip..."), deviceBox);
    connect(changeDevice, &QPushButton::clicked, this, &MainWindow::chooseDevice);
    deviceLayout->addWidget(deviceName_);
    deviceLayout->addWidget(deviceDetails_);
    deviceLayout->addWidget(deviceNotes_);
    deviceLayout->addWidget(changeDevice);

    // Buffer
    auto* bufferBox = new QGroupBox(tr("Buffer"), side);
    auto* bufferLayout = new QFormLayout(bufferBox);
    bufferFile_ = new QLabel(bufferBox);
    bufferFile_->setWordWrap(true);
    bufferSum_ = new QLabel(bufferBox);
    bufferCrc_ = new QLabel(bufferBox);
    bufferSum_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    bufferCrc_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    cursorInfo_ = new QLabel(bufferBox);
    bufferLayout->addRow(tr("Arquivo:"), bufferFile_);
    bufferLayout->addRow(tr("Soma16:"), bufferSum_);
    bufferLayout->addRow(tr("CRC32:"), bufferCrc_);
    bufferLayout->addRow(cursorInfo_);

    // Opções
    auto* optionsBox = new QGroupBox(tr("Opções"), side);
    auto* optionsLayout = new QVBoxLayout(optionsBox);
    optVerify_ = new QCheckBox(tr("Verificar após gravar"), optionsBox);
    optSkipFF_ = new QCheckBox(tr("Pular páginas vazias (0xFF)"), optionsBox);
    optSkipFF_->setToolTip(tr("Não grava páginas todas em 0xFF. Use com o chip apagado."));
    optAutoErase_ = new QCheckBox(tr("AUTO: apagar antes de gravar"), optionsBox);
    optProtected_ = new QCheckBox(tr("Escrita protegida (SDP)"), optionsBox);
    optProtected_->setToolTip(
        tr("Envia a sequência de desbloqueio SDP antes de cada página: grava chips\n"
           "protegidos sem remover a proteção."));
    optVerify_->setChecked(true);
    optAutoErase_->setChecked(true);
    connect(optProtected_, &QCheckBox::toggled, this, &MainWindow::sendConfiguration);
    optionsLayout->addWidget(optVerify_);
    optionsLayout->addWidget(optSkipFF_);
    optionsLayout->addWidget(optAutoErase_);
    optionsLayout->addWidget(optProtected_);

    // Faixa de endereços
    auto* rangeBox = new QGroupBox(tr("Faixa de endereços"), side);
    auto* rangeLayout = new QFormLayout(rangeBox);
    wholeChip_ = new QCheckBox(tr("Chip inteiro"), rangeBox);
    wholeChip_->setChecked(true);
    rangeStart_ = new QLineEdit(QStringLiteral("0000"), rangeBox);
    rangeEnd_ = new QLineEdit(rangeBox);
    rangeStart_->setEnabled(false);
    rangeEnd_->setEnabled(false);
    connect(wholeChip_, &QCheckBox::toggled, this, [this](bool whole) {
        rangeStart_->setEnabled(!whole);
        rangeEnd_->setEnabled(!whole);
    });
    auto* useSelection = new QPushButton(tr("Usar seleção do editor"), rangeBox);
    connect(useSelection, &QPushButton::clicked, this, [this] {
        wholeChip_->setChecked(false);
        rangeStart_->setText(hex4(hex_->selectionStart()).mid(2));
        rangeEnd_->setText(hex4(hex_->selectionEnd()).mid(2));
    });
    rangeLayout->addRow(wholeChip_);
    rangeLayout->addRow(tr("Início (hex):"), rangeStart_);
    rangeLayout->addRow(tr("Fim (hex):"), rangeEnd_);
    rangeLayout->addRow(useSelection);

    sideLayout->addWidget(deviceBox);
    sideLayout->addWidget(bufferBox);
    sideLayout->addWidget(optionsBox);
    sideLayout->addWidget(rangeBox);
    sideLayout->addStretch(1);

    // O painel fica dentro de uma área rolável: em janelas baixas ele rola
    // em vez de ser comprimido (o que sobreporia os rótulos).
    auto* sideScroll = new QScrollArea(this);
    sideScroll->setWidget(side);
    sideScroll->setWidgetResizable(true);
    sideScroll->setFrameShape(QFrame::NoFrame);
    sideScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sideScroll->setMinimumWidth(side->sizeHint().width() +
                                sideScroll->verticalScrollBar()->sizeHint().width());

    // --- Log -----------------------------------------------------------------
    log_ = new QPlainTextEdit(this);
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(5000);
    log_->setPlaceholderText(tr("Log de operações"));

    // --- Divisórias ----------------------------------------------------------
    topSplitter_ = new QSplitter(Qt::Horizontal, this);
    topSplitter_->addWidget(hex_);
    topSplitter_->addWidget(sideScroll);
    topSplitter_->setStretchFactor(0, 1);
    topSplitter_->setStretchFactor(1, 0);

    mainSplitter_ = new QSplitter(Qt::Vertical, this);
    mainSplitter_->addWidget(topSplitter_);
    mainSplitter_->addWidget(log_);
    mainSplitter_->setStretchFactor(0, 1);
    mainSplitter_->setStretchFactor(1, 0);
    mainSplitter_->setSizes({520, 140});

    setCentralWidget(mainSplitter_);
}

void MainWindow::createStatusBar() {
    connectionLabel_ = new QLabel(this);
    progressBar_ = new QProgressBar(this);
    progressBar_->setMaximumWidth(260);
    progressBar_->setRange(0, 100);
    progressBar_->setFormat(QStringLiteral("%p%"));
    progressBar_->hide();
    cancelButton_ = new QPushButton(tr("Cancelar"), this);
    cancelButton_->hide();
    connect(cancelButton_, &QPushButton::clicked, actCancel_, &QAction::trigger);

    statusBar()->addPermanentWidget(progressBar_);
    statusBar()->addPermanentWidget(cancelButton_);
    statusBar()->addPermanentWidget(connectionLabel_);
}

// =============================================================================
//  Configurações persistentes
// =============================================================================

void MainWindow::loadSettings() {
    QSettings s;
    restoreGeometry(s.value(QStringLiteral("window/geometry")).toByteArray());
    restoreState(s.value(QStringLiteral("window/state")).toByteArray());
    if (s.contains(QStringLiteral("window/splitter"))) {
        mainSplitter_->restoreState(s.value(QStringLiteral("window/splitter")).toByteArray());
    }
    if (!s.contains(QStringLiteral("window/geometry"))) resize(1100, 720);

    if (const epn::Device* d = epn::findDevice(s.value(QStringLiteral("device")).toString().toStdString())) {
        device_ = *d;
    }
    optVerify_->setChecked(s.value(QStringLiteral("options/verify"), true).toBool());
    optSkipFF_->setChecked(s.value(QStringLiteral("options/skipFF"), false).toBool());
    optAutoErase_->setChecked(s.value(QStringLiteral("options/autoErase"), true).toBool());
    optProtected_->setChecked(s.value(QStringLiteral("options/protected"), false).toBool());
    recentFiles_ = s.value(QStringLiteral("recentFiles")).toStringList();
    updateRecentMenu();
}

void MainWindow::saveSettings() {
    QSettings s;
    s.setValue(QStringLiteral("window/geometry"), saveGeometry());
    s.setValue(QStringLiteral("window/state"), saveState());
    s.setValue(QStringLiteral("window/splitter"), mainSplitter_->saveState());
    s.setValue(QStringLiteral("device"), QString::fromStdString(device_.name));
    s.setValue(QStringLiteral("port"), portCombo_->currentData().toString());
    s.setValue(QStringLiteral("options/verify"), optVerify_->isChecked());
    s.setValue(QStringLiteral("options/skipFF"), optSkipFF_->isChecked());
    s.setValue(QStringLiteral("options/autoErase"), optAutoErase_->isChecked());
    s.setValue(QStringLiteral("options/protected"), optProtected_->isChecked());
    s.setValue(QStringLiteral("recentFiles"), recentFiles_);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (busy_ && !askYesNo(this, tr("Operação em andamento"),
                           tr("Uma operação está em andamento. Cancelar e sair?"))) {
        event->ignore();
        return;
    }
    if (!maybeSave()) {
        event->ignore();
        return;
    }
    saveSettings();
    event->accept();
}

// =============================================================================
//  Arquivo
// =============================================================================

void MainWindow::openFileDialog() {
    if (!maybeSave()) return;
    const QString dir = filePath_.isEmpty() ? QString() : QFileInfo(filePath_).absolutePath();
    const QString path = QFileDialog::getOpenFileName(this, tr("Abrir arquivo"), dir, tr(kOpenFilter));
    if (!path.isEmpty()) openFile(path);
}

bool MainWindow::openFile(const QString& path) {
    const std::filesystem::path fsPath = path.toStdWString();
    try {
        const epn::FileFormat format = epn::detectFormat(fsPath);
        std::vector<uint8_t> image(device_.size, 0xFF);
        epn::LoadResult r = epn::loadFile(fsPath, format, image, 0);
        int64_t offset = 0;

        // Arquivo HEX/S-Record com dados acima da capacidade do chip (ex.:
        // ROM de 6502 montada em 0x8000): oferece um deslocamento.
        if (r.bytesIgnored > 0 && format != epn::FileFormat::Binary) {
            std::vector<uint8_t> probe(1u << 24, 0xFF);  // 16 MB: cobre S2/HEX 04 típicos
            const epn::LoadResult all = epn::loadFile(fsPath, format, probe, 0);
            const uint32_t base = all.lowestAddress - all.lowestAddress % device_.size;
            if (base > 0 && all.highestAddress - base < device_.size) {
                const bool accepted = askYesNo(
                    this, tr("Endereços fora do chip"),
                    tr("O arquivo contém dados em %1..%2, além da capacidade do %3.\n\n"
                       "Carregar com deslocamento de -%4 (o endereço %4 do arquivo vira 0x0000 "
                       "no chip)?")
                        .arg(hex4(all.lowestAddress), hex4(all.highestAddress),
                             QString::fromStdString(device_.name), hex4(base)),
                    true);
                if (accepted) {
                    offset = -static_cast<int64_t>(base);
                    image.assign(device_.size, 0xFF);
                    r = epn::loadFile(fsPath, format, image, offset);
                }
            }
        }

        hex_->setData(std::move(image));
        filePath_ = path;
        setModified(false);
        addRecentFile(path);
        updateBufferInfo();

        appendLog(epn::LogLevel::Info,
                  tr("Arquivo aberto: %1 (%2), %3 bytes em %4..%5%6.")
                      .arg(QFileInfo(path).fileName(), QString::fromStdString(epn::formatName(format)))
                      .arg(r.bytesLoaded)
                      .arg(hex4(r.lowestAddress), hex4(r.highestAddress))
                      .arg(offset ? tr(", deslocamento %1").arg(offset) : QString()));
        if (r.bytesIgnored > 0) {
            appendLog(epn::LogLevel::Warning,
                      tr("%1 bytes do arquivo ficaram fora da capacidade do chip e foram ignorados.")
                          .arg(r.bytesIgnored));
        }
        return true;
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Erro ao abrir"), QString::fromUtf8(e.what()));
        return false;
    }
}

bool MainWindow::save() {
    // Só regrava direto arquivos nos formatos que sabemos escrever.
    if (filePath_.isEmpty()) return saveAs();
    return saveTo(filePath_);
}

bool MainWindow::saveAs() {
    QString selectedFilter;
    QString path = QFileDialog::getSaveFileName(this, tr("Salvar buffer"),
                                                filePath_.isEmpty() ? QStringLiteral("eeprom.bin") : filePath_,
                                                tr(kSaveFilter), &selectedFilter);
    if (path.isEmpty()) return false;
    // Sem extensão: usa a do filtro escolhido.
    if (QFileInfo(path).suffix().isEmpty()) {
        if (selectedFilter.contains(QLatin1String("HEX"))) path += QStringLiteral(".hex");
        else if (selectedFilter.contains(QLatin1String("S-Record"))) path += QStringLiteral(".s19");
        else path += QStringLiteral(".bin");
    }
    return saveTo(path);
}

bool MainWindow::saveTo(const QString& path) {
    try {
        const std::filesystem::path fsPath = path.toStdWString();
        const epn::FileFormat format = epn::detectFormat(fsPath);
        epn::saveFile(fsPath, format, hex_->data());
        filePath_ = path;
        setModified(false);
        addRecentFile(path);
        updateBufferInfo();
        appendLog(epn::LogLevel::Info, tr("Buffer salvo em %1 (%2).")
                                           .arg(QFileInfo(path).fileName(),
                                                QString::fromStdString(epn::formatName(format))));
        return true;
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Erro ao salvar"), QString::fromUtf8(e.what()));
        return false;
    }
}

bool MainWindow::maybeSave() {
    if (!modified_) return true;
    QMessageBox box(QMessageBox::Warning, tr("Buffer modificado"),
                    tr("O buffer tem alterações não salvas. Deseja salvá-las?"),
                    QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, this);
    box.button(QMessageBox::Save)->setText(tr("Salvar"));
    box.button(QMessageBox::Discard)->setText(tr("Descartar"));
    box.button(QMessageBox::Cancel)->setText(tr("Cancelar"));
    box.setDefaultButton(QMessageBox::Save);
    const int answer = box.exec();
    if (answer == QMessageBox::Save) return save();
    return answer == QMessageBox::Discard;
}

void MainWindow::addRecentFile(const QString& path) {
    recentFiles_.removeAll(path);
    recentFiles_.prepend(path);
    while (recentFiles_.size() > kMaxRecentFiles) recentFiles_.removeLast();
    updateRecentMenu();
}

void MainWindow::updateRecentMenu() {
    recentMenu_->clear();
    for (const QString& path : recentFiles_) {
        QAction* a = recentMenu_->addAction(QFileInfo(path).fileName());
        a->setToolTip(path);
        connect(a, &QAction::triggered, this, [this, path] {
            if (maybeSave()) openFile(path);
        });
    }
    recentMenu_->setEnabled(!recentFiles_.isEmpty());
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event) {
    const auto urls = event->mimeData()->urls();
    if (!urls.isEmpty() && urls.first().isLocalFile() && maybeSave()) {
        openFile(urls.first().toLocalFile());
    }
}

// =============================================================================
//  Dispositivo e conexão
// =============================================================================

void MainWindow::chooseDevice() {
    DeviceDialog dialog(QString::fromStdString(device_.name), this);
    if (dialog.exec() != QDialog::Accepted) return;
    if (const epn::Device* d = epn::findDevice(dialog.selectedDevice().toStdString())) applyDevice(*d);
}

void MainWindow::applyDevice(const epn::Device& device) {
    // Ajusta o buffer ao tamanho do novo chip, avisando se dados forem cortados.
    std::vector<uint8_t> data = hex_->data();
    if (data.size() > device.size &&
        std::any_of(data.begin() + device.size, data.end(), [](uint8_t b) { return b != 0xFF; })) {
        appendLog(epn::LogLevel::Warning,
                  tr("O buffer foi reduzido para %1 bytes: dados acima de %2 foram descartados.")
                      .arg(device.size)
                      .arg(hex4(device.size - 1)));
    }
    data.resize(device.size, 0xFF);
    hex_->setData(std::move(data));

    device_ = device;
    appendLog(epn::LogLevel::Info, tr("Chip selecionado: %1.").arg(QString::fromStdString(device.name)));
    sendConfiguration();
    updateDeviceInfo();
    updateBufferInfo();
    updateTitle();
}

void MainWindow::sendConfiguration() {
    if (connected_) worker_->requestConfigure(device_, optProtected_->isChecked());
}

void MainWindow::refreshPorts() {
    // Mantém a seleção atual (ou a salva nas configurações) se ainda existir.
    QString wanted = portCombo_->currentData().toString();
    if (portCombo_->count() == 0) wanted = QSettings().value(QStringLiteral("port")).toString();

    portCombo_->clear();
    for (const epn::PortInfo& p : epn::SerialPort::listPorts()) {
        const QString path = QString::fromStdString(p.path);
        const QString label = p.description.empty()
                                  ? path
                                  : QStringLiteral("%1 — %2").arg(path, QString::fromStdString(p.description));
        portCombo_->addItem(label, path);
    }
    // Item especial: emulador (dados vazios = sem porta).
    portCombo_->addItem(tr("Emulador (sem hardware)"), QString());

    const int index = portCombo_->findData(wanted);
    portCombo_->setCurrentIndex(index >= 0 ? index : 0);
}

void MainWindow::toggleConnection() {
    if (busy_) return;
    if (connected_) {
        worker_->requestDisconnect();
        return;
    }
    connectedPort_ = portCombo_->currentData().toString();
    setBusy(true);
    statusBar()->showMessage(tr("Conectando..."));
    worker_->requestConnect(connectedPort_, kBaud, device_, optProtected_->isChecked());
}

void MainWindow::onWorkerDisconnected() {
    if (!connected_) return;
    connected_ = false;
    appendLog(epn::LogLevel::Info, tr("Gravador desconectado."));
    updateActions();
}

// =============================================================================
//  Operações
// =============================================================================

std::optional<epn::Range> MainWindow::currentRange() {
    if (wholeChip_->isChecked()) return epn::Range{};
    const auto start = parseHex(rangeStart_->text());
    const auto end = parseHex(rangeEnd_->text());
    if (!start || !end || *start > *end || *end >= device_.size) {
        QMessageBox::warning(this, tr("Faixa inválida"),
                             tr("Informe início e fim em hexadecimal, com início ≤ fim ≤ %1.")
                                 .arg(hex4(device_.size - 1)));
        return std::nullopt;
    }
    return epn::Range{*start, *end - *start + 1};
}

void MainWindow::startOperation(Operation operation) {
    if (busy_) return;

    const auto range = currentRange();
    if (!range) return;

    // Não conectado: conecta primeiro e executa a operação em seguida.
    if (!connected_) {
        pendingOperation_ = operation;
        toggleConnection();
        return;
    }

    OperationRequest request;
    request.operation = operation;
    request.range = *range;

    // Descrição da faixa para os diálogos de confirmação.
    const uint32_t first = range->start;
    const uint32_t count = range->length ? range->length : device_.size - range->start;
    const QString where = tr("%1 bytes (%2..%3) do %4 em %5")
                              .arg(count)
                              .arg(hex4(first), hex4(first + count - 1),
                                   QString::fromStdString(device_.name),
                                   connectedPort_.isEmpty() ? tr("emulador") : connectedPort_);

    // Operações destrutivas pedem confirmação, como nos gravadores comerciais.
    auto confirm = [this](const QString& title, const QString& text) {
        return askYesNo(this, title, text);
    };

    switch (operation) {
        case Operation::Program:
        case Operation::Auto: {
            const auto& data = hex_->data();
            const std::span<const uint8_t> part(data.data() + first, count);
            const QString steps = operation == Operation::Auto
                                      ? tr("\nSequência: %1verificar vazio, gravar%2.")
                                            .arg(optAutoErase_->isChecked() ? tr("apagar, ") : QString(),
                                                 optVerify_->isChecked() ? tr(", verificar") : QString())
                                      : QString();
            if (!confirm(operationName(operation),
                         tr("Gravar %1?\n\nChecksum (soma16) da faixa: 0x%2%3")
                             .arg(where)
                             .arg(epn::sum16(part), 4, 16, QLatin1Char('0'))
                             .arg(steps))) {
                return;
            }
            request.image = data;
            request.programOptions.verify = optVerify_->isChecked();
            request.programOptions.skipBlankPages = optSkipFF_->isChecked();
            request.autoOptions.erase = optAutoErase_->isChecked();
            request.autoOptions.blankCheck = optAutoErase_->isChecked();
            request.autoOptions.verify = optVerify_->isChecked();
            request.autoOptions.skipBlankPages = optAutoErase_->isChecked() || optSkipFF_->isChecked();
            break;
        }
        case Operation::Erase:
            if (!confirm(tr("Apagar"), tr("Apagar (preencher com 0xFF) %1?").arg(where))) return;
            break;
        case Operation::Verify:
            request.image = hex_->data();
            break;
        default:
            break;
    }

    hex_->setDifferences({});
    setBusy(true);
    appendLog(epn::LogLevel::Info, tr("— %1 —").arg(operationName(operation)));
    worker_->requestOperation(std::move(request));
}

void MainWindow::onProgress(const QString& stage, int percent) {
    progressBar_->setFormat(QStringLiteral("%1 %p%").arg(stage));
    progressBar_->setValue(percent);
}

void MainWindow::onOperationFinished(const OperationResult& r) {
    setBusy(false);

    // --- Conexão --------------------------------------------------------------
    if (r.operation == Operation::Connect) {
        connected_ = r.success;
        updateActions();
        if (!r.success) {
            pendingOperation_.reset();
            statusBar()->showMessage(tr("Falha na conexão"), 5000);
            appendLog(epn::LogLevel::Error, r.message);
            QMessageBox::critical(this, tr("Falha na conexão"), r.message);
            return;
        }
        statusBar()->showMessage(tr("Conectado"), 3000);
        // Executa a operação que motivou a conexão automática.
        if (pendingOperation_) {
            const Operation next = *pendingOperation_;
            pendingOperation_.reset();
            startOperation(next);
        }
        return;
    }

    const QString name = operationName(r.operation);

    // --- Cancelamento e erros -------------------------------------------------
    if (r.cancelled) {
        appendLog(epn::LogLevel::Warning, tr("%1 cancelado.").arg(name));
        statusBar()->showMessage(tr("%1 cancelado").arg(name), 5000);
        return;
    }
    if (!r.message.isEmpty()) {
        appendLog(epn::LogLevel::Error, r.message);
        statusBar()->showMessage(tr("%1 falhou").arg(name), 5000);
        QMessageBox::critical(this, tr("%1 falhou").arg(name), r.message);
        return;
    }

    // --- Resultados por operação ----------------------------------------------
    switch (r.operation) {
        case Operation::Read: {
            // Copia os dados lidos para a posição correspondente do buffer.
            std::vector<uint8_t> data = hex_->data();
            std::copy(r.data.begin(), r.data.end(), data.begin() + r.range.start);
            hex_->setData(std::move(data));
            setModified(true);  // conteúdo novo ainda não salvo em disco
            updateBufferInfo();
            appendLog(epn::LogLevel::Info, tr("Soma16 lida: 0x%1, CRC32: 0x%2")
                                               .arg(epn::sum16(r.data), 4, 16, QLatin1Char('0'))
                                               .arg(epn::crc32(r.data), 8, 16, QLatin1Char('0'))
                                               .toUpper().replace(QLatin1String("0X"), QLatin1String("0x")));
            break;
        }
        case Operation::Program:
        case Operation::Auto:
        case Operation::Verify:
            if (!r.diffs.empty()) {
                showDifferences(r.diffs);
                QMessageBox::warning(this, tr("%1: verificação falhou").arg(name),
                                     tr("%1 byte(s) diferentes. A primeira diferença está em %2 "
                                        "(destacada em vermelho no editor).")
                                         .arg(r.diffs.size())
                                         .arg(hex4(r.diffs.front().address)));
            } else if (r.operation != Operation::Verify) {
                hex_->clearMarks();  // chip e buffer agora coincidem
            }
            break;
        case Operation::BlankCheck:
            if (!r.blank.blank) {
                hex_->goTo(r.blank.firstUsedAddress);
                QMessageBox::information(this, name,
                                         tr("O chip NÃO está vazio: %1 contém %2.")
                                             .arg(hex4(r.blank.firstUsedAddress), hex2(r.blank.value)));
            }
            break;
        default:
            break;
    }
    statusBar()->showMessage(r.success ? tr("%1 concluído com sucesso").arg(name)
                                       : tr("%1 concluído com problemas").arg(name),
                             8000);
}

void MainWindow::showDifferences(const std::vector<epn::Difference>& diffs) {
    std::vector<uint32_t> addresses;
    addresses.reserve(diffs.size());
    for (const auto& d : diffs) addresses.push_back(d.address);
    hex_->setDifferences(addresses);
    hex_->goTo(diffs.front().address);

    // Lista as primeiras diferenças no log.
    const size_t shown = std::min<size_t>(diffs.size(), 10);
    for (size_t i = 0; i < shown; ++i) {
        appendLog(epn::LogLevel::Error, tr("  %1: esperado %2, lido %3")
                                            .arg(hex4(diffs[i].address), hex2(diffs[i].expected),
                                                 hex2(diffs[i].actual)));
    }
    if (diffs.size() > shown) {
        appendLog(epn::LogLevel::Error, tr("  ... e mais %1 diferença(s).").arg(diffs.size() - shown));
    }
}

// =============================================================================
//  Buffer
// =============================================================================

void MainWindow::goToAddress() {
    bool ok = false;
    const QString text = QInputDialog::getText(this, tr("Ir para endereço"), tr("Endereço (hex):"),
                                               QLineEdit::Normal, hex4(hex_->cursorAddress()).mid(2), &ok);
    if (!ok) return;
    const auto address = parseHex(text);
    if (!address || *address >= hex_->data().size()) {
        QMessageBox::warning(this, tr("Endereço inválido"), tr("Endereço fora do buffer."));
        return;
    }
    hex_->goTo(*address);
    hex_->setFocus();
}

void MainWindow::fillBuffer() {
    // Diálogo simples: início, fim e valor (hex). Começa com a seleção atual.
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Preencher buffer"));
    auto* form = new QFormLayout(&dialog);
    auto* start = new QLineEdit(hex4(hex_->selectionStart()).mid(2), &dialog);
    auto* end = new QLineEdit(hex4(hex_->hasSelection() ? hex_->selectionEnd()
                                                        : static_cast<uint32_t>(hex_->data().size() - 1))
                                  .mid(2),
                              &dialog);
    auto* value = new QLineEdit(QStringLiteral("FF"), &dialog);
    form->addRow(tr("Início (hex):"), start);
    form->addRow(tr("Fim (hex):"), end);
    form->addRow(tr("Valor (hex):"), value);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("Cancelar"));
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;

    const auto s = parseHex(start->text());
    const auto e = parseHex(end->text());
    const auto v = parseHex(value->text());
    if (!s || !e || !v || *s > *e || *e >= hex_->data().size() || *v > 0xFF) {
        QMessageBox::warning(this, tr("Valores inválidos"), tr("Confira início, fim e valor."));
        return;
    }
    hex_->fill(*s, *e - *s + 1, static_cast<uint8_t>(*v));
}

void MainWindow::clearBuffer() {
    hex_->fill(0, static_cast<uint32_t>(hex_->data().size()), 0xFF);
}

void MainWindow::compareWithFile() {
    const QString path = QFileDialog::getOpenFileName(this, tr("Comparar com arquivo"), QString(),
                                                      tr(kOpenFilter));
    if (path.isEmpty()) return;
    try {
        const std::filesystem::path fsPath = path.toStdWString();
        std::vector<uint8_t> other(device_.size, 0xFF);
        epn::loadFile(fsPath, epn::detectFormat(fsPath), other, 0);

        // Comparação local (sem o gravador). expected = arquivo, actual = buffer.
        std::vector<epn::Difference> diffs;
        const auto& data = hex_->data();
        for (uint32_t i = 0; i < data.size(); ++i) {
            if (data[i] != other[i]) diffs.push_back({i, other[i], data[i]});
        }
        if (diffs.empty()) {
            appendLog(epn::LogLevel::Info, tr("Buffer idêntico a %1.").arg(QFileInfo(path).fileName()));
            hex_->setDifferences({});
        } else {
            appendLog(epn::LogLevel::Warning, tr("%1 diferença(s) em relação a %2:")
                                                  .arg(diffs.size())
                                                  .arg(QFileInfo(path).fileName()));
            showDifferences(diffs);
        }
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Erro ao comparar"), QString::fromUtf8(e.what()));
    }
}

void MainWindow::showChecksum() {
    const auto& data = hex_->data();
    QString text = tr("Buffer inteiro (%1 bytes):\n  Soma16 = 0x%2\n  CRC32 = 0x%3")
                       .arg(data.size())
                       .arg(epn::sum16(data), 4, 16, QLatin1Char('0'))
                       .arg(epn::crc32(data), 8, 16, QLatin1Char('0'));
    if (hex_->hasSelection()) {
        const std::span<const uint8_t> sel(data.data() + hex_->selectionStart(),
                                           hex_->selectionEnd() - hex_->selectionStart() + 1);
        text += tr("\n\nSeleção %1..%2 (%3 bytes):\n  Soma16 = 0x%4\n  CRC32 = 0x%5")
                    .arg(hex4(hex_->selectionStart()), hex4(hex_->selectionEnd()))
                    .arg(sel.size())
                    .arg(epn::sum16(sel), 4, 16, QLatin1Char('0'))
                    .arg(epn::crc32(sel), 8, 16, QLatin1Char('0'));
    }
    QMessageBox::information(this, tr("Checksum"), text.toUpper().replace(QLatin1String("0X"), QLatin1String("0x")));
}

// =============================================================================
//  Estado da interface
// =============================================================================

void MainWindow::setBusy(bool busy) {
    busy_ = busy;
    progressBar_->setVisible(busy);
    cancelButton_->setVisible(busy);
    progressBar_->setValue(0);
    progressBar_->setFormat(QStringLiteral("%p%"));
    if (busy) QApplication::setOverrideCursor(Qt::BusyCursor);
    else QApplication::restoreOverrideCursor();
    updateActions();
}

void MainWindow::updateActions() {
    // Durante uma operação, só "Cancelar" fica ativo.
    const bool idle = !busy_;
    for (QAction* a : {actRead_, actProgram_, actVerify_, actErase_, actBlank_, actAuto_, actOpen_,
                       actChooseDevice_, actFill_, actClear_, actCompare_, recentMenu_->menuAction()}) {
        a->setEnabled(idle);
    }
    actSdpOn_->setEnabled(idle && device_.supportsSdp);
    actSdpOff_->setEnabled(idle && device_.supportsSdp);
    optProtected_->setEnabled(device_.supportsSdp);
    actCancel_->setEnabled(busy_);
    actConnect_->setEnabled(idle);
    portCombo_->setEnabled(idle && !connected_);
    connectButton_->setEnabled(idle);
    hex_->setEnabled(idle);

    actConnect_->setText(connected_ ? tr("Des&conectar") : tr("C&onectar"));
    connectButton_->setText(connected_ ? tr("Desconectar") : tr("Conectar"));
    connectionLabel_->setText(connected_ ? tr("● Conectado: %1")
                                               .arg(connectedPort_.isEmpty() ? tr("emulador") : connectedPort_)
                                         : tr("○ Desconectado"));
}

void MainWindow::updateTitle() {
    const QString file = filePath_.isEmpty() ? tr("sem título") : QFileInfo(filePath_).fileName();
    setWindowTitle(QStringLiteral("%1%2 — %3 — %4")
                       .arg(file, modified_ ? QStringLiteral(" *") : QString(),
                            QString::fromStdString(device_.name), QString::fromLatin1(epn::kAppName)));
}

void MainWindow::setModified(bool modified) {
    modified_ = modified;
    updateTitle();
}

void MainWindow::updateBufferInfo() {
    const auto& data = hex_->data();
    bufferFile_->setText(filePath_.isEmpty() ? tr("(sem arquivo)") : QFileInfo(filePath_).fileName());
    bufferFile_->setToolTip(filePath_);
    bufferSum_->setText(QStringLiteral("0x%1").arg(epn::sum16(data), 4, 16, QLatin1Char('0')).toUpper().replace("0X", "0x"));
    bufferCrc_->setText(QStringLiteral("0x%1").arg(epn::crc32(data), 8, 16, QLatin1Char('0')).toUpper().replace("0X", "0x"));
}

void MainWindow::updateDeviceInfo() {
    deviceName_->setText(QString::fromStdString(device_.name));
    deviceDetails_->setText(tr("%1\n%2 KB (%3 bytes)\nPágina: %4 · SDP: %5")
                                .arg(QString::fromStdString(device_.manufacturer))
                                .arg(device_.size / 1024)
                                .arg(device_.size)
                                .arg(device_.pageSize > 1 ? QString::number(device_.pageSize) : tr("—"))
                                .arg(device_.supportsSdp ? tr("sim") : tr("não")));
    deviceNotes_->setText(QString::fromStdString(device_.notes));
    rangeEnd_->setPlaceholderText(hex4(device_.size - 1).mid(2));
    updateActions();
}

void MainWindow::appendLog(epn::LogLevel level, const QString& message) {
    // Cada linha com horário; avisos e erros coloridos.
    QString color;
    QString prefix;
    switch (level) {
        case epn::LogLevel::Info:    break;
        case epn::LogLevel::Warning: color = QStringLiteral("#c77c00"); prefix = tr("AVISO: "); break;
        case epn::LogLevel::Error:   color = QStringLiteral("#d03030"); prefix = tr("ERRO: "); break;
    }
    const QString time = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    QString html = QStringLiteral("<span style='opacity:0.6'>%1</span> %2")
                       .arg(time, (prefix + message).toHtmlEscaped());
    if (!color.isEmpty()) {
        html = QStringLiteral("<span style='opacity:0.6'>%1</span> <span style='color:%2'>%3</span>")
                   .arg(time, color, (prefix + message).toHtmlEscaped());
    }
    log_->appendHtml(html);
}
