#include "library_persistence.h"

#include "app_paths.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QByteArrayView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QUuid>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using Manifest = QMap<QString, QByteArray>;

struct Journal {
    QString id;
    QString libraryPath;
    QString relativePath;
    QByteArray oldBytes;
    QByteArray newBytes;
    QString state;
};

QString systemError(const QString &operation, const QString &path)
{
    return QStringLiteral("%1 %2: %3")
        .arg(operation, path, QString::fromLocal8Bit(std::strerror(errno)));
}

QString cleanAbsolutePath(const QString &path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

QString canonicalDirectory(const QString &path)
{
    QString candidate = cleanAbsolutePath(path);
    QStringList missingComponents;
    QFileInfo info(candidate);
    while (!info.exists() && !info.isSymLink()) {
        const QString component = info.fileName();
        const QString parent = info.absolutePath();
        if (component.isEmpty() || parent == candidate) {
            break;
        }
        missingComponents.prepend(component);
        candidate = parent;
        info.setFile(candidate);
    }

    QString resolved = info.canonicalFilePath();
    if (resolved.isEmpty()) {
        resolved = cleanAbsolutePath(candidate);
    }
    for (const QString &component : missingComponents) {
        resolved = QDir(resolved).filePath(component);
    }
    return QDir::cleanPath(resolved);
}

bool isWithin(const QString &parent, const QString &child)
{
    const QString normalizedParent = QDir::cleanPath(parent);
    const QString normalizedChild = QDir::cleanPath(child);
    if (normalizedParent == normalizedChild) {
        return true;
    }
    return normalizedChild.startsWith(
        normalizedParent.endsWith(QLatin1Char('/'))
            ? normalizedParent
            : normalizedParent + QLatin1Char('/'));
}

bool ensurePrivateDirectory(const QString &path, QString *error)
{
    if (!QDir().mkpath(path)) {
        *error = QStringLiteral("Cannot create private save directory %1.").arg(path);
        return false;
    }
    if (::chmod(QFile::encodeName(path).constData(), 0700) != 0) {
        *error = systemError(QStringLiteral("Cannot secure directory"), path);
        return false;
    }
    const QFileInfo info(path);
    if (!info.isDir() || info.isSymLink() || !info.isWritable()) {
        *error = QStringLiteral("Private save directory is not a writable directory: %1")
                     .arg(path);
        return false;
    }
    return true;
}

bool ensureOutsideLibrary(const QString &path, const QString &libraryRoot, QString *error)
{
    const QString canonicalPath = canonicalDirectory(path);
    if (isWithin(libraryRoot, canonicalPath)) {
        *error = QStringLiteral(
            "Private save data resolves inside the Library and cannot be used safely.");
        return false;
    }
    return true;
}

QString journalDirectory()
{
    return QDir(AppPaths::stateDirectory()).filePath(QStringLiteral("save-journal"));
}

QString snapshotDirectory()
{
    return QDir(AppPaths::dataDirectory()).filePath(QStringLiteral("safety-snapshots"));
}

bool syncDirectory(const QString &path, QString *error)
{
    const QByteArray nativePath = QFile::encodeName(path);
    const int descriptor = ::open(nativePath.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) {
        *error = systemError(QStringLiteral("Cannot open directory for flush"), path);
        return false;
    }
    const int syncResult = ::fsync(descriptor);
    const int syncError = errno;
    const int closeResult = ::close(descriptor);
    if (syncResult != 0) {
        errno = syncError;
        *error = systemError(QStringLiteral("Cannot flush directory"), path);
        return false;
    }
    if (closeResult != 0) {
        *error = systemError(QStringLiteral("Cannot close directory"), path);
        return false;
    }
    return true;
}

QString checkpointName(PersistenceCheckpoint checkpoint)
{
    switch (checkpoint) {
    case PersistenceCheckpoint::BeforeTargetStaging:
        return QStringLiteral("before target staging");
    case PersistenceCheckpoint::AfterTargetStaging:
        return QStringLiteral("after target staging");
    case PersistenceCheckpoint::BeforeTargetFlush:
        return QStringLiteral("before target flush");
    case PersistenceCheckpoint::AfterTargetFlush:
        return QStringLiteral("after target flush");
    case PersistenceCheckpoint::BeforeTargetRename:
        return QStringLiteral("before target rename");
    case PersistenceCheckpoint::AfterTargetRename:
        return QStringLiteral("after target rename");
    case PersistenceCheckpoint::BeforeTargetDirectoryFlush:
        return QStringLiteral("before target directory flush");
    case PersistenceCheckpoint::AfterTargetDirectoryFlush:
        return QStringLiteral("after target directory flush");
    case PersistenceCheckpoint::BeforeJournalCompletion:
        return QStringLiteral("before journal completion");
    case PersistenceCheckpoint::AfterJournalCompletion:
        return QStringLiteral("after journal completion");
    }
    return QStringLiteral("unknown save checkpoint");
}

bool runCheckpoint(const PersistenceCheckpointHook &hook,
                   PersistenceCheckpoint checkpoint, QString *error)
{
    if (!hook || hook(checkpoint, error)) {
        return true;
    }
    if (error->isEmpty()) {
        *error = QStringLiteral("Save interrupted %1.").arg(checkpointName(checkpoint));
    }
    return false;
}

QString ioOperationName(PersistenceIoOperation operation)
{
    switch (operation) {
    case PersistenceIoOperation::ReadTarget:
        return QStringLiteral("read target");
    case PersistenceIoOperation::WriteTargetStage:
        return QStringLiteral("write staged target");
    case PersistenceIoOperation::FlushTargetStage:
        return QStringLiteral("flush staged target");
    case PersistenceIoOperation::RenameTarget:
        return QStringLiteral("rename target");
    case PersistenceIoOperation::FlushTargetDirectory:
        return QStringLiteral("flush target directory");
    case PersistenceIoOperation::ReadJournal:
        return QStringLiteral("read save journal");
    case PersistenceIoOperation::WriteJournalStage:
        return QStringLiteral("write staged save journal");
    case PersistenceIoOperation::FlushJournalStage:
        return QStringLiteral("flush staged save journal");
    case PersistenceIoOperation::RenameJournal:
        return QStringLiteral("rename save journal");
    case PersistenceIoOperation::FlushJournalDirectory:
        return QStringLiteral("flush save journal directory");
    }
    return QStringLiteral("unknown save I/O operation");
}

bool injectIoFailure(const PersistenceIoFailureHook &hook,
                     PersistenceIoOperation operation, QString *error)
{
    if (!hook || !hook(operation, error)) {
        return false;
    }
    if (error->isEmpty()) {
        *error = QStringLiteral("Injected failure while attempting to %1.")
                     .arg(ioOperationName(operation));
    }
    return true;
}

bool writeAll(int descriptor, const char *data, qsizetype size, QString *error,
              const QString &path)
{
    qsizetype offset = 0;
    while (offset < size) {
        const size_t remaining = static_cast<size_t>(size - offset);
        const size_t chunk = std::min(remaining, static_cast<size_t>(1024 * 1024));
        const ssize_t written = ::write(descriptor, data + offset, chunk);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            *error = systemError(QStringLiteral("Cannot write staged file"), path);
            return false;
        }
        offset += written;
    }
    return true;
}

