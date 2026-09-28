#include "library_window.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileDialog>
#include <QGuiApplication>

#include <memory>

namespace {

bool isCommandLineQuery(int argc, char *argv[])
{
    for (int index = 1; index < argc; ++index) {
        const QByteArray argument(argv[index]);
        if (argument == "-h" || argument == "--help" || argument == "-?" ||
            argument == "-v" || argument == "--version") {
            return true;
        }
    }
    return false;
}

}

int main(int argc, char *argv[])
{
    const bool commandLineQuery = isCommandLineQuery(argc, argv);
    std::unique_ptr<QCoreApplication> application;
    if (commandLineQuery) {
        application = std::make_unique<QCoreApplication>(argc, argv);
    } else {
        application = std::make_unique<QApplication>(argc, argv);
        QGuiApplication::setDesktopFileName(QStringLiteral("io.github.soubarnak.LeoWriter"));
    }

    QCoreApplication::setApplicationName(QStringLiteral("LEO"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("io.github.soubarnak"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Browse an existing NEO Library."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("library"),
                                 QStringLiteral("Path to an existing NEO Library."),
                                 QStringLiteral("[library]"));
    parser.process(*application);
    if (commandLineQuery) {
        return 0;
    }

    QString path;
    const QStringList arguments = parser.positionalArguments();
    if (!arguments.isEmpty()) {
        path = arguments.first();
    } else {
        const QString defaultPath = LibraryWindow::defaultLibraryPath();
        if (QDir(defaultPath).exists()) {
            path = defaultPath;
        } else {
            path = QFileDialog::getExistingDirectory(
                nullptr, QStringLiteral("Open existing NEO Library"), defaultPath,
                QFileDialog::ShowDirsOnly);
            if (path.isEmpty()) {
                return 0;
            }
        }
    }

    LibraryWindow window;
    window.openLibrary(path);
    window.show();
    return application->exec();
}
