#pragma once

#include "mail_draft.h"

#include <QDateTime>
#include <QString>

struct SnapshotResult {
    bool ok = false;
    QString error;
    QString title;
    QString stamp;          // UTC, yyyyMMdd'T'HHmmss'Z'
    QString pdfPath;
    QString textPath;       // plain-text export that the fingerprint covers
    QString fingerprintPath; // sha256sum-format sidecar: digests of textPath and pdfPath
    QString fingerprint;    // lowercase hex SHA-256 of the text export bytes
};

class EmailSnapshot final {
public:
    // Writes <title>-<stamp>.pdf, .txt and .sha256 into snapshotDir. Nothing is written
    // when the PDF fails. A partial set is rolled back when a later step fails.
    // The snapshot is complete before any mail draft is attempted.
    static SnapshotResult create(const QString &libraryPath, const QString &bookId,
                                 const QString &snapshotDir, const QDateTime &now);
    static DraftMessage draftMessage(const SnapshotResult &snapshot);
};
