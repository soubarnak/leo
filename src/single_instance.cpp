#include "single_instance.h"

#include "app_paths.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QThread>

SingleInstance::SingleInstance(QObject *parent)
    : QObject(parent)
    , lock_(AppPaths::instanceLockFilePath())
    , serverName_(AppPaths::instanceServerName())
{
    connect(&server_, &QLocalServer::newConnection, this, &SingleInstance::acceptConnections);
}

SingleInstance::StartResult SingleInstance::acquireOrForward(const QString &libraryPath,
                                                              QString *errorMessage)
{
    if (lock_.tryLock(0)) {
        QLocalServer::removeServer(serverName_);
        server_.setSocketOptions(QLocalServer::UserAccessOption);
        if (!server_.listen(serverName_)) {
            qWarning("Single-instance activation socket unavailable: %s",
                     qPrintable(server_.errorString()));
        }
        return StartResult::Primary;
    }

    if (lock_.error() != QLockFile::LockFailedError) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Could not acquire the single-instance lock.");
        }
        return StartResult::Failed;
    }

    if (forwardToPrimary(libraryPath)) {
        return StartResult::AlreadyRunning;
    }

    qWarning("Another LEO process is running; this launch was suppressed because activation failed.");
    return StartResult::AlreadyRunning;
}

bool SingleInstance::forwardToPrimary(const QString &libraryPath)
{
    QJsonObject request;
    request.insert(QStringLiteral("library"), libraryPath);
    QByteArray message = QJsonDocument(request).toJson(QJsonDocument::Compact);
    message.append('\n');

    for (int attempt = 0; attempt < 20; ++attempt) {
        QLocalSocket socket;
        socket.connectToServer(serverName_, QIODevice::ReadWrite);
        if (socket.waitForConnected(50)) {
            if (socket.write(message) != message.size() || !socket.waitForBytesWritten(1000) ||
                !socket.waitForReadyRead(1000)) {
                return false;
            }
            return socket.readLine().trimmed() == QByteArrayLiteral("ok");
        }
        if (attempt < 19) {
            QThread::msleep(25);
        }
    }
    return false;
}

void SingleInstance::acceptConnections()
{
    while (QLocalSocket *socket = server_.nextPendingConnection()) {
        connect(socket, &QLocalSocket::readyRead, this,
                [this, socket] { receiveRequest(socket); });
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        if (socket->bytesAvailable() > 0) {
            receiveRequest(socket);
        }
    }
}

void SingleInstance::receiveRequest(QLocalSocket *socket)
{
    if (socket->property("leoRequestHandled").toBool()) {
        return;
    }

    QByteArray buffered = socket->property("leoRequestBuffer").toByteArray();
    buffered.append(socket->readAll());
    const qsizetype newline = buffered.indexOf('\n');
    if (newline < 0) {
        socket->setProperty("leoRequestBuffer", buffered);
        return;
    }

    socket->setProperty("leoRequestHandled", true);
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(buffered.left(newline), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        qWarning("Rejected malformed single-instance request: %s", qPrintable(parseError.errorString()));
        socket->write(QByteArrayLiteral("error\n"));
        socket->flush();
        socket->disconnectFromServer();
        return;
    }

    const QString libraryPath = document.object().value(QStringLiteral("library")).toString();
    emit activationRequested(libraryPath);
    socket->write(QByteArrayLiteral("ok\n"));
    socket->flush();
    socket->disconnectFromServer();
}