bool writeStageFile(const QString &path, const QByteArray &bytes, mode_t mode,
                    const PersistenceCheckpointHook &checkpoint,
                    const PersistenceIoFailureHook &ioFailure,
                    PersistenceIoOperation writeOperation,
                    PersistenceIoOperation flushOperation, QString *error)
{
    const QByteArray nativePath = QFile::encodeName(path);
    const int descriptor = ::open(nativePath.constData(),
                                  O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        *error = systemError(QStringLiteral("Cannot create staged file"), path);
        return false;
    }

    bool ok = true;
    if (::fchmod(descriptor, mode & 0777) != 0) {
        *error = systemError(QStringLiteral("Cannot set staged file permissions"), path);
        ok = false;
    }
    if (ok && injectIoFailure(ioFailure, writeOperation, error)) {
        ok = false;
    }
    if (ok) {
        ok = writeAll(descriptor, bytes.constData(), bytes.size(), error, path);
    }
    if (ok) {
        ok = runCheckpoint(checkpoint, PersistenceCheckpoint::AfterTargetStaging, error);
    }
    if (ok) {
        ok = runCheckpoint(checkpoint, PersistenceCheckpoint::BeforeTargetFlush, error);
    }
    if (ok && injectIoFailure(ioFailure, flushOperation, error)) {
        ok = false;
    }
    if (ok && ::fsync(descriptor) != 0) {
        *error = systemError(QStringLiteral("Cannot flush staged file"), path);
        ok = false;
    }
    if (ok) {
        ok = runCheckpoint(checkpoint, PersistenceCheckpoint::AfterTargetFlush, error);
    }
    const int closeResult = ::close(descriptor);
    if (ok && closeResult != 0) {
        *error = systemError(QStringLiteral("Cannot close staged file"), path);
        ok = false;
    }
    if (!ok) {
        ::unlink(nativePath.constData());
    }
    return ok;
}

QString stagePathFor(const QString &path, const QString &operationId)
{
    const QFileInfo targetInfo(path);
    return QDir(targetInfo.absolutePath()).filePath(
        QStringLiteral(".%1.leo-%2.tmp").arg(targetInfo.fileName(), operationId));
}

bool removeInterruptedStage(const QString &target, const QString &operationId,
                            const QByteArray &expectedBytes, QString *error)
{
    const QString stagePath = stagePathFor(target, operationId);
    const QFileInfo stageInfo(stagePath);
    if (!stageInfo.exists() && !stageInfo.isSymLink()) {
        return true;
    }
    if (!stageInfo.isFile() || stageInfo.isSymLink()) {
        *error = QStringLiteral(
            "Save recovery found an unsafe staged file in %1. It was left untouched.")
                     .arg(stagePath);
        return false;
    }

    QFile stageFile(stagePath);
    if (!stageFile.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Cannot read interrupted stage %1: %2")
                     .arg(stagePath, stageFile.errorString());
        return false;
    }
    const QByteArray stagedBytes = stageFile.readAll();
    if (stageFile.error() != QFileDevice::NoError) {
        *error = QStringLiteral("Cannot read interrupted stage %1: %2")
                     .arg(stagePath, stageFile.errorString());
        return false;
    }
    if (stagedBytes != expectedBytes) {
        *error = QStringLiteral(
            "Save recovery found unexpected staged bytes in %1. They were left untouched.")
                     .arg(stagePath);
        return false;
    }
    if (::unlink(QFile::encodeName(stagePath).constData()) != 0) {
        *error = systemError(QStringLiteral("Cannot remove interrupted stage"), stagePath);
        return false;
    }
    return syncDirectory(QFileInfo(stagePath).absolutePath(), error);
}

