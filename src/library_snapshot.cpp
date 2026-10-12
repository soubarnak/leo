#include "library_snapshot.h"

#include "app_paths.h"
#include "library_reader.h"
#include "library_snapshot_internal.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <algorithm>

using namespace LibrarySnapshotInternal;

namespace {

const QString dateFormat = QStringLiteral("yyyy-MM-dd");

QString canonicalRoot(const QString &libraryRoot)
{
    return canonicalDirectory(libraryRoot);
}

QString snapshotsBase()
{
    return QDir(AppPaths::dataDirectory()).filePath(QStringLiteral("daily-snapshots"));
}

bool isDayName(const QString &name)
{
    static const QRegularExpression pattern(QStringLiteral("^\\d{4}-\\d{2}-\\d{2}$"));
    return pattern.match(name).hasMatch() && QDate::fromString(name, dateFormat).isValid();
}

// "<yyyy-MM-dd>.invalid-<stamp>": a day folder that failed verification and was set aside.
bool isSetAsideName(const QString &name)
{
    return name.size() > 10 && isDayName(name.left(10)) &&
           name.mid(10).startsWith(QStringLiteral(".invalid-"));
}

// Renames a failed day folder out of the way (kept for inspection, never counted or pruned).
QString setAside(const QString &dayPath)
{
    const QString aside = dayPath + QStringLiteral(".invalid-") +
                          QDateTime::currentDateTimeUtc().toString(QStringLiteral("HHmmsszzz"));
    return QDir().rename(dayPath, aside) ? aside : QString();
}

bool readBytes(const QString &path, QByteArray *bytes, QString *error)
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

QByteArray manifestJson(const Manifest &manifest, const QString &date)
{
    QJsonArray entries;
    for (auto entry = manifest.cbegin(); entry != manifest.cend(); ++entry) {
        entries.append(QJsonObject{{QStringLiteral("key"), entry.key()},
                                   {QStringLiteral("sha256"), QString::fromLatin1(entry.value())}});
    }
    return QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
                                     {QStringLiteral("date"), date},
                                     {QStringLiteral("manifestHash"),
                                      QString::fromLatin1(manifestHash(manifest))},
                                     {QStringLiteral("entries"), entries}})
        .toJson(QJsonDocument::Compact);
}

bool parseManifest(const QString &snapshotPath, Manifest *manifest, QString *date,
                   QByteArray *storedHash, QString *error)
{
    QByteArray bytes;
    if (!readBytes(QDir(snapshotPath).filePath(QStringLiteral("manifest.json")), &bytes, error)) {
        return false;
    }
    const QJsonDocument document = QJsonDocument::fromJson(bytes);
    if (!document.isObject()) {
        *error = QStringLiteral("Snapshot manifest is not valid: %1").arg(snapshotPath);
        return false;
    }
    const QJsonObject object = document.object();
    const QJsonValue entries = object.value(QStringLiteral("entries"));
    if (!entries.isArray()) {
        *error = QStringLiteral("Snapshot manifest has no entries: %1").arg(snapshotPath);
        return false;
    }
    manifest->clear();
    for (const QJsonValue &value : entries.toArray()) {
        const QJsonObject entry = value.toObject();
        const QString key = entry.value(QStringLiteral("key")).toString();
        if (!key.startsWith(QStringLiteral("D:")) && !key.startsWith(QStringLiteral("F:"))) {
            *error = QStringLiteral("Snapshot manifest entry is not valid: %1").arg(snapshotPath);
            return false;
        }
        manifest->insert(key, entry.value(QStringLiteral("sha256")).toString().toLatin1());
    }
    *date = object.value(QStringLiteral("date")).toString();
    *storedHash = object.value(QStringLiteral("manifestHash")).toString().toLatin1();
    return true;
}

bool readLatest(const QString &bucket, QString *date, QByteArray *hash, QByteArray *manifestSha)
{
    QByteArray bytes;
    QString ignored;
    if (!readBytes(QDir(bucket).filePath(QStringLiteral("latest.json")), &bytes, &ignored)) {
        return false;
    }
    const QJsonObject object = QJsonDocument::fromJson(bytes).object();
    *date = object.value(QStringLiteral("date")).toString();
    *hash = object.value(QStringLiteral("manifestHash")).toString().toLatin1();
    *manifestSha = object.value(QStringLiteral("manifestSha256")).toString().toLatin1();
    return !date->isEmpty() && !hash->isEmpty() && !manifestSha->isEmpty();
}

