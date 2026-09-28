#pragma once

#include <QByteArray>
#include <QString>

#include <functional>

enum class PersistenceCheckpoint {
    BeforeTargetStaging,
    AfterTargetStaging,
    BeforeTargetFlush,
    AfterTargetFlush,
    BeforeTargetRename,
    AfterTargetRename,
    BeforeTargetDirectoryFlush,
    AfterTargetDirectoryFlush,
    BeforeJournalCompletion,
    AfterJournalCompletion
};

enum class PersistenceIoOperation {
    ReadTarget,
    WriteTargetStage,
    FlushTargetStage,
    RenameTarget,
    FlushTargetDirectory,
    ReadJournal,
    WriteJournalStage,
    FlushJournalStage,
    RenameJournal,
    FlushJournalDirectory
};

using PersistenceCheckpointHook =
    std::function<bool(PersistenceCheckpoint, QString *)>;
// Return true to make the selected target I/O operation fail.
using PersistenceIoFailureHook =
    std::function<bool(PersistenceIoOperation, QString *)>;

struct PersistenceResult {
    bool ok = false;
    bool conflict = false;
    bool recovered = false;
    QString error;
    QByteArray savedHash;
    QString conflictDraftPath;
    QString recoveredLibraryPath;
};

class LibraryPersistence final {
public:
    static QByteArray hash(const QByteArray &bytes);
    static bool readLibraryFile(const QString &libraryPath,
                                const QString &relativePath,
                                QByteArray *bytes,
                                QString *error);
    static PersistenceResult recoverPendingSaves(
        const QString &libraryPath,
        const PersistenceIoFailureHook &ioFailure = {});
    static PersistenceResult updateConflictDraft(
        const QString &libraryPath,
        const QString &relativePath,
        const QString &draftPath,
        const QString &recoveredLibraryPath,
        const QByteArray &draftBytes);
    static PersistenceResult saveFile(const QString &libraryPath,
                                      const QString &relativePath,
                                      const QByteArray &expectedHash,
                                      const QByteArray &newBytes,
                                      const PersistenceCheckpointHook &checkpoint = {},
                                      const PersistenceIoFailureHook &ioFailure = {});
};