bool fileHash(const QString &path, QByteArray *contents, QByteArray *digest,
              QString *error);

bool atomicReplace(const QString &path, const QByteArray &bytes, mode_t mode,
                   const QString &operationId, QString *error,
                   const PersistenceCheckpointHook &checkpoint = {},
                   const QByteArray &expectedCurrentHash = {}, bool *conflict = nullptr,
                   const PersistenceIoFailureHook &ioFailure = {}, bool journalFile = false)
{
    const QFileInfo targetInfo(path);
    const QString parent = targetInfo.absolutePath();
    const QString stagePath = stagePathFor(path, operationId);
    if (!removeInterruptedStage(path, operationId, bytes, error)) {
        return false;
    }
    const PersistenceIoOperation writeOperation = journalFile
        ? PersistenceIoOperation::WriteJournalStage
        : PersistenceIoOperation::WriteTargetStage;
    const PersistenceIoOperation flushOperation = journalFile
        ? PersistenceIoOperation::FlushJournalStage
        : PersistenceIoOperation::FlushTargetStage;
    const PersistenceIoOperation renameOperation = journalFile
        ? PersistenceIoOperation::RenameJournal
        : PersistenceIoOperation::RenameTarget;
    const PersistenceIoOperation directoryFlushOperation = journalFile
        ? PersistenceIoOperation::FlushJournalDirectory
        : PersistenceIoOperation::FlushTargetDirectory;
    if (!runCheckpoint(checkpoint, PersistenceCheckpoint::BeforeTargetStaging, error) ||
        !writeStageFile(stagePath, bytes, mode, checkpoint, ioFailure,
                        writeOperation, flushOperation, error)) {
        return false;
    }

    const QByteArray nativeStage = QFile::encodeName(stagePath);
    const QByteArray nativeTarget = QFile::encodeName(path);
    if (!runCheckpoint(checkpoint, PersistenceCheckpoint::BeforeTargetRename, error)) {
        ::unlink(nativeStage.constData());
        return false;
    }
    if (!expectedCurrentHash.isEmpty()) {
        QByteArray currentBytes;
        QByteArray currentHash;
        if (injectIoFailure(ioFailure, PersistenceIoOperation::ReadTarget, error)) {
            ::unlink(nativeStage.constData());
            return false;
        }
        if (!fileHash(path, &currentBytes, &currentHash, error)) {
            ::unlink(nativeStage.constData());
            return false;
        }
        if (currentHash != expectedCurrentHash) {
            *error = QStringLiteral(
                "This chapter changed before atomic replacement. The current file was left untouched.");
            if (conflict) {
                *conflict = true;
            }
            ::unlink(nativeStage.constData());
            return false;
        }
    }
    if (injectIoFailure(ioFailure, renameOperation, error)) {
        ::unlink(nativeStage.constData());
        return false;
    }
    if (::rename(nativeStage.constData(), nativeTarget.constData()) != 0) {
        *error = systemError(QStringLiteral("Cannot atomically replace file"), path);
        ::unlink(nativeStage.constData());
        return false;
    }
    if (!runCheckpoint(checkpoint, PersistenceCheckpoint::AfterTargetRename, error) ||
        !runCheckpoint(checkpoint, PersistenceCheckpoint::BeforeTargetDirectoryFlush, error)) {
        return false;
    }
    if (injectIoFailure(ioFailure, directoryFlushOperation, error) ||
        !syncDirectory(parent, error)) {
        return false;
    }
    return runCheckpoint(checkpoint, PersistenceCheckpoint::AfterTargetDirectoryFlush, error);
}

bool modeForFile(const QString &path, mode_t *mode, QString *error)
{
    struct stat fileStatus {};
    if (::stat(QFile::encodeName(path).constData(), &fileStatus) != 0) {
        *error = systemError(QStringLiteral("Cannot inspect file permissions"), path);
        return false;
    }
    *mode = fileStatus.st_mode & 0777;
    return true;
}

mode_t modeForPermissions(QFile::Permissions permissions)
{
    mode_t mode = 0;
    if (permissions.testFlag(QFileDevice::ReadOwner) ||
        permissions.testFlag(QFileDevice::ReadUser)) {
        mode |= S_IRUSR;
    }
    if (permissions.testFlag(QFileDevice::WriteOwner) ||
        permissions.testFlag(QFileDevice::WriteUser)) {
        mode |= S_IWUSR;
    }
    if (permissions.testFlag(QFileDevice::ExeOwner) ||
        permissions.testFlag(QFileDevice::ExeUser)) {
        mode |= S_IXUSR;
    }
    if (permissions.testFlag(QFileDevice::ReadGroup)) {
        mode |= S_IRGRP;
    }
    if (permissions.testFlag(QFileDevice::WriteGroup)) {
        mode |= S_IWGRP;
    }
    if (permissions.testFlag(QFileDevice::ExeGroup)) {
        mode |= S_IXGRP;
    }
    if (permissions.testFlag(QFileDevice::ReadOther)) {
        mode |= S_IROTH;
    }
    if (permissions.testFlag(QFileDevice::WriteOther)) {
        mode |= S_IWOTH;
    }
    if (permissions.testFlag(QFileDevice::ExeOther)) {
        mode |= S_IXOTH;
    }
    return mode;
}

