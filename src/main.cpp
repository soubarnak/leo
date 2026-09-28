#include "error_log.h"
#include "library_window.h"
#include "single_instance.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
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

    QCoreApplication::setApplicationName(QStringLiteral("leo-writer"));
    QCoreApplication::setApplicationVersion(QStringLiteral(LEO_VERSION));
    QCoreApplication::setOrganizationDomain(QStringLiteral("io.github.soubarnak"));
    if (!commandLineQuery) {
        ErrorLog::install();
    }

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Browse an existing NEO Library in LEO."));
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
    }

    LibraryWindow window;
    SingleInstance instance;
    QString instanceError;
    const SingleInstance::StartResult startResult = instance.acquireOrForward(path, &instanceError);
    if (startResult == SingleInstance::StartResult::AlreadyRunning) {
        return 0;
    }
    if (startResult == SingleInstance::StartResult::Failed) {
        qCritical("Could not start the single LEO instance: %s", qPrintable(instanceError));
        return 1;
    }
    QObject::connect(&instance, &SingleInstance::activationRequested, &window,
                     [&window](const QString &requestedPath) {
                         if (!requestedPath.isEmpty()) {
                             window.openLibrary(requestedPath);
                         }
                         window.showNormal();
                         window.raise();
                         window.activateWindow();
                     });

    if (path.isEmpty()) {
        const QString defaultPath = LibraryWindow::defaultLibraryPath();
        if (QDir(defaultPath).exists()) {
            path = defaultPath;
        } else {
            path = LibraryWindow::selectLibraryDirectory(nullptr, defaultPath);
            if (path.isEmpty()) {
                return 0;
            }
        }
    }

    window.openLibrary(path);
    window.show();
    return application->exec();
}