QString configuredDrive(const QString &hash)
{
    QByteArray bytes;
    QString ignored;
    if (!readBytes(QDir(AppPaths::configDirectory()).filePath(QStringLiteral("snapshot-drives.json")),
                   &bytes, &ignored)) {
        return {};
    }
    return QJsonDocument::fromJson(bytes).object().value(hash).toString();
}

// Copies the finished local day folder to the optional drive and verifies it there.
// Returns true when no drive is configured or a verified copy exists afterwards.
bool mirrorToDrive(const QString &root, const QString &hash, const QString &date,
                   const QString &localDay, QString *error)
{
    const QString drive = configuredDrive(hash);
    if (drive.isEmpty()) {
        return true;
    }
    if (!QDir::isAbsolutePath(drive) || !QFileInfo(drive).isDir() || !QFileInfo(drive).isWritable()) {
        *error = QStringLiteral("Backup drive is not available: %1").arg(drive);
        return false;
    }
    if (isWithin(root, canonicalDirectory(drive))) {
        *error = QStringLiteral("Backup drive folder is inside the Library: %1").arg(drive);
        return false;
    }
    const QString parent = QDir(drive).filePath(QStringLiteral("leo-snapshots/") + hash);
    const QString finalPath = QDir(parent).filePath(date);
    QString verifyError;
    if (QFileInfo::exists(finalPath)) {
        if (LibrarySnapshots::verifySnapshot(finalPath, &verifyError)) {
            return true;
        }
        // A damaged drive copy is set aside (kept, never deleted) and replaced from the local archive.
        if (setAside(finalPath).isEmpty()) {
            *error = QStringLiteral("Existing backup drive copy is not valid and could not be "
                                    "set aside: %1").arg(verifyError);
            return false;
        }
    }
    if (!QDir().mkpath(parent)) {
        *error = QStringLiteral("Cannot create backup drive folder: %1").arg(parent);
        return false;
    }
    const QString pending = QDir(parent).filePath(QStringLiteral(".pending-") + newOperationId());
    if (!QDir().mkpath(pending)) {
        *error = QStringLiteral("Cannot create backup drive folder: %1").arg(pending);
        return false;
    }
    // Per-file fsync (inside the verified copy) plus a directory fsync before publishing.
    Manifest copied;
    if (!copyVerifiedTree(localDay, pending, &copied, error, true) ||
        !syncDirectory(pending, error)) {
        QDir(pending).removeRecursively();
        return false;
    }
    if (!QDir().rename(pending, finalPath)) {
        QDir(pending).removeRecursively();
        *error = QStringLiteral("Cannot publish backup drive copy: %1").arg(finalPath);
        return false;
    }
    if (!syncDirectory(parent, error)) {
        return false;
    }
    if (!LibrarySnapshots::verifySnapshot(finalPath, &verifyError)) {
        setAside(finalPath);
        *error = QStringLiteral("Backup drive copy failed verification: %1").arg(verifyError);
        return false;
    }
    return true;
}

}

QString LibrarySnapshots::libraryHash(const QString &libraryRoot)
{
    return QString::fromLatin1(QCryptographicHash::hash(canonicalRoot(libraryRoot).toUtf8(),
                                                        QCryptographicHash::Sha256)
                                   .toHex());
}

QString LibrarySnapshots::snapshotBucket(const QString &libraryRoot)
{
    return QDir(snapshotsBase()).filePath(libraryHash(libraryRoot));
}

bool LibrarySnapshots::verifySnapshot(const QString &snapshotPath, QString *error)
{
    const QFileInfo dayInfo(snapshotPath);
    if (!dayInfo.isDir() || dayInfo.isSymLink()) {
        *error = QStringLiteral("Snapshot folder is missing: %1").arg(snapshotPath);
        return false;
    }
    Manifest expected;
    QString date;
    QByteArray storedHash;
    if (!parseManifest(snapshotPath, &expected, &date, &storedHash, error)) {
        return false;
    }
    if (storedHash != manifestHash(expected)) {
        *error = QStringLiteral("Snapshot manifest hash does not match: %1").arg(snapshotPath);
        return false;
    }
    const QString libraryPath = QDir(snapshotPath).filePath(QStringLiteral("library"));
    if (!QFileInfo(libraryPath).isDir() || QFileInfo(libraryPath).isSymLink()) {
        *error = QStringLiteral("Snapshot has no library folder: %1").arg(snapshotPath);
        return false;
    }
    Manifest actual;
    if (!scanTree(libraryPath, &actual, error, true)) {
        return false;
    }
    if (actual != expected) {
        *error = QStringLiteral("Snapshot files do not match their manifest: %1").arg(snapshotPath);
        return false;
    }
    return true;
}