bool validRelativePath(const QString &path)
{
    if (path.isEmpty() || path.startsWith(QLatin1Char('/')) ||
        path.contains(QLatin1Char('\\')) || path.contains(QChar::Null) ||
        QDir::cleanPath(path) != path) {
        return false;
    }
    const QStringList components = path.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    for (const QString &component : components) {
        if (component.isEmpty() || component == QStringLiteral(".") ||
            component == QStringLiteral("..")) {
            return false;
        }
    }
    return true;
}

bool resolveTarget(const QString &root, const QString &relativePath, QString *target,
                   QString *error)
{
    if (!validRelativePath(relativePath)) {
        *error = QStringLiteral("Unsafe Library file path: %1").arg(relativePath);
        return false;
    }

    const QString absoluteTarget = QDir(root).filePath(relativePath);
    const QStringList components = relativePath.split(QLatin1Char('/'));
    QString current = root;
    for (qsizetype index = 0; index + 1 < components.size(); ++index) {
        current = QDir(current).filePath(components.at(index));
        const QFileInfo parentInfo(current);
        if (!parentInfo.isDir() || parentInfo.isSymLink()) {
            *error = QStringLiteral("Library chapter folder is missing or unsafe: %1")
                         .arg(current);
            return false;
        }
    }

    const QFileInfo parentInfo(QFileInfo(absoluteTarget).absolutePath());
    const QString canonicalParent = parentInfo.canonicalFilePath();
    if (canonicalParent.isEmpty() || !isWithin(root, canonicalParent)) {
        *error = QStringLiteral("Library file path resolves outside the Library: %1")
                     .arg(relativePath);
        return false;
    }
    const QFileInfo targetInfo(absoluteTarget);
    if (!targetInfo.exists() || !targetInfo.isFile() || targetInfo.isSymLink()) {
        *error = QStringLiteral("Library file is missing or unsafe: %1").arg(absoluteTarget);
        return false;
    }
    *target = absoluteTarget;
    return true;
}

bool readFile(const QString &path, QByteArray *bytes, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Cannot read %1: %2").arg(path, file.errorString());
        return false;
    }
    *bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        *error = QStringLiteral("Cannot read %1: %2").arg(path, file.errorString());
        return false;
    }
    return true;
}

QString newOperationId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool validOperationId(const QString &id)
{
    const QUuid parsed(id);
    return !parsed.isNull() && parsed.toString(QUuid::WithoutBraces) == id;
}

QJsonObject journalObject(const Journal &journal)
{
    return {{QStringLiteral("id"), journal.id},
            {QStringLiteral("library_path"), journal.libraryPath},
            {QStringLiteral("relative_path"), journal.relativePath},
            {QStringLiteral("old_bytes"), QString::fromLatin1(journal.oldBytes.toBase64())},
            {QStringLiteral("new_bytes"), QString::fromLatin1(journal.newBytes.toBase64())},
            {QStringLiteral("old_sha256"), QString::fromLatin1(LibraryPersistence::hash(journal.oldBytes))},
            {QStringLiteral("new_sha256"), QString::fromLatin1(LibraryPersistence::hash(journal.newBytes))},
            {QStringLiteral("state"), journal.state}};
}

bool writeJournal(const QString &directory, const Journal &journal, QString *path,
                  QString *error, const PersistenceIoFailureHook &ioFailure = {})
{
    if (!ensurePrivateDirectory(directory, error)) {
        return false;
    }
    const QString journalPath = QDir(directory).filePath(journal.id + QStringLiteral(".json"));
    const QByteArray bytes = QJsonDocument(journalObject(journal)).toJson(QJsonDocument::Compact);
    if (!atomicReplace(journalPath, bytes, 0600, journal.id, error,
                       {}, {}, nullptr, ioFailure, true)) {
        return false;
    }
    *path = journalPath;
    return true;
}

