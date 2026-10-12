#pragma once

#include <QDate>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

struct LibrarySnapshotInfo {
    QDate date;
    QString path;
};

struct LibrarySnapshotResult {
    bool created = false;
    // True when nothing was created because the Library is unchanged or today already has an archive.
    bool skipped = false;
    QString snapshotPath;
    // Non-empty when the local snapshot (or its bookkeeping) failed.
    QString error;
    // Non-empty when the optional backup drive could not take a verified copy. Never fails the local snapshot.
    QString driveError;
    // Non-fatal: for example old snapshots were not pruned because the store looks inconsistent.
    QString warning;
    // Finalized day folders that did not verify. They are kept, not counted, and not pruned.
    QStringList invalidSnapshots;
};

// Daily Library snapshots and restore. Everything lives outside the synchronized Library:
// <data>/daily-snapshots/<sha256(library)>/<yyyy-MM-dd>/{library/, manifest.json}.
class LibrarySnapshots final {
public:
    static constexpr int retainedSnapshots = 14;

    // Creates at most one verified snapshot per UTC day, only when the Library differs from the
    // last snapshot. Callers must have quiesced writes first. Never writes under libraryRoot.
    // afterCopy is a test seam run after the copy finished and before it is verified.
    static LibrarySnapshotResult maybeSnapshotDaily(
        const QString &libraryRoot, const QDateTime &nowUtc,
        const std::function<void()> &afterCopy = {});

    // The local half of maybeSnapshotDaily: no drive access, so it is safe to run on the GUI thread.
    static LibrarySnapshotResult maybeSnapshotLocal(
        const QString &libraryRoot, const QDateTime &nowUtc,
        const std::function<void()> &afterCopy = {});
    static bool driveConfigured(const QString &libraryRoot);
    // Mirrors and verifies the latest valid local archive on the configured drive. Returns the
    // error text, empty on success or when no drive is configured. Blocking and thread-safe: it
    // touches only the drive folder and read-only local archive files, never any widget, so the
    // window runs it on a worker thread.
    static QString mirrorLatestToDrive(const QString &libraryRoot);

    // Valid snapshots, newest first. Invalid day folders are appended to invalid when given.
    static QList<LibrarySnapshotInfo> listSnapshots(const QString &libraryRoot,
                                                    QStringList *invalid = nullptr);

    // Verified restore into a separate Recovered library. First takes a fresh verified safety
    // snapshot of the current Library. The Library itself is never written.
    static bool restoreToRecoveredLibrary(const QString &libraryRoot,
                                          const QString &snapshotPath,
                                          const QDateTime &nowUtc, QString *recoveredPath,
                                          QString *error);

    // Verifies one day folder: manifest hash and every file under library/.
    static bool verifySnapshot(const QString &snapshotPath, QString *error);

    static QString snapshotBucket(const QString &libraryRoot);
    static QString libraryHash(const QString &libraryRoot);
};
