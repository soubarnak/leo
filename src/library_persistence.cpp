#include "library_persistence.h"

#include "app_paths.h"
#include "library_reader.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QByteArrayView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSet>
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
    struct File {
        QString relativePath;
        bool oldExists = true;
        QByteArray oldBytes;
        QByteArray newBytes;
    };

    QString id;
    QString libraryPath;
    QString relativePath;
    QByteArray oldBytes;
    QByteArray newBytes;
    QVector<File> files;
    QString state;
};

PersistenceResult conflictWithPreservedDraft(const QString &libraryPath,
                                             const QString &relativePath,
                                             const QByteArray &draftBytes,
                                             const QString &reason);
PersistenceResult failed(const QString &error, bool conflict = false);

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
                   const PersistenceIoFailureHook &ioFailure = {}, bool journalFile = false,
                   bool expectedAbsent = false)
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
    if (expectedAbsent && (QFileInfo::exists(path) || QFileInfo(path).isSymLink())) {
        *error = QStringLiteral(
            "A Library file appeared before atomic creation. The current file was left untouched.");
        if (conflict) {
            *conflict = true;
        }
        ::unlink(nativeStage.constData());
        return false;
    }
    if (injectIoFailure(ioFailure, renameOperation, error)) {
        ::unlink(nativeStage.constData());
        return false;
    }
    if (expectedAbsent) {
        if (::link(nativeStage.constData(), nativeTarget.constData()) != 0) {
            if (errno == EEXIST && conflict) {
                *conflict = true;
                *error = QStringLiteral(
                    "A Library file appeared before atomic creation. The current file was left untouched.");
            } else {
                *error = systemError(QStringLiteral("Cannot atomically create file"), path);
            }
            ::unlink(nativeStage.constData());
            return false;
        }
        if (::unlink(nativeStage.constData()) != 0) {
            *error = systemError(QStringLiteral("Cannot remove staged file"), stagePath);
            return false;
        }
    } else if (::rename(nativeStage.constData(), nativeTarget.constData()) != 0) {
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
                   QString *error, bool allowMissing = false, bool *exists = nullptr)
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
    const bool targetExists = targetInfo.exists() || targetInfo.isSymLink();
    if (exists) {
        *exists = targetExists;
    }
    if ((!allowMissing && !targetExists) ||
        (targetExists && (!targetInfo.isFile() || targetInfo.isSymLink()))) {
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
    QJsonObject object{{QStringLiteral("id"), journal.id},
                       {QStringLiteral("library_path"), journal.libraryPath},
                       {QStringLiteral("state"), journal.state}};
    if (journal.files.isEmpty()) {
        object.insert(QStringLiteral("relative_path"), journal.relativePath);
        object.insert(QStringLiteral("old_bytes"),
                      QString::fromLatin1(journal.oldBytes.toBase64()));
        object.insert(QStringLiteral("new_bytes"),
                      QString::fromLatin1(journal.newBytes.toBase64()));
        object.insert(QStringLiteral("old_sha256"),
                      QString::fromLatin1(LibraryPersistence::hash(journal.oldBytes)));
        object.insert(QStringLiteral("new_sha256"),
                      QString::fromLatin1(LibraryPersistence::hash(journal.newBytes)));
        return object;
    }

    QJsonArray files;
    for (const Journal::File &file : journal.files) {
        files.append(QJsonObject{
            {QStringLiteral("relative_path"), file.relativePath},
            {QStringLiteral("old_exists"), file.oldExists},
            {QStringLiteral("old_bytes"), QString::fromLatin1(file.oldBytes.toBase64())},
            {QStringLiteral("new_bytes"), QString::fromLatin1(file.newBytes.toBase64())},
            {QStringLiteral("old_sha256"), file.oldExists
                 ? QString::fromLatin1(LibraryPersistence::hash(file.oldBytes))
                 : QString()},
            {QStringLiteral("new_sha256"),
             QString::fromLatin1(LibraryPersistence::hash(file.newBytes))}});
    }
    object.insert(QStringLiteral("files"), files);
    return object;
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
    const QString id = object.value(QStringLiteral("id")).toString();
    const QString libraryPath = object.value(QStringLiteral("library_path")).toString();
    const QString state = object.value(QStringLiteral("state")).toString();
    if (!validOperationId(id) || QFileInfo(path).completeBaseName() != id ||
        libraryPath.isEmpty() ||
        (state != QStringLiteral("prepared") && state != QStringLiteral("complete"))) {
        *error = QStringLiteral("Cannot recover save journal %1: invalid journal data.").arg(path);
        return false;
    }

    journal->id = id;
    journal->libraryPath = libraryPath;
    journal->state = state;
    const QJsonValue filesValue = object.value(QStringLiteral("files"));
    if (filesValue.isArray()) {
        const QJsonArray files = filesValue.toArray();
        if (files.isEmpty()) {
            *error = QStringLiteral("Cannot recover save journal %1: empty file transaction.")
                         .arg(path);
            return false;
        }
        QSet<QString> paths;
        for (const QJsonValue &value : files) {
            if (!value.isObject()) {
                *error = QStringLiteral("Cannot recover save journal %1: invalid file transaction.")
                             .arg(path);
                return false;
            }
            const QJsonObject fileObject = value.toObject();
            Journal::File file;
            file.relativePath = fileObject.value(QStringLiteral("relative_path")).toString();
            file.oldExists = fileObject.value(QStringLiteral("old_exists")).toBool();
            const QString oldBase64 = fileObject.value(QStringLiteral("old_bytes")).toString();
            const QString newBase64 = fileObject.value(QStringLiteral("new_bytes")).toString();
            file.oldBytes = QByteArray::fromBase64(oldBase64.toLatin1());
            file.newBytes = QByteArray::fromBase64(newBase64.toLatin1());
            const QString oldHash = fileObject.value(QStringLiteral("old_sha256")).toString();
            const QString newHash = fileObject.value(QStringLiteral("new_sha256")).toString();
            const bool oldDataValid = file.oldExists
                ? QString::fromLatin1(file.oldBytes.toBase64()) == oldBase64 &&
                      oldHash.toLatin1() == LibraryPersistence::hash(file.oldBytes)
                : oldBase64.isEmpty() && oldHash.isEmpty();
            if (!validRelativePath(file.relativePath) || paths.contains(file.relativePath) ||
                !fileObject.value(QStringLiteral("old_exists")).isBool() || !oldDataValid ||
                QString::fromLatin1(file.newBytes.toBase64()) != newBase64 ||
                newHash.toLatin1() != LibraryPersistence::hash(file.newBytes)) {
                *error = QStringLiteral("Cannot recover save journal %1: invalid file transaction.")
                             .arg(path);
                return false;
            }
            paths.insert(file.relativePath);
            journal->files.append(file);
        }
        return true;
    }

    const QString oldBase64 = object.value(QStringLiteral("old_bytes")).toString();
    const QString newBase64 = object.value(QStringLiteral("new_bytes")).toString();
    const QByteArray oldBytes = QByteArray::fromBase64(oldBase64.toLatin1());
    const QByteArray newBytes = QByteArray::fromBase64(newBase64.toLatin1());
    const QString relativePath = object.value(QStringLiteral("relative_path")).toString();
    if (!validRelativePath(relativePath) ||
        QString::fromLatin1(oldBytes.toBase64()) != oldBase64 ||
        QString::fromLatin1(newBytes.toBase64()) != newBase64 ||
        object.value(QStringLiteral("old_sha256")).toString().toLatin1() !=
            LibraryPersistence::hash(oldBytes) ||
        object.value(QStringLiteral("new_sha256")).toString().toLatin1() !=
            LibraryPersistence::hash(newBytes)) {
        *error = QStringLiteral("Cannot recover save journal %1: invalid journal data.").arg(path);
        return false;
    }
    journal->relativePath = relativePath;
    journal->oldBytes = oldBytes;
    journal->newBytes = newBytes;
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

PersistenceResult recoverBatch(const QString &directory, const QString &journalPath,
                               Journal journal, const PersistenceIoFailureHook &ioFailure,
                               bool preserveConflictDraft);

PersistenceResult recoverOne(const QString &directory, const QString &journalPath,
                             Journal journal, const PersistenceIoFailureHook &ioFailure,
                             bool preserveConflictDraft)
{
    if (!journal.files.isEmpty()) {
        return recoverBatch(directory, journalPath, std::move(journal), ioFailure,
                           preserveConflictDraft);
    }
    const auto pauseForConflict = [&](const QString &reason) {
        if (preserveConflictDraft) {
            return conflictWithPreservedDraft(journal.libraryPath, journal.relativePath,
                                              journal.newBytes, reason);
        }
        PersistenceResult paused;
        paused.conflict = true;
        paused.error = reason;
        return paused;
    };

    PersistenceResult result;
    QString target;
    if (!resolveTarget(journal.libraryPath, journal.relativePath, &target, &result.error)) {
        return pauseForConflict(result.error);
    }

    QByteArray currentBytes;
    QByteArray currentHash;
    if (injectIoFailure(ioFailure, PersistenceIoOperation::ReadTarget, &result.error)) {
        return pauseForConflict(result.error);
    }
    if (!fileHash(target, &currentBytes, &currentHash, &result.error)) {
        return pauseForConflict(result.error);
    }
    const QByteArray oldHash = LibraryPersistence::hash(journal.oldBytes);
    const QByteArray newHash = LibraryPersistence::hash(journal.newBytes);
    if (currentHash != oldHash && currentHash != newHash) {
        result.error = QStringLiteral(
            "Save recovery found an external change in %1. The Library file was left untouched.")
                           .arg(journal.relativePath);
        return pauseForConflict(result.error);
    }

    if (!removeInterruptedStage(target, journal.id, journal.newBytes, &result.error)) {
        return pauseForConflict(result.error);
    }

    if (currentHash == oldHash) {
        mode_t mode = 0;
        if (!modeForFile(target, &mode, &result.error)) {
            return result;
        }
        bool targetConflict = false;
        if (!atomicReplace(target, journal.newBytes, mode, journal.id, &result.error,
                           {}, oldHash, &targetConflict, ioFailure)) {
            if (targetConflict) {
                return pauseForConflict(result.error);
            }
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
              QString *error, bool includeLocalArtifacts = false)
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
        if (!includeLocalArtifacts && relativeDirectory.isEmpty() &&
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
            if (!scanTree(root, relativePath, manifest, error, includeLocalArtifacts)) {
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

struct ConflictPreservation {
    QString draftPath;
    QString recoveredLibraryPath;
    QString error;
};

bool copyVerifiedLibrary(const QString &libraryRoot, const QString &destination,
                         Manifest *sourceManifest, QString *error)
{
    Manifest before;
    if (!scanTree(libraryRoot, QString(), &before, error, true)) {
        return false;
    }
    if (!QDir().mkpath(destination) ||
        ::chmod(QFile::encodeName(destination).constData(), 0700) != 0) {
        *error = systemError(QStringLiteral("Cannot create verified Library copy"), destination);
        QDir(destination).removeRecursively();
        return false;
    }

    bool copied = true;
    for (auto entry = before.cbegin(); entry != before.cend() && copied; ++entry) {
        const QString relativePath = entry.key().mid(2);
        const QString sourcePath = QDir(libraryRoot).filePath(relativePath);
        const QString destinationPath = QDir(destination).filePath(relativePath);
        if (entry.key().startsWith(QStringLiteral("D:"))) {
            if (!QDir().mkpath(destinationPath)) {
                *error = QStringLiteral("Cannot create recovered Library folder %1.")
                             .arg(destinationPath);
                copied = false;
            }
        } else {
            copied = copyFile(sourcePath, destinationPath, entry.value(), error);
        }
    }

    Manifest afterSource;
    Manifest copy;
    if (copied && !scanTree(libraryRoot, QString(), &afterSource, error, true)) {
        copied = false;
    }
    if (copied && !scanTree(destination, QString(), &copy, error, true)) {
        copied = false;
    }
    if (copied && (before != afterSource || before != copy)) {
        *error = QStringLiteral(
            "The shared Library changed while LEO verified its copy. The shared files remain untouched.");
        copied = false;
    }
    if (copied) {
        for (auto entry = before.cbegin(); entry != before.cend(); ++entry) {
            if (!entry.key().startsWith(QStringLiteral("D:"))) {
                continue;
            }
            const QString relativePath = entry.key().mid(2);
            const QString sourcePath = QDir(libraryRoot).filePath(relativePath);
            const QString destinationPath = QDir(destination).filePath(relativePath);
            if (!QFile::setPermissions(destinationPath,
                                       QFileInfo(sourcePath).permissions())) {
                *error = QStringLiteral("Cannot preserve recovered Library folder permissions: %1")
                             .arg(destinationPath);
                copied = false;
                break;
            }
        }
    }
    if (copied && !syncSnapshotDirectories(destination, QString(), error)) {
        copied = false;
    }
    if (!copied) {
        QDir(destination).removeRecursively();
        return false;
    }

    *sourceManifest = before;
    return true;
}

PersistenceResult preserveConflictChanges(const QString &libraryPath,
                                          const QVector<Journal::File> &files,
                                          const QString &reason)
{
    PersistenceResult result = failed(reason, true);
    const QString libraryRoot = canonicalDirectory(libraryPath);
    const QString recoveredDirectory = QDir(AppPaths::dataDirectory())
                                           .filePath(QStringLiteral("Recovered Libraries"));
    QString error;
    if (!ensureOutsideLibrary(recoveredDirectory, libraryRoot, &error) ||
        !ensurePrivateDirectory(recoveredDirectory, &error)) {
        result.error += QStringLiteral(" LEO could not preserve the structural draft: %1")
                            .arg(error);
        return result;
    }

    const QString operationId = newOperationId();
    const QString temporaryPath = QDir(recoveredDirectory)
                                      .filePath(QStringLiteral(".pending-") + operationId);
    const QString recoveredPath = QDir(recoveredDirectory).filePath(
        QStringLiteral("Recovered library ") +
        QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'")) +
        QLatin1Char('-') + operationId.left(8));
    Manifest sourceManifest;
    if (!copyVerifiedLibrary(libraryRoot, temporaryPath, &sourceManifest, &error)) {
        result.error += QStringLiteral(" LEO could not preserve the structural draft: %1")
                            .arg(error);
        return result;
    }

    Manifest expectedRecovered = sourceManifest;
    for (qsizetype index = 0; index < files.size(); ++index) {
        const Journal::File &file = files.at(index);
        QString target;
        bool exists = false;
        if (!resolveTarget(temporaryPath, file.relativePath, &target, &error, true, &exists)) {
            QDir(temporaryPath).removeRecursively();
            result.error += QStringLiteral(" LEO could not preserve the structural draft: %1")
                                .arg(error);
            return result;
        }
        mode_t mode = 0600;
        if (exists && !modeForFile(target, &mode, &error)) {
            QDir(temporaryPath).removeRecursively();
            result.error += QStringLiteral(" LEO could not preserve the structural draft: %1")
                                .arg(error);
            return result;
        }
        if (!atomicReplace(target, file.newBytes, mode,
                           operationId + QLatin1Char('-') + QString::number(index),
                           &error)) {
            QDir(temporaryPath).removeRecursively();
            result.error += QStringLiteral(" LEO could not preserve the structural draft: %1")
                                .arg(error);
            return result;
        }
        expectedRecovered.insert(QStringLiteral("F:") + file.relativePath,
                                 LibraryPersistence::hash(file.newBytes));
    }

    Manifest stillShared;
    Manifest recoveredManifest;
    if (!scanTree(libraryRoot, QString(), &stillShared, &error, true) ||
        !scanTree(temporaryPath, QString(), &recoveredManifest, &error, true) ||
        stillShared != sourceManifest || recoveredManifest != expectedRecovered) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The shared Library changed while LEO verified the structural draft.");
        }
        QDir(temporaryPath).removeRecursively();
        result.error += QStringLiteral(" LEO could not verify the structural draft: %1")
                            .arg(error);
        return result;
    }

    const LibraryReadResult parsedLibrary = LibraryReader::read(temporaryPath);
    if (!parsedLibrary.ok()) {
        QDir(temporaryPath).removeRecursively();
        result.error += QStringLiteral(" The structural draft could not be opened: %1")
                            .arg(parsedLibrary.error);
        return result;
    }
    if (::rename(QFile::encodeName(temporaryPath).constData(),
                 QFile::encodeName(recoveredPath).constData()) != 0) {
        error = systemError(QStringLiteral("Cannot publish Recovered library"), recoveredPath);
        QDir(temporaryPath).removeRecursively();
        result.error += QStringLiteral(" LEO could not publish the structural draft: %1")
                            .arg(error);
        return result;
    }
    if (!syncDirectory(recoveredDirectory, &error)) {
        result.error += QStringLiteral(" LEO could not flush the structural draft: %1")
                            .arg(error);
        return result;
    }
    result.recoveredLibraryPath = recoveredPath;
    result.error += QStringLiteral(
        " The structural draft is preserved in a separate Recovered Library at %1; the shared Library remains available.")
                        .arg(recoveredPath);
    return result;
}

PersistenceResult recoverBatch(const QString &directory, const QString &journalPath,
                               Journal journal, const PersistenceIoFailureHook &ioFailure,
                               bool preserveConflictDraft)
{
    const auto pauseForConflict = [&](const QString &reason) {
        if (preserveConflictDraft) {
            return preserveConflictChanges(journal.libraryPath, journal.files, reason);
        }
        PersistenceResult paused = failed(reason, true);
        return paused;
    };

    struct TargetState {
        QString path;
        bool exists = false;
        QByteArray bytes;
        QByteArray hash;
    };
    QVector<TargetState> targets;
    targets.reserve(journal.files.size());
    for (const Journal::File &file : journal.files) {
        TargetState targetState;
        QString error;
        if (!resolveTarget(journal.libraryPath, file.relativePath, &targetState.path,
                           &error, true, &targetState.exists)) {
            return pauseForConflict(error);
        }
        if (targetState.exists) {
            if (injectIoFailure(ioFailure, PersistenceIoOperation::ReadTarget, &error) ||
                !fileHash(targetState.path, &targetState.bytes, &targetState.hash, &error)) {
                return pauseForConflict(error);
            }
        }
        const QByteArray newHash = LibraryPersistence::hash(file.newBytes);
        const bool matchesOld = file.oldExists
            ? targetState.exists && targetState.hash == LibraryPersistence::hash(file.oldBytes)
            : !targetState.exists;
        const bool matchesNew = targetState.exists && targetState.hash == newHash;
        if (!matchesOld && !matchesNew) {
            return pauseForConflict(QStringLiteral(
                "Save recovery found an external change in %1. The shared file was left untouched.")
                                        .arg(file.relativePath));
        }
        const QString stageOperationId =
            journal.id + QLatin1Char('-') + QString::number(targets.size());
        if (!removeInterruptedStage(targetState.path, stageOperationId,
                                     file.newBytes, &error)) {
            return pauseForConflict(error);
        }
        targets.append(targetState);
    }

    PersistenceResult result;
    for (qsizetype index = 0; index < journal.files.size(); ++index) {
        const Journal::File &file = journal.files.at(index);
        const TargetState &target = targets.at(index);
        const QByteArray newHash = LibraryPersistence::hash(file.newBytes);
        if (target.exists && target.hash == newHash) {
            continue;
        }

        mode_t mode = 0600;
        if (file.oldExists && target.exists && !modeForFile(target.path, &mode, &result.error)) {
            return result;
        }
        bool targetConflict = false;
        if (!atomicReplace(target.path, file.newBytes, mode,
                           journal.id + QLatin1Char('-') + QString::number(index),
                           &result.error, {},
                           file.oldExists ? LibraryPersistence::hash(file.oldBytes)
                                          : QByteArray{},
                           &targetConflict, ioFailure, false, !file.oldExists)) {
            if (targetConflict) {
                return pauseForConflict(result.error);
            }
            return result;
        }
    }

    if (!finishJournal(directory, journalPath, &journal, &result.error, {}, ioFailure)) {
        return result;
    }
    result.ok = true;
    result.recovered = true;
    for (const Journal::File &file : journal.files) {
        result.savedHashes.insert(file.relativePath,
                                  LibraryPersistence::hash(file.newBytes));
    }
    return result;
}

ConflictPreservation preserveConflictDraft(const QString &libraryPath,
                                           const QString &relativePath,
                                           const QByteArray &draftBytes)
{
    ConflictPreservation result;
    const QString libraryRoot = canonicalDirectory(libraryPath);
    const QString operationId = newOperationId();
    const QString draftDirectory = QDir(AppPaths::dataDirectory())
                                      .filePath(QStringLiteral("conflict-drafts"));
    if (!ensureOutsideLibrary(draftDirectory, libraryRoot, &result.error) ||
        !ensurePrivateDirectory(draftDirectory, &result.error)) {
        return result;
    }

    const QString draftPath = QDir(draftDirectory).filePath(operationId + QStringLiteral(".html"));
    if (!atomicReplace(draftPath, draftBytes, 0600, operationId, &result.error)) {
        return result;
    }
    result.draftPath = draftPath;

    const QString recoveredDirectory = QDir(AppPaths::dataDirectory())
                                           .filePath(QStringLiteral("Recovered Libraries"));
    if (!ensureOutsideLibrary(recoveredDirectory, libraryRoot, &result.error) ||
        !ensurePrivateDirectory(recoveredDirectory, &result.error)) {
        return result;
    }

    const QString temporaryPath = QDir(recoveredDirectory)
                                      .filePath(QStringLiteral(".pending-") + operationId);
    const QString recoveredPath = QDir(recoveredDirectory).filePath(
        QStringLiteral("Recovered library ") +
        QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'")) +
        QLatin1Char('-') + operationId.left(8));

    Manifest sharedManifest;
    if (!copyVerifiedLibrary(libraryRoot, temporaryPath, &sharedManifest, &result.error)) {
        return result;
    }

    QString sourceTarget;
    QString recoveredTarget;
    if (!resolveTarget(libraryRoot, relativePath, &sourceTarget, &result.error) ||
        !resolveTarget(temporaryPath, relativePath, &recoveredTarget, &result.error)) {
        QDir(temporaryPath).removeRecursively();
        return result;
    }

    QByteArray sharedBytes;
    QByteArray sharedHash;
    if (!fileHash(sourceTarget, &sharedBytes, &sharedHash, &result.error) ||
        sharedManifest.value(QStringLiteral("F:") + relativePath) != sharedHash) {
        if (result.error.isEmpty()) {
            result.error = QStringLiteral(
                "The shared chapter changed during conflict recovery. The shared files remain untouched.");
        }
        QDir(temporaryPath).removeRecursively();
        return result;
    }

    mode_t mode = 0;
    if (!modeForFile(recoveredTarget, &mode, &result.error)) {
        QDir(temporaryPath).removeRecursively();
        return result;
    }
    if (!atomicReplace(recoveredTarget, draftBytes, mode, operationId, &result.error,
                       {}, sharedHash)) {
        QDir(temporaryPath).removeRecursively();
        return result;
    }

    Manifest expectedRecoveredManifest = sharedManifest;
    expectedRecoveredManifest.insert(QStringLiteral("F:") + relativePath,
                                     LibraryPersistence::hash(draftBytes));
    Manifest recoveredManifest;
    Manifest stillSharedManifest;
    if (!scanTree(temporaryPath, QString(), &recoveredManifest, &result.error, true) ||
        recoveredManifest != expectedRecoveredManifest ||
        !scanTree(libraryRoot, QString(), &stillSharedManifest, &result.error, true) ||
        stillSharedManifest != sharedManifest) {
        if (result.error.isEmpty()) {
            result.error = QStringLiteral(
                "LEO could not verify the Recovered library against the shared copy. The shared files remain untouched.");
        }
        QDir(temporaryPath).removeRecursively();
        return result;
    }

    const LibraryReadResult parsedLibrary = LibraryReader::read(temporaryPath);
    if (!parsedLibrary.ok()) {
        result.error = QStringLiteral("The shared copy could not be opened as a Library: %1")
                           .arg(parsedLibrary.error);
        QDir(temporaryPath).removeRecursively();
        return result;
    }

    const QByteArray nativeTemporary = QFile::encodeName(temporaryPath);
    const QByteArray nativeRecovered = QFile::encodeName(recoveredPath);
    if (::rename(nativeTemporary.constData(), nativeRecovered.constData()) != 0) {
        result.error = systemError(QStringLiteral("Cannot publish Recovered library"), recoveredPath);
        QDir(temporaryPath).removeRecursively();
        return result;
    }
    if (!syncDirectory(recoveredDirectory, &result.error)) {
        return result;
    }
    result.recoveredLibraryPath = recoveredPath;
    return result;
}

PersistenceResult failed(const QString &error, bool conflict)
{
    PersistenceResult result;
    result.error = error;
    result.conflict = conflict;
    return result;
}

PersistenceResult conflictWithPreservedDraft(const QString &libraryPath,
                                             const QString &relativePath,
                                             const QByteArray &draftBytes,
                                             const QString &reason)
{
    const ConflictPreservation preservation =
        preserveConflictDraft(libraryPath, relativePath, draftBytes);
    PersistenceResult result = failed(reason, true);
    result.conflictDraftPath = preservation.draftPath;
    result.recoveredLibraryPath = preservation.recoveredLibraryPath;
    if (preservation.draftPath.isEmpty()) {
        result.error += QStringLiteral(
            " LEO could not preserve the local draft outside the shared Library: %1")
                            .arg(preservation.error);
    } else {
        result.error += QStringLiteral(" Local draft preserved at %1.")
                            .arg(preservation.draftPath);
        if (!preservation.recoveredLibraryPath.isEmpty()) {
            result.error += QStringLiteral(
                " Recovered library created at %1. The shared Library remains unchanged.")
                                .arg(preservation.recoveredLibraryPath);
        } else if (!preservation.error.isEmpty()) {
            result.error += QStringLiteral(" LEO could not build a Recovered library: %1")
                                .arg(preservation.error);
        }
    }
    return result;
}

PersistenceResult recoverPendingSavesInternal(
    const QString &libraryPath, const PersistenceIoFailureHook &ioFailure,
    bool preserveConflicts)
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
        const PersistenceResult result = recoverOne(directory, entry.filePath(), journal,
                                                    ioFailure, preserveConflicts);
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
    return recoverPendingSavesInternal(libraryPath, ioFailure, true);
}

PersistenceResult LibraryPersistence::updateConflictDraft(
    const QString &libraryPath, const QString &relativePath,
    const QString &draftPath, const QString &recoveredLibraryPath,
    const QByteArray &draftBytes)
{
    const QString libraryRoot = canonicalDirectory(libraryPath);
    const QString draftDirectory = QDir(AppPaths::dataDirectory())
                                      .filePath(QStringLiteral("conflict-drafts"));
    const QString canonicalDraftDirectory = canonicalDirectory(draftDirectory);
    const QString canonicalDraftPath = cleanAbsolutePath(draftPath);
    PersistenceResult result;
    result.conflict = true;
    result.conflictDraftPath = draftPath;
    if (draftPath.isEmpty()) {
        const ConflictPreservation preservation =
            preserveConflictDraft(libraryRoot, relativePath, draftBytes);
        result.conflictDraftPath = preservation.draftPath;
        result.recoveredLibraryPath = preservation.recoveredLibraryPath;
        result.ok = !preservation.draftPath.isEmpty();
        result.error = preservation.error;
        return result;
    }
    if (!ensureOutsideLibrary(draftDirectory, libraryRoot, &result.error) ||
        !ensurePrivateDirectory(draftDirectory, &result.error) ||
        !isWithin(canonicalDraftDirectory, canonicalDraftPath) ||
        canonicalDraftPath == canonicalDraftDirectory ||
        QFileInfo(canonicalDraftPath).isSymLink() ||
        !QFileInfo(canonicalDraftPath).isFile()) {
        if (result.error.isEmpty()) {
            result.error = QStringLiteral("The private local draft could not be updated safely.");
        }
        return result;
    }

    const QString operationId = newOperationId();
    if (!atomicReplace(canonicalDraftPath, draftBytes, 0600, operationId,
                       &result.error)) {
        return result;
    }
    result.ok = true;

    if (recoveredLibraryPath.isEmpty()) {
        return result;
    }

    const QString recoveredDirectory = QDir(AppPaths::dataDirectory())
                                           .filePath(QStringLiteral("Recovered Libraries"));
    const QString canonicalRecoveredDirectory = canonicalDirectory(recoveredDirectory);
    const QString canonicalRecoveredPath = cleanAbsolutePath(recoveredLibraryPath);
    QString recoveryError;
    if (!ensureOutsideLibrary(recoveredDirectory, libraryRoot, &recoveryError) ||
        !ensurePrivateDirectory(recoveredDirectory, &recoveryError) ||
        !isWithin(canonicalRecoveredDirectory, canonicalRecoveredPath) ||
        canonicalRecoveredPath == canonicalRecoveredDirectory ||
        QFileInfo(canonicalRecoveredPath).isSymLink() ||
        !QFileInfo(canonicalRecoveredPath).isDir()) {
        if (recoveryError.isEmpty()) {
            recoveryError = QStringLiteral("The Recovered library path is not safe.");
        }
    } else {
        const LibraryReadResult currentLibrary = LibraryReader::read(canonicalRecoveredPath);
        Manifest before;
        QString target;
        QByteArray currentBytes;
        QByteArray currentHash;
        mode_t mode = 0;
        bool updated = currentLibrary.ok() &&
                       scanTree(canonicalRecoveredPath, QString(), &before,
                                &recoveryError, true) &&
                       resolveTarget(canonicalRecoveredPath, relativePath, &target,
                                     &recoveryError) &&
                       fileHash(target, &currentBytes, &currentHash, &recoveryError) &&
                       modeForFile(target, &mode, &recoveryError);
        if (!currentLibrary.ok() && recoveryError.isEmpty()) {
            recoveryError = QStringLiteral("The Recovered library could not be read: %1")
                                .arg(currentLibrary.error);
        }
        if (updated) {
            bool targetConflict = false;
            updated = atomicReplace(target, draftBytes, mode, operationId,
                                    &recoveryError, {}, currentHash,
                                    &targetConflict);
            if (targetConflict && recoveryError.isEmpty()) {
                recoveryError = QStringLiteral(
                    "The Recovered chapter changed while LEO updated the local draft.");
            }
        }
        if (updated) {
            Manifest after;
            Manifest expected = before;
            expected.insert(QStringLiteral("F:") + relativePath,
                            LibraryPersistence::hash(draftBytes));
            updated = scanTree(canonicalRecoveredPath, QString(), &after,
                               &recoveryError, true) && after == expected;
            if (!updated && recoveryError.isEmpty()) {
                recoveryError = QStringLiteral(
                    "The Recovered library changed while LEO verified the local draft.");
            }
        }
        if (updated) {
            const LibraryReadResult verifiedLibrary = LibraryReader::read(canonicalRecoveredPath);
            updated = verifiedLibrary.ok();
            if (!updated) {
                recoveryError = QStringLiteral("The updated Recovered library could not be read: %1")
                                    .arg(verifiedLibrary.error);
            }
        }
        if (updated) {
            result.recoveredLibraryPath = canonicalRecoveredPath;
            return result;
        }
    }

    result.error = QStringLiteral(
        "The local draft is saved outside the shared Library at %1. The Recovered library could not be updated: %2")
                       .arg(draftPath, recoveryError);
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
    const PersistenceResult recovery =
        recoverPendingSavesInternal(libraryRoot, ioFailure, false);
    if (!recovery.ok) {
        PersistenceResult result = conflictWithPreservedDraft(
            libraryRoot, relativePath, newBytes, recovery.error);
        result.recovered = recovery.recovered;
        return result;
    }

    QString target;
    QString error;
    if (!resolveTarget(libraryRoot, relativePath, &target, &error)) {
        return conflictWithPreservedDraft(libraryRoot, relativePath, newBytes, error);
    }

    QByteArray currentBytes;
    QByteArray currentHash;
    if (!fileHash(target, &currentBytes, &currentHash, &error)) {
        return conflictWithPreservedDraft(libraryRoot, relativePath, newBytes, error);
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
        return conflictWithPreservedDraft(
            libraryRoot, relativePath, newBytes,
            QStringLiteral("This chapter changed outside LEO. The current file was left untouched."));
    }

    if (!ensureSafetySnapshot(libraryRoot, &error)) {
        return conflictWithPreservedDraft(libraryRoot, relativePath, newBytes, error);
    }

    if (!fileHash(target, &currentBytes, &currentHash, &error)) {
        return conflictWithPreservedDraft(libraryRoot, relativePath, newBytes, error);
    }
    if (currentHash != expectedHash) {
        return conflictWithPreservedDraft(
            libraryRoot, relativePath, newBytes,
            QStringLiteral("This chapter changed while LEO prepared its safety snapshot. "
                           "The current file was left untouched."));
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
        return conflictWithPreservedDraft(libraryRoot, relativePath, newBytes, error);
    }
    if (currentHash != expectedHash) {
        return conflictWithPreservedDraft(
            libraryRoot, relativePath, newBytes,
            QStringLiteral("This chapter changed before atomic replacement. "
                           "The local draft remains in the private save journal."));
    }

    mode_t mode = 0;
    if (!modeForFile(target, &mode, &error)) {
        return failed(error);
    }
    bool targetConflict = false;
    if (!atomicReplace(target, newBytes, mode, journal.id, &error,
                       checkpoint, expectedHash, &targetConflict, ioFailure)) {
        if (targetConflict) {
            return conflictWithPreservedDraft(libraryRoot, relativePath, newBytes, error);
        }
        return failed(error);
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

PersistenceResult LibraryPersistence::saveFiles(
    const QString &libraryPath, const QVector<PersistenceFileChange> &changes,
    const PersistenceCheckpointHook &checkpoint,
    const PersistenceIoFailureHook &ioFailure)
{
    if (changes.isEmpty()) {
        return failed(QStringLiteral("A Library transaction needs at least one file."));
    }

    const QString libraryRoot = canonicalDirectory(libraryPath);
    const PersistenceResult recovery =
        recoverPendingSavesInternal(libraryRoot, ioFailure, false);
    QVector<Journal::File> intendedFiles;
    intendedFiles.reserve(changes.size());
    for (const PersistenceFileChange &change : changes) {
        Journal::File file;
        file.relativePath = change.relativePath;
        file.newBytes = change.newBytes;
        intendedFiles.append(file);
    }
    if (!recovery.ok) {
        return preserveConflictChanges(
            libraryRoot, intendedFiles,
            QStringLiteral("LEO paused this structural edit while recovering an earlier save: %1")
                .arg(recovery.error));
    }

    QSet<QString> paths;
    Journal journal;
    journal.id = newOperationId();
    journal.libraryPath = libraryRoot;
    journal.state = QStringLiteral("prepared");
    journal.files.reserve(changes.size());
    QString error;
    bool hasChanges = false;
    for (const PersistenceFileChange &change : changes) {
        if (paths.contains(change.relativePath) ||
            (change.expectedAbsent && !change.expectedHash.isEmpty()) ||
            (!change.expectedAbsent && change.expectedHash.isEmpty())) {
            return failed(QStringLiteral(
                "The Library transaction has a duplicate path or invalid expected file state."));
        }
        paths.insert(change.relativePath);

        QString target;
        bool exists = false;
        if (!resolveTarget(libraryRoot, change.relativePath, &target, &error, true, &exists)) {
            return failed(error);
        }
        QByteArray oldBytes;
        QByteArray oldHash;
        if (exists) {
            if (!fileHash(target, &oldBytes, &oldHash, &error)) {
                return failed(error);
            }
        }
        const bool expectedStateMatches = change.expectedAbsent
            ? !exists
            : exists && oldHash == change.expectedHash;
        if (!expectedStateMatches) {
            return preserveConflictChanges(
                libraryRoot, intendedFiles,
                QStringLiteral("The Library changed before this structural edit could be saved. "
                               "The shared files were left untouched."));
        }

        Journal::File file;
        file.relativePath = change.relativePath;
        file.oldExists = exists;
        file.oldBytes = oldBytes;
        file.newBytes = change.newBytes;
        hasChanges = hasChanges || !exists || oldBytes != change.newBytes;
        journal.files.append(file);
    }

    if (!hasChanges) {
        PersistenceResult result;
        result.ok = true;
        for (const Journal::File &file : journal.files) {
            result.savedHashes.insert(file.relativePath,
                                      LibraryPersistence::hash(file.newBytes));
        }
        return result;
    }

    if (!ensureSafetySnapshot(libraryRoot, &error)) {
        return failed(error);
    }
    for (const Journal::File &file : journal.files) {
        QString target;
        bool exists = false;
        if (!resolveTarget(libraryRoot, file.relativePath, &target, &error, true, &exists)) {
            return failed(error);
        }
        QByteArray currentBytes;
        if (exists && !readFile(target, &currentBytes, &error)) {
            return failed(error);
        }
        if (exists != file.oldExists || currentBytes != file.oldBytes) {
            return preserveConflictChanges(
                libraryRoot, intendedFiles,
                QStringLiteral("The Library changed while LEO prepared a safety snapshot. "
                               "The shared files were left untouched."));
        }
    }

    QString journalPath;
    const QString privateJournalDirectory = journalDirectory();
    if (!ensureOutsideLibrary(privateJournalDirectory, libraryRoot, &error) ||
        !writeJournal(privateJournalDirectory, journal, &journalPath, &error, ioFailure)) {
        return failed(error);
    }

    for (qsizetype index = 0; index < journal.files.size(); ++index) {
        const Journal::File &file = journal.files.at(index);
        if (file.oldExists && file.oldBytes == file.newBytes) {
            continue;
        }
        QString target;
        if (!resolveTarget(libraryRoot, file.relativePath, &target, &error, true)) {
            return preserveConflictChanges(libraryRoot, journal.files, error);
        }
        mode_t mode = 0600;
        if (file.oldExists && !modeForFile(target, &mode, &error)) {
            return failed(error);
        }
        bool targetConflict = false;
        if (!atomicReplace(
                target, file.newBytes, mode,
                journal.id + QLatin1Char('-') + QString::number(index), &error, checkpoint,
                file.oldExists ? LibraryPersistence::hash(file.oldBytes) : QByteArray{},
                &targetConflict, ioFailure, false, !file.oldExists)) {
            if (targetConflict) {
                return preserveConflictChanges(
                    libraryRoot, journal.files,
                    QStringLiteral("The Library changed during a structural edit. "
                                   "The shared files were left untouched. %1")
                        .arg(error));
            }
            return failed(error);
        }
    }

    if (!finishJournal(privateJournalDirectory, journalPath, &journal, &error,
                       checkpoint, ioFailure)) {
        return failed(error);
    }

    PersistenceResult result;
    result.ok = true;
    for (const Journal::File &file : journal.files) {
        result.savedHashes.insert(file.relativePath,
                                  LibraryPersistence::hash(file.newBytes));
    }
    const auto bookHash = result.savedHashes.constFind(QStringLiteral("book.json"));
    if (bookHash != result.savedHashes.cend()) {
        result.savedHash = bookHash.value();
    }
    return result;
}