bool parseJournal(const QString &path, Journal *journal, QString *error,
                  const PersistenceIoFailureHook &ioFailure = {})
{
    QByteArray bytes;
    if (injectIoFailure(ioFailure, PersistenceIoOperation::ReadJournal, error)) {
        return false;
    }
    if (!readFile(path, &bytes, error)) {
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        *error = QStringLiteral("Cannot recover save journal %1: invalid JSON.").arg(path);
        return false;
    }

    const QJsonObject object = document.object();
    const QString oldBase64 = object.value(QStringLiteral("old_bytes")).toString();
    const QString newBase64 = object.value(QStringLiteral("new_bytes")).toString();
    const QByteArray oldBytes = QByteArray::fromBase64(oldBase64.toLatin1());
    const QByteArray newBytes = QByteArray::fromBase64(newBase64.toLatin1());
    const QString id = object.value(QStringLiteral("id")).toString();
    const QString libraryPath = object.value(QStringLiteral("library_path")).toString();
    const QString relativePath = object.value(QStringLiteral("relative_path")).toString();
    const QString state = object.value(QStringLiteral("state")).toString();
    if (!validOperationId(id) ||
        QFileInfo(path).completeBaseName() != id || libraryPath.isEmpty() ||
        !validRelativePath(relativePath) ||
        (state != QStringLiteral("prepared") && state != QStringLiteral("complete")) ||
        QString::fromLatin1(oldBytes.toBase64()) != oldBase64 ||
        QString::fromLatin1(newBytes.toBase64()) != newBase64 ||
        object.value(QStringLiteral("old_sha256")).toString().toLatin1() !=
            LibraryPersistence::hash(oldBytes) ||
        object.value(QStringLiteral("new_sha256")).toString().toLatin1() !=
            LibraryPersistence::hash(newBytes)) {
        *error = QStringLiteral("Cannot recover save journal %1: invalid journal data.").arg(path);
        return false;
    }

    journal->id = id;
    journal->libraryPath = libraryPath;
    journal->relativePath = relativePath;
    journal->oldBytes = oldBytes;
    journal->newBytes = newBytes;
    journal->state = state;
    return true;
}

bool finishJournal(const QString &directory, const QString &journalPath, Journal *journal,
                   QString *error, const PersistenceCheckpointHook &checkpoint = {},
                   const PersistenceIoFailureHook &ioFailure = {})
{
    if (journal->state != QStringLiteral("complete")) {
        if (!runCheckpoint(checkpoint, PersistenceCheckpoint::BeforeJournalCompletion, error)) {
            return false;
        }
        journal->state = QStringLiteral("complete");
        QString ignoredPath;
        if (!writeJournal(directory, *journal, &ignoredPath, error, ioFailure)) {
            return false;
        }
        if (!runCheckpoint(checkpoint, PersistenceCheckpoint::AfterJournalCompletion, error)) {
            return false;
        }
    }

    const QByteArray nativePath = QFile::encodeName(journalPath);
    if (::unlink(nativePath.constData()) != 0 && errno != ENOENT) {
        return true;
    }
    QString cleanupError;
    // Durable completion already committed the save; journal cleanup cannot revoke it.
    syncDirectory(directory, &cleanupError);
    return true;
}

bool fileHash(const QString &path, QByteArray *contents, QByteArray *digest,
              QString *error)
{
    if (!readFile(path, contents, error)) {
        return false;
    }
    *digest = LibraryPersistence::hash(*contents);
    return true;
}

PersistenceResult recoverOne(const QString &directory, const QString &journalPath,
                             Journal journal, const PersistenceIoFailureHook &ioFailure)
{
    PersistenceResult result;
    QString target;
    if (!resolveTarget(journal.libraryPath, journal.relativePath, &target, &result.error)) {
        result.conflict = true;
        return result;
    }

    QByteArray currentBytes;
    QByteArray currentHash;
    if (injectIoFailure(ioFailure, PersistenceIoOperation::ReadTarget, &result.error)) {
        result.conflict = true;
        return result;
    }
    if (!fileHash(target, &currentBytes, &currentHash, &result.error)) {
        result.conflict = true;
        return result;
    }
    const QByteArray oldHash = LibraryPersistence::hash(journal.oldBytes);
    const QByteArray newHash = LibraryPersistence::hash(journal.newBytes);
    if (currentHash != oldHash && currentHash != newHash) {
        result.conflict = true;
        result.error = QStringLiteral(
            "Save recovery found an external change in %1. The Library file was left untouched.")
                           .arg(journal.relativePath);
        return result;
    }

    if (!removeInterruptedStage(target, journal.id, journal.newBytes, &result.error)) {
        result.conflict = true;
        return result;
    }

    if (currentHash == oldHash) {
        mode_t mode = 0;
        if (!modeForFile(target, &mode, &result.error)) {
            return result;
        }
        bool targetConflict = false;
        if (!atomicReplace(target, journal.newBytes, mode, journal.id, &result.error,
                           {}, oldHash, &targetConflict, ioFailure)) {
            result.conflict = targetConflict;
            return result;
        }
    }

    if (!syncDirectory(QFileInfo(target).absolutePath(), &result.error)) {
        return result;
    }
    if (!finishJournal(directory, journalPath, &journal, &result.error, {}, ioFailure)) {
        return result;
    }
    result.ok = true;
    result.recovered = true;
    result.savedHash = newHash;
    return result;
}