QList<LibrarySnapshotInfo> LibrarySnapshots::listSnapshots(const QString &libraryRoot,
                                                           QStringList *invalid)
{
    QList<LibrarySnapshotInfo> snapshots;
    const QDir bucket(snapshotBucket(libraryRoot));
    const QStringList names = bucket.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &name : names) {
        if (!isDayName(name)) {
            if (invalid && isSetAsideName(name)) {
                invalid->append(bucket.filePath(name));
            }
            continue;
        }
        const QString path = bucket.filePath(name);
        QString error;
        if (verifySnapshot(path, &error)) {
            snapshots.append({QDate::fromString(name, dateFormat), path});
        } else if (invalid) {
            invalid->append(path);
        }
    }
    std::sort(snapshots.begin(), snapshots.end(),
              [](const LibrarySnapshotInfo &a, const LibrarySnapshotInfo &b) {
                  return a.date > b.date;
              });
    return snapshots;
}

namespace {

QByteArray manifestFileHash(const QString &dayPath)
{
    QByteArray bytes;
    QString ignored;
    if (!readBytes(QDir(dayPath).filePath(QStringLiteral("manifest.json")), &bytes, &ignored)) {
        return {};
    }
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

// The newest day folder name already in the store, valid or not (including renamed invalid ones).
QDate newestStoredDay(const QString &bucket)
{
    QDate newest;
    const QStringList names = QDir(bucket).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &name : names) {
        const QDate date = QDate::fromString(name.left(10), dateFormat);
        if (date.isValid() && isDayName(name.left(10)) && (!newest.isValid() || date > newest)) {
            newest = date;
        }
    }
    return newest;
}

// Local half of a daily snapshot. Never touches the optional drive.
LibrarySnapshotResult snapshotLocal(const QString &libraryRoot, const QDateTime &nowUtc,
                                    const std::function<void()> &afterCopy)
{
    LibrarySnapshotResult result;
    const QString root = canonicalRoot(libraryRoot);
    const QString base = snapshotsBase();
    const QString bucket = QDir(base).filePath(LibrarySnapshots::libraryHash(libraryRoot));
    QString error;
    if (!ensureOutsideLibrary(base, root, &error) || !ensurePrivateDirectory(base, &error) ||
        !ensurePrivateDirectory(bucket, &error)) {
        result.error = error;
        return result;
    }

    const QDate todayDate = nowUtc.toUTC().date();
    const QString today = todayDate.toString(dateFormat);
    // A folder dated after today (bogus date or a clock that stepped back) must not stop
    // archiving: today's snapshot is still made, but pruning stays off and a warning is shown.
    const QDate newest = newestStoredDay(bucket);
    if (newest.isValid() && todayDate < newest) {
        result.warning = QStringLiteral(
            "A snapshot is dated %1, after today (%2). Check the clock. Old snapshots are not pruned.")
                             .arg(newest.toString(dateFormat), today);
    }

    Manifest live;
    if (!scanTree(root, &live, &error, false)) {
        result.error = error;
        return result;
    }
    const QByteArray liveHash = manifestHash(live);
    const QString finalPath = QDir(bucket).filePath(today);

    QString latestDate;
    QByteArray latestHash;
    QByteArray latestManifestSha;
    if (readLatest(bucket, &latestDate, &latestHash, &latestManifestSha) &&
        latestHash == liveHash && QFileInfo(QDir(bucket).filePath(latestDate)).isDir() &&
        manifestFileHash(QDir(bucket).filePath(latestDate)) == latestManifestSha) {
        result.skipped = true;
        result.snapshotPath = QDir(bucket).filePath(latestDate);
        return result;
    }
    if (QFileInfo::exists(finalPath)) {
        QString verifyError;
        if (LibrarySnapshots::verifySnapshot(finalPath, &verifyError)) {
            // One archive per UTC day. Later edits are captured by the next day's snapshot.
            result.skipped = true;
            result.snapshotPath = finalPath;
            return result;
        }
        // A broken archive for today is set aside (kept, never counted or pruned).
        const QString aside = setAside(finalPath);
        if (aside.isEmpty()) {
            result.error = QStringLiteral("Today's snapshot failed verification and could not be "
                                          "set aside: %1").arg(verifyError);
            return result;
        }
        result.invalidSnapshots.append(aside);
    }

    const QString pending = QDir(bucket).filePath(QStringLiteral(".pending-") + newOperationId());
    if (!QDir().mkpath(pending) || !QFile::setPermissions(pending, QFileDevice::ReadOwner |
                                                          QFileDevice::WriteOwner |
                                                          QFileDevice::ExeOwner)) {
        result.error = QStringLiteral("Cannot create snapshot folder %1.").arg(pending);
        QDir(pending).removeRecursively();
        return result;
    }
    // Per-file fsync happens inside the verified copy; manifest.json is written atomically and
    // the staging folder is fsynced before it is published.
    Manifest copied;
    if (!copyVerifiedTree(root, QDir(pending).filePath(QStringLiteral("library")), &copied,
                          &error, false, afterCopy)) {
        result.error = error;
        QDir(pending).removeRecursively();
        return result;
    }
    const QByteArray manifestBytes = manifestJson(copied, today);
    if (!writeFileAtomic(QDir(pending).filePath(QStringLiteral("manifest.json")), manifestBytes,
                         &error) ||
        !syncDirectory(pending, &error)) {
        result.error = error;
        QDir(pending).removeRecursively();
        return result;
    }
    if (!QDir().rename(pending, finalPath)) {
        result.error = QStringLiteral("Cannot publish snapshot %1.").arg(finalPath);
        QDir(pending).removeRecursively();
        return result;
    }
    if (!syncDirectory(bucket, &error)) {
        result.error = error;
        return result;
    }
    result.created = true;
    result.snapshotPath = finalPath;

    const QByteArray latest = QJsonDocument(QJsonObject{
        {QStringLiteral("date"), today},
        {QStringLiteral("manifestHash"), QString::fromLatin1(manifestHash(copied))},
        {QStringLiteral("manifestSha256"),
         QString::fromLatin1(QCryptographicHash::hash(manifestBytes, QCryptographicHash::Sha256).toHex())}})
                                  .toJson(QJsonDocument::Compact);
    if (!writeFileAtomic(QDir(bucket).filePath(QStringLiteral("latest.json")), latest, &error)) {
        result.error = QStringLiteral("Snapshot saved but its index could not be updated: %1")
                           .arg(error);
    }

    // Retention: drop interrupted builds, keep the newest valid days, never touch invalid ones.
    // The new day is the newest stored day (clock check above), so it is always kept.
    const QStringList leftovers = QDir(bucket).entryList(
        QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &name : leftovers) {
        if (name.startsWith(QStringLiteral(".pending-"))) {
            QDir(QDir(bucket).filePath(name)).removeRecursively();
        }
    }
    QStringList invalidNow;
    const QList<LibrarySnapshotInfo> valid = LibrarySnapshots::listSnapshots(libraryRoot, &invalidNow);
    for (const QString &path : std::as_const(invalidNow)) {
        // Failed day folders are renamed so every later tick reports them by name alone.
        const QString aside = isSetAsideName(QFileInfo(path).fileName()) ? path : setAside(path);
        result.invalidSnapshots.append(aside.isEmpty() ? path : aside);
    }
    result.invalidSnapshots.removeDuplicates();
    // Prune only when the store looks sane: nothing dated after today, nothing failing
    // verification (a bad signal must never cost a valid archive).
    const QDate newestNow = newestStoredDay(bucket);
    if (!result.invalidSnapshots.isEmpty() || (newestNow.isValid() && newestNow > todayDate)) {
        result.warning = QStringLiteral(
            "Old snapshots were not pruned because the snapshot folder has invalid or future-dated entries.");
    } else {
        for (qsizetype index = LibrarySnapshots::retainedSnapshots; index < valid.size(); ++index) {
            if (valid.at(index).path != finalPath) {
                QDir(valid.at(index).path).removeRecursively();
            }
        }
    }
    return result;
}

}

