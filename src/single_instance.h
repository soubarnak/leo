#pragma once

#include <QLockFile>
#include <QLocalServer>
#include <QObject>
#include <QString>

class QLocalSocket;

class SingleInstance final : public QObject {
    Q_OBJECT

public:
    enum class StartResult { Primary, AlreadyRunning, Failed };

    explicit SingleInstance(QObject *parent = nullptr);
    StartResult acquireOrForward(const QString &libraryPath, QString *errorMessage = nullptr);

signals:
    void activationRequested(const QString &libraryPath);

private:
    bool forwardToPrimary(const QString &libraryPath);
    void acceptConnections();
    void receiveRequest(QLocalSocket *socket);

    QLockFile lock_;
    QString serverName_;
    QLocalServer server_;
};