bool scanTree(const QString &root, const QString &relativeDirectory, Manifest *manifest,
              QString *error)
{
    const QString absoluteDirectory = relativeDirectory.isEmpty()
                                          ? root
                                          : QDir(root).filePath(relativeDirectory);
    QDir directory(absoluteDirectory);
    if (!directory.exists() || !directory.isReadable()) {
        *error = QStringLiteral("Cannot inspect Library folder: %1").arg(absoluteDirectory);
        return false;
    }

    const QFileInfoList entries = directory.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::Name | QDir::DirsFirst);
    for (const QFileInfo &entry : entries) {
        if (relativeDirectory.isEmpty() &&
            (entry.fileName() == QStringLiteral("Backups") ||
             entry.fileName() == QStringLiteral("Exports"))) {
            continue;
        }
        if (entry.isSymLink()) {
            *error = QStringLiteral("Safety snapshot refused symbolic link: %1")
                         .arg(entry.filePath());
            return false;
        }

        const QString relativePath = relativeDirectory.isEmpty()
                                         ? entry.fileName()
                                         : relativeDirectory + QLatin1Char('/') + entry.fileName();
        if (entry.isDir()) {
            manifest->insert(QStringLiteral("D:") + relativePath, QByteArray());
            if (!scanTree(root, relativePath, manifest, error)) {
                return false;
            }
        } else if (entry.isFile()) {
            QFile file(entry.filePath());
            if (!file.open(QIODevice::ReadOnly)) {
                *error = QStringLiteral("Cannot read %1: %2").arg(entry.filePath(),
                                                                  file.errorString());
                return false;
            }
            QCryptographicHash fileHash(QCryptographicHash::Sha256);
            while (true) {
                const QByteArray chunk = file.read(1024 * 1024);
                if (chunk.isEmpty()) {
                    if (file.error() != QFileDevice::NoError) {
                        *error = QStringLiteral("Cannot read %1: %2")
                                     .arg(entry.filePath(), file.errorString());
                        return false;
                    }
                    break;
                }
                fileHash.addData(chunk);
            }
            manifest->insert(QStringLiteral("F:") + relativePath, fileHash.result().toHex());
        } else {
            *error = QStringLiteral("Safety snapshot refused special file: %1")
                         .arg(entry.filePath());
            return false;
        }
    }
    return true;
}

QByteArray manifestHash(const Manifest &manifest)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const char separator = '\0';
    const char lineBreak = '\n';
    for (auto entry = manifest.cbegin(); entry != manifest.cend(); ++entry) {
        hash.addData(entry.key().toUtf8());
        hash.addData(QByteArrayView(&separator, 1));
        hash.addData(entry.value());
        hash.addData(QByteArrayView(&lineBreak, 1));
    }
    return hash.result().toHex();
}

bool copyFile(const QString &sourcePath, const QString &destinationPath,
              const QByteArray &expectedHash, QString *error)
{
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Cannot read %1: %2").arg(sourcePath, source.errorString());
        return false;
    }
    if (!QDir().mkpath(QFileInfo(destinationPath).absolutePath())) {
        *error = QStringLiteral("Cannot create snapshot folder for %1.").arg(destinationPath);
        return false;
    }

    const QByteArray nativeDestination = QFile::encodeName(destinationPath);
    const int descriptor = ::open(nativeDestination.constData(),
                                  O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        *error = systemError(QStringLiteral("Cannot create snapshot file"), destinationPath);
        return false;
    }

    const mode_t mode = modeForPermissions(QFileInfo(sourcePath).permissions());
    bool ok = true;
    if (::fchmod(descriptor, mode) != 0) {
        *error = systemError(QStringLiteral("Cannot set snapshot permissions"), destinationPath);
        ok = false;
    }
    QCryptographicHash copiedHash(QCryptographicHash::Sha256);
    while (ok) {
        const QByteArray chunk = source.read(1024 * 1024);
        if (chunk.isEmpty()) {
            if (source.error() != QFileDevice::NoError) {
                *error = QStringLiteral("Cannot read %1: %2").arg(sourcePath,
                                                                  source.errorString());
                ok = false;
            }
            break;
        }
        copiedHash.addData(chunk);
        ok = writeAll(descriptor, chunk.constData(), chunk.size(), error, destinationPath);
    }
    if (ok && copiedHash.result().toHex() != expectedHash) {
        *error = QStringLiteral("Library file changed while making safety snapshot: %1")
                     .arg(sourcePath);
        ok = false;
    }
    if (ok && ::fsync(descriptor) != 0) {
        *error = systemError(QStringLiteral("Cannot flush snapshot file"), destinationPath);
        ok = false;
    }
    const int closeResult = ::close(descriptor);
    if (ok && closeResult != 0) {
        *error = systemError(QStringLiteral("Cannot close snapshot file"), destinationPath);
        ok = false;
    }
    if (!ok) {
        ::unlink(nativeDestination.constData());
    }
    return ok;
}

bool syncSnapshotDirectories(const QString &root, const QString &relativeDirectory,
                             QString *error)
{
    const QString absoluteDirectory = relativeDirectory.isEmpty()
                                          ? root
                                          : QDir(root).filePath(relativeDirectory);
    QDir directory(absoluteDirectory);
    const QFileInfoList entries = directory.entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
    for (const QFileInfo &entry : entries) {
        const QString relativePath = relativeDirectory.isEmpty()
                                         ? entry.fileName()
                                         : relativeDirectory + QLatin1Char('/') + entry.fileName();
        if (!syncSnapshotDirectories(root, relativePath, error)) {
            return false;
        }
    }
    return syncDirectory(absoluteDirectory, error);
}

bool markerIsValid(const QString &markerPath, const QString &libraryRoot)
{
    if (QFileInfo(markerPath).isSymLink()) {
        return false;
    }
    QByteArray bytes;
    QString error;
    if (!readFile(markerPath, &bytes, &error)) {
        return false;
    }
    const QJsonDocument document = QJsonDocument::fromJson(bytes);
    if (!document.isObject()) {
        return false;
    }
    const QJsonObject object = document.object();
    if (object.value(QStringLiteral("library_path")).toString() != libraryRoot ||
        object.value(QStringLiteral("verified")).toBool() != true) {
        return false;
    }
    const QString snapshotPath = object.value(QStringLiteral("snapshot_path")).toString();
    if (snapshotPath.isEmpty() || !isWithin(snapshotDirectory(), snapshotPath) ||
        isWithin(libraryRoot, snapshotPath)) {
        return false;
    }
    const QFileInfo snapshotInfo(snapshotPath);
    return snapshotInfo.isDir() && !snapshotInfo.isSymLink();
}

