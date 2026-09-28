#pragma once

#include <QByteArray>
#include <QString>

struct PersistenceResult {
    bool ok = false;
    bool conflict = false;
    QString error;
    QByteArray savedHash;
};

class LibraryPersistence final {
public:
    static QByteArray hash(const QByteArray &bytes);
    static bool readLibraryFile(const QString &libraryPath,
                                const QString &relativePath,
                                QByteArray *bytes,
                                QString *error);
    static PersistenceResult recoverPendingSaves(const QString &libraryPath);
    static PersistenceResult saveFile(const QString &libraryPath,
                                      const QString &relativePath,
                                      const QByteArray &expectedHash,
                                      const QByteArray &newBytes);
};
