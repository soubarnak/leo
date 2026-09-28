#include "app_paths.h"

#include <QDir>
#include <QFileInfo>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

namespace {

QString privateDirectory(const char *environmentVariable, const QString &defaultBase)
{
    const QString configuredBase = qEnvironmentVariable(environmentVariable);
    const QString base = QDir::isAbsolutePath(configuredBase) ? configuredBase : defaultBase;
    return QDir(QDir::cleanPath(base)).filePath(QStringLiteral("leo-writer"));
}

QString userId()
{
#ifdef Q_OS_UNIX
    return QString::number(::getuid());
#else
    return QString::fromLocal8Bit(qgetenv("USER"));
#endif
}

}

QString AppPaths::configDirectory()
{
    return privateDirectory("XDG_CONFIG_HOME", QDir::home().filePath(QStringLiteral(".config")));
}

QString AppPaths::dataDirectory()
{
    return privateDirectory("XDG_DATA_HOME",
                            QDir::home().filePath(QStringLiteral(".local/share")));
}

QString AppPaths::stateDirectory()
{
    return privateDirectory("XDG_STATE_HOME",
                            QDir::home().filePath(QStringLiteral(".local/state")));
}

QString AppPaths::cacheDirectory()
{
    return privateDirectory("XDG_CACHE_HOME", QDir::home().filePath(QStringLiteral(".cache")));
}

QString AppPaths::runtimeDirectory()
{
    const QString configuredRuntime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    const QFileInfo runtimeInfo(configuredRuntime);
    if (QDir::isAbsolutePath(configuredRuntime) && runtimeInfo.isDir() && runtimeInfo.isWritable()) {
        return QDir::cleanPath(configuredRuntime);
    }
    return QDir::tempPath();
}

QString AppPaths::logFilePath()
{
    return QDir(stateDirectory()).filePath(QStringLiteral("leo-writer.log"));
}

QString AppPaths::instanceLockFilePath()
{
    return QDir(runtimeDirectory()).filePath(QStringLiteral("leo-writer-%1.lock").arg(userId()));
}

QString AppPaths::instanceServerName()
{
    return QDir(runtimeDirectory()).filePath(QStringLiteral("leo-writer-%1.sock").arg(userId()));
}