bool ensureSafetySnapshot(const QString &libraryRoot, QString *error)
{
    const QString baseDirectory = snapshotDirectory();
    if (!ensureOutsideLibrary(baseDirectory, libraryRoot, error) ||
        !ensurePrivateDirectory(baseDirectory, error)) {
        return false;
    }

    const QByteArray pathHash = QCryptographicHash::hash(
                                    libraryRoot.toUtf8(), QCryptographicHash::Sha256)
                                    .toHex();
    const QString markerPath = QDir(baseDirectory).filePath(
        QString::fromLatin1(pathHash) + QStringLiteral(".json"));
    if (markerIsValid(markerPath, libraryRoot)) {
        return true;
    }

    Manifest before;
    if (!scanTree(libraryRoot, QString(), &before, error)) {
        return false;
    }

    const QString bucketPath = QDir(baseDirectory).filePath(QString::fromLatin1(pathHash));
    if (!ensurePrivateDirectory(bucketPath, error)) {
        return false;
    }
    const QString operationId = newOperationId();
    const QString temporaryPath = QDir(bucketPath).filePath(QStringLiteral(".pending-") + operationId);
    const QString finalPath = QDir(bucketPath).filePath(
        QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'")) +
        QLatin1Char('-') + operationId);
    if (!QDir().mkpath(temporaryPath) ||
        ::chmod(QFile::encodeName(temporaryPath).constData(), 0700) != 0) {
        *error = systemError(QStringLiteral("Cannot create safety snapshot"), temporaryPath);
        return false;
    }

    bool copied = true;
    for (auto entry = before.cbegin(); entry != before.cend() && copied; ++entry) {
        const QString relativePath = entry.key().mid(2);
        const QString destination = QDir(temporaryPath).filePath(relativePath);
        if (entry.key().startsWith(QStringLiteral("D:"))) {
            if (!QDir().mkpath(destination)) {
                *error = QStringLiteral("Cannot create snapshot folder %1.").arg(destination);
                copied = false;
            }
        } else {
            copied = copyFile(QDir(libraryRoot).filePath(relativePath), destination,
                              entry.value(), error);
        }
    }

    Manifest afterSource;
    Manifest snapshot;
    if (copied && !scanTree(libraryRoot, QString(), &afterSource, error)) {
        copied = false;
    }
    if (copied && !scanTree(temporaryPath, QString(), &snapshot, error)) {
        copied = false;
    }
    if (copied && (before != afterSource || before != snapshot)) {
        *error = QStringLiteral(
            "Library changed while making its safety snapshot. No chapter was saved.");
        copied = false;
    }
    if (copied) {
        for (auto entry = before.cbegin(); entry != before.cend(); ++entry) {
            if (!entry.key().startsWith(QStringLiteral("D:"))) {
                continue;
            }
            const QString relativePath = entry.key().mid(2);
            const QString sourcePath = QDir(libraryRoot).filePath(relativePath);
            const QString destination = QDir(temporaryPath).filePath(relativePath);
            if (!QFile::setPermissions(destination, QFileInfo(sourcePath).permissions())) {
                *error = QStringLiteral("Cannot preserve snapshot folder permissions: %1")
                             .arg(destination);
                copied = false;
                break;
            }
        }
    }
    if (copied && !syncSnapshotDirectories(temporaryPath, QString(), error)) {
        copied = false;
    }
    if (!copied) {
        QDir(temporaryPath).removeRecursively();
        return false;
    }

    const QByteArray nativeTemporary = QFile::encodeName(temporaryPath);
    const QByteArray nativeFinal = QFile::encodeName(finalPath);
    if (::rename(nativeTemporary.constData(), nativeFinal.constData()) != 0) {
        *error = systemError(QStringLiteral("Cannot publish safety snapshot"), finalPath);
        QDir(temporaryPath).removeRecursively();
        return false;
    }
    if (!syncDirectory(bucketPath, error)) {
        return false;
    }

    const QJsonObject marker{{QStringLiteral("library_path"), libraryRoot},
                             {QStringLiteral("snapshot_path"), finalPath},
                             {QStringLiteral("manifest_sha256"),
                              QString::fromLatin1(manifestHash(before))},
                             {QStringLiteral("created_utc"),
                              QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                             {QStringLiteral("verified"), true}};
    const QByteArray markerBytes = QJsonDocument(marker).toJson(QJsonDocument::Compact);
    if (!atomicReplace(markerPath, markerBytes, 0600, operationId, error)) {
        return false;
    }
    return true;
}

PersistenceResult failed(const QString &error, bool conflict = false)
{
    PersistenceResult result;
    result.error = error;
    result.conflict = conflict;
    return result;
}

}