LibrarySnapshotResult LibrarySnapshots::maybeSnapshotLocal(
    const QString &libraryRoot, const QDateTime &nowUtc, const std::function<void()> &afterCopy)
{
    LibrarySnapshotResult result = snapshotLocal(libraryRoot, nowUtc, afterCopy);

    // Set-aside folders are reported on every call (name check only, no hashing).
    const QString bucketPath = snapshotBucket(libraryRoot);
    const QStringList names = QDir(bucketPath).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &name : names) {
        if (isSetAsideName(name)) {
            result.invalidSnapshots.append(QDir(bucketPath).filePath(name));
        }
    }
    result.invalidSnapshots.removeDuplicates();
    return result;
}

bool LibrarySnapshots::driveConfigured(const QString &libraryRoot)
{
    return !configuredDrive(libraryHash(libraryRoot)).isEmpty();
}

QString LibrarySnapshots::mirrorLatestToDrive(const QString &libraryRoot)
{
    // Checked on every call: the latest valid local archive must be mirrored and verified on
    // the drive, whether or not the last call created a new local archive.
    const QString hash = libraryHash(libraryRoot);
    const QString bucket = QDir(snapshotsBase()).filePath(hash);
    QString latestDate;
    QByteArray latestHash;
    QByteArray latestManifestSha;
    QString error;
    if (!configuredDrive(hash).isEmpty() &&
        readLatest(bucket, &latestDate, &latestHash, &latestManifestSha)) {
        const QString latestPath = QDir(bucket).filePath(latestDate);
        if (QFileInfo(latestPath).isDir() && manifestFileHash(latestPath) == latestManifestSha) {
            mirrorToDrive(canonicalRoot(libraryRoot), hash, latestDate, latestPath, &error);
        }
    }
    return error;
}

