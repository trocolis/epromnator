// =============================================================================
//  main.cpp — ponto de entrada da interface gráfica
// =============================================================================
//
//  Uso:  epromnator-gui [arquivo]
//  Um arquivo passado na linha de comando é aberto no buffer.
// =============================================================================
#include <QApplication>
#include <QLibraryInfo>
#include <QLocale>
#include <QTranslator>

#include "MainWindow.h"
#include "epromnator/Version.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    // Usados pelo QSettings para localizar as configurações salvas.
    QApplication::setOrganizationName(QStringLiteral("EPROMnator"));
    QApplication::setApplicationName(QStringLiteral("EPROMnator"));
    QApplication::setApplicationVersion(QString::fromLatin1(epn::kAppVersion));

    // Traduções dos textos internos do Qt (diálogos de arquivo, menus de
    // contexto...), quando o pacote de traduções está instalado no sistema.
    QTranslator qtTranslator;
    if (qtTranslator.load(QLocale(), QStringLiteral("qtbase"), QStringLiteral("_"),
                          QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
        QApplication::installTranslator(&qtTranslator);
    }

    MainWindow window;
    window.show();

    const QStringList args = QApplication::arguments();
    if (args.size() > 1) window.openFile(args.at(1));

    return QApplication::exec();
}