QByteArray LibraryPersistence::hash(const QByteArray &bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

bool LibraryPersistence::readLibraryFile(const QString &libraryPath,
                                         const QString &relativePath,
                                         QByteArray *bytes,
                                         QString *error)
{
    const QString libraryRoot = canonicalDirectory(libraryPath);
    QString target;
    if (!resolveTarget(libraryRoot, relativePath, &target, error)) {
        return false;
    }
    return readFile(target, bytes, error);
}

PersistenceResult LibraryPersistence::recoverPendingSaves(
    const QString &libraryPath, const PersistenceIoFailureHook &ioFailure)
{
    const QString libraryRoot = canonicalDirectory(libraryPath);
    const QString directory = journalDirectory();
    if (!QDir(directory).exists()) {
        PersistenceResult result;
        result.ok = true;
        return result;
    }
    QString outsideError;
    if (!ensureOutsideLibrary(directory, libraryRoot, &outsideError)) {
        return failed(outsideError);
    }

    const QFileInfo directoryInfo(directory);
    if (directoryInfo.isSymLink() || !directoryInfo.isReadable()) {
        return failed(QStringLiteral("Cannot inspect private save journals in %1.").arg(directory));
    }
    const QFileInfoList journals = QDir(directory).entryInfoList(
        {QStringLiteral("*.json")}, QDir::Files | QDir::NoSymLinks, QDir::Name);
    bool recovered = false;
    for (const QFileInfo &entry : journals) {
        Journal journal;
        QString error;
        if (!parseJournal(entry.filePath(), &journal, &error, ioFailure)) {
            PersistenceResult failure = failed(error);
            failure.recovered = recovered;
            return failure;
        }
        if (canonicalDirectory(journal.libraryPath) != libraryRoot) {
            continue;
        }
        const PersistenceResult result = recoverOne(directory, entry.filePath(), journal, ioFailure);
        if (!result.ok) {
            PersistenceResult failure = result;
            failure.recovered = recovered || result.recovered;
            return failure;
        }
        recovered = recovered || result.recovered;
    }

    PersistenceResult result;
    result.ok = true;
    result.recovered = recovered;
    return result;
}

PersistenceResult LibraryPersistence::saveFile(const QString &libraryPath,
                                                const QString &relativePath,
                                                const QByteArray &expectedHash,
                                                const QByteArray &newBytes,
                                                const PersistenceCheckpointHook &checkpoint,
                                                const PersistenceIoFailureHook &ioFailure)
{
    const QString libraryRoot = canonicalDirectory(libraryPath);
    const PersistenceResult recovery = recoverPendingSaves(libraryRoot, ioFailure);
    if (!recovery.ok) {
        return recovery;
    }

    QString target;
    QString error;
    if (!resolveTarget(libraryRoot, relativePath, &target, &error)) {
        return failed(error);
    }

    QByteArray currentBytes;
    QByteArray currentHash;
    if (!fileHash(target, &currentBytes, &currentHash, &error)) {
        return failed(error);
    }
    const QByteArray newHash = hash(newBytes);
    if (currentHash == newHash) {
        PersistenceResult result;
        result.ok = true;
        result.recovered = recovery.recovered;
        result.savedHash = currentHash;
        return result;
    }
    if (expectedHash.isEmpty() || currentHash != expectedHash) {
        return failed(QStringLiteral(
                          "This chapter changed outside LEO. The current file was left untouched."),
                      true);
    }

    if (!ensureSafetySnapshot(libraryRoot, &error)) {
        return failed(error);
    }

    if (!fileHash(target, &currentBytes, &currentHash, &error)) {
        return failed(error);
    }
    if (currentHash != expectedHash) {
        return failed(QStringLiteral(
                          "This chapter changed while LEO prepared its safety snapshot. "
                          "The current file was left untouched."),
                      true);
    }

    Journal journal;
    journal.id = newOperationId();
    journal.libraryPath = libraryRoot;
    journal.relativePath = relativePath;
    journal.oldBytes = currentBytes;
    journal.newBytes = newBytes;
    journal.state = QStringLiteral("prepared");
    QString journalPath;
    const QString privateJournalDirectory = journalDirectory();
    if (!ensureOutsideLibrary(privateJournalDirectory, libraryRoot, &error) ||
        !writeJournal(privateJournalDirectory, journal, &journalPath, &error, ioFailure)) {
        return failed(error);
    }

    if (!fileHash(target, &currentBytes, &currentHash, &error)) {
        return failed(error);
    }
    if (currentHash != expectedHash) {
        return failed(QStringLiteral(
                          "This chapter changed before atomic replacement. "
                          "The local draft remains in the private save journal."),
                      true);
    }

    mode_t mode = 0;
    if (!modeForFile(target, &mode, &error)) {
        return failed(error);
    }
    bool targetConflict = false;
    if (!atomicReplace(target, newBytes, mode, journal.id, &error,
                       checkpoint, expectedHash, &targetConflict, ioFailure)) {
        return failed(error, targetConflict);
    }
    if (!finishJournal(privateJournalDirectory, journalPath, &journal, &error,
                       checkpoint, ioFailure)) {
        PersistenceResult result = failed(error);
        result.recovered = recovery.recovered;
        return result;
    }

    PersistenceResult result;
    result.ok = true;
    result.recovered = recovery.recovered;
    result.savedHash = newHash;
    return result;
}