LibrarySnapshotResult LibrarySnapshots::maybeSnapshotDaily(
    const QString &libraryRoot, const QDateTime &nowUtc, const std::function<void()> &afterCopy)
{
    LibrarySnapshotResult result = maybeSnapshotLocal(libraryRoot, nowUtc, afterCopy);
    result.driveError = mirrorLatestToDrive(libraryRoot);
    return result;
}

bool LibrarySnapshots::restoreToRecoveredLibrary(const QString &libraryRoot,
                                                 const QString &snapshotPath,
                                                 const QDateTime &nowUtc, QString *recoveredPath,
                                                 QString *error)
{
    recoveredPath->clear();
    const QString root = canonicalRoot(libraryRoot);
    const QString bucket = snapshotBucket(libraryRoot);
    const QString dayPath = QDir::cleanPath(snapshotPath);
    if (QFileInfo(dayPath).dir().absolutePath() != QDir::cleanPath(bucket) ||
        !isDayName(QFileInfo(dayPath).fileName())) {
        *error = QStringLiteral("That is not a snapshot of this Library.");
        return false;
    }

    // The safety snapshot comes first so a restore can always be walked back.
    QString safetyPath;
    if (!createSafetySnapshot(root, &safetyPath, error)) {
        *error = QStringLiteral("Could not verify a safety snapshot first: %1").arg(*error);
        return false;
    }
    if (!verifySnapshot(dayPath, error)) {
        return false;
    }
    Manifest expected;
    QString date;
    QByteArray storedHash;
    if (!parseManifest(dayPath, &expected, &date, &storedHash, error)) {
        return false;
    }

    const QString recoveredBase = QDir(AppPaths::dataDirectory())
                                      .filePath(QStringLiteral("Recovered Libraries"));
    if (!ensureOutsideLibrary(recoveredBase, root, error) ||
        !ensurePrivateDirectory(recoveredBase, error)) {
        return false;
    }
    const QString operationId = newOperationId();
    const QString pending = QDir(recoveredBase).filePath(QStringLiteral(".pending-") + operationId);
    const QString finalPath = QDir(recoveredBase).filePath(
        QStringLiteral("Recovered library ") +
        nowUtc.toUTC().toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'")) + QLatin1Char('-') +
        operationId.left(8));
    Manifest copied;
    if (!copyVerifiedTree(QDir(dayPath).filePath(QStringLiteral("library")), pending, &copied,
                          error, true)) {
        return false;
    }
    Manifest check;
    if (copied != expected || !scanTree(pending, &check, error, true) || check != expected) {
        if (error->isEmpty()) {
            *error = QStringLiteral("The recovered copy does not match the snapshot.");
        }
        QDir(pending).removeRecursively();
        return false;
    }
    const LibraryReadResult opened = LibraryReader::read(pending);
    if (!opened.ok()) {
        *error = QStringLiteral("The recovered copy could not be opened: %1").arg(opened.error);
        QDir(pending).removeRecursively();
        return false;
    }
    if (!QDir().rename(pending, finalPath)) {
        *error = QStringLiteral("Cannot publish the Recovered library: %1").arg(finalPath);
        QDir(pending).removeRecursively();
        return false;
    }
    if (!syncDirectory(recoveredBase, error)) {
        QDir(finalPath).removeRecursively();
        return false;
    }
    *recoveredPath = finalPath;
    return true;
}
