#include "error_log.h"

#include "app_paths.h"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QTextStream>

#include <cstdio>
#include <cstdlib>

namespace {

QMutex logMutex;
thread_local bool writingLog = false;

QString levelName(QtMsgType type)
{
    switch (type) {
    case QtWarningMsg:
        return QStringLiteral("warning");
    case QtCriticalMsg:
        return QStringLiteral("critical");
    case QtFatalMsg:
        return QStringLiteral("fatal");
    case QtDebugMsg:
        return QStringLiteral("debug");
    case QtInfoMsg:
        return QStringLiteral("info");
    }
    return QStringLiteral("unknown");
}

void writeToStderr(const QByteArray &encoded)
{
    std::fwrite(encoded.constData(), 1, static_cast<size_t>(encoded.size()), stderr);
    std::fputc('\n', stderr);
}

void writeMessage(QtMsgType type, const QMessageLogContext &, const QString &message)
{
    if (type == QtDebugMsg || type == QtInfoMsg) {
        return;
    }
    const QByteArray encoded = message.toUtf8();
    if (writingLog) {
        writeToStderr(encoded);
        return;
    }

    writingLog = true;
    {
        QMutexLocker locker(&logMutex);
        const QString logPath = AppPaths::logFilePath();
        const QString parentPath = QFileInfo(logPath).absolutePath();
        if (QDir().mkpath(parentPath)) {
            QFile file(logPath);
            if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
                QTextStream stream(&file);
                stream << QDateTime::currentDateTime().toString(Qt::ISODateWithMs) << ' '
                       << levelName(type) << ": " << message << Qt::endl;
                file.flush();
            } else {
                writeToStderr(encoded);
            }
        } else {
            writeToStderr(encoded);
        }
    }
    writingLog = false;

    if (type == QtFatalMsg) {
        std::abort();
    }
}

}

void ErrorLog::install()
{
    qInstallMessageHandler(writeMessage);
}
