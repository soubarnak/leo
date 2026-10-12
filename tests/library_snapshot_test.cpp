#include "library_persistence.h"
#include "library_snapshot.h"
#include "library_window.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <memory>

namespace {

void writeFile(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        qFatal("Could not create snapshot test fixture");
    }
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

void writeLibrary(const QString &root)
{
    writeFile(QDir(root).filePath(QStringLiteral("library.json")), QByteArrayLiteral(
        R"json({"authors":[{"id":"a1","name":"Ada"}],"shelves":[{"id":"s1","name":"Drafts","authorId":"a1","bookIds":["book-1"]}]})json"));
    writeFile(QDir(root).filePath(QStringLiteral("book-1/book.json")), QByteArrayLiteral(
        R"json({"id":"book-1","title":"First Title","author":"Ada","chapterOrder":["chapter-a"],"chapterTitles":{"chapter-a":"Arrival"}})json"));
    writeFile(QDir(root).filePath(QStringLiteral("book-1/chapters/chapter-a.html")),
              QByteArrayLiteral("<p>First paragraph.</p>"));
}

// Listing of every entry with a content hash: byte-identical trees give identical listings.
QByteArray treeListing(const QString &root)
{
    QStringList lines;
    QDirIterator it(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QFileInfo info = it.fileInfo();
        lines << QDir(root).relativeFilePath(info.filePath()) + QLatin1Char(' ') +
                     (info.isDir() ? QStringLiteral("dir")
                                   : QString::fromLatin1(QCryptographicHash::hash(
                                                             readFile(info.filePath()),
                                                             QCryptographicHash::Sha256)
                                                             .toHex()));
    }
    lines.sort();
    return lines.join(QLatin1Char('\n')).toUtf8();
}

QDateTime day(int dayOfMonth, int month = 1)
{
    return QDateTime(QDate(2026, month, dayOfMonth), QTime(12, 0), QTimeZone::UTC);
}

class ScopedEnvironmentVariable final {
public:
    ScopedEnvironmentVariable(const char *name, const QByteArray &value)
        : name_(name), wasSet_(qEnvironmentVariableIsSet(name)), previous_(qgetenv(name))
    {
        qputenv(name, value);
    }
    ~ScopedEnvironmentVariable()
    {
        if (wasSet_) {
            qputenv(name_.constData(), previous_);
        } else {
            qunsetenv(name_.constData());
        }
    }

private:
    QByteArray name_;
    bool wasSet_;
    QByteArray previous_;
};

class LibrarySnapshotTest final : public QObject {
    Q_OBJECT


private slots:
    void init()
    {
        data_.reset(new QTemporaryDir);
        state_.reset(new QTemporaryDir);
        config_.reset(new QTemporaryDir);
        libraryDir_.reset(new QTemporaryDir);
        QVERIFY(data_->isValid() && state_->isValid() && config_->isValid() &&
                libraryDir_->isValid());
        dataEnv_.reset(new ScopedEnvironmentVariable("XDG_DATA_HOME", data_->path().toLocal8Bit()));
        stateEnv_.reset(new ScopedEnvironmentVariable("XDG_STATE_HOME", state_->path().toLocal8Bit()));
        configEnv_.reset(new ScopedEnvironmentVariable("XDG_CONFIG_HOME", config_->path().toLocal8Bit()));
        root_ = libraryDir_->path();
        writeLibrary(root_);
    }

    void cleanup()
    {
        dataEnv_.reset();
        stateEnv_.reset();
        configEnv_.reset();
    }

    void firstRunCreatesVerifiedSnapshotOutsideLibrary()
    {
        const LibrarySnapshotResult result = LibrarySnapshots::maybeSnapshotDaily(root_, day(1));
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QVERIFY(result.created);
        QVERIFY(result.driveError.isEmpty());
        QVERIFY(result.snapshotPath.endsWith(QStringLiteral("2026-01-01")));
        QVERIFY(!result.snapshotPath.startsWith(root_));
        QVERIFY(result.snapshotPath.startsWith(data_->path()));
        QString error;
        QVERIFY2(LibrarySnapshots::verifySnapshot(result.snapshotPath, &error), qPrintable(error));
        QCOMPARE(readFile(result.snapshotPath + QStringLiteral("/library/book-1/chapters/chapter-a.html")),
                 QByteArrayLiteral("<p>First paragraph.</p>"));
    }

    void unchangedLibraryOnNewDayCreatesNoSnapshot()
    {
        QVERIFY(LibrarySnapshots::maybeSnapshotDaily(root_, day(1)).created);
        const LibrarySnapshotResult next = LibrarySnapshots::maybeSnapshotDaily(root_, day(2));
        QVERIFY(!next.created);
        QVERIFY(next.skipped);
        QVERIFY(next.error.isEmpty());
        QCOMPARE(LibrarySnapshots::listSnapshots(root_).size(), 1);
    }

    void changedLibraryOnNewUtcDayCreatesSnapshot()
    {
        QVERIFY(LibrarySnapshots::maybeSnapshotDaily(root_, day(1)).created);
        writeFile(root_ + QStringLiteral("/book-1/chapters/chapter-a.html"),
                  QByteArrayLiteral("<p>Changed.</p>"));
        const LibrarySnapshotResult next = LibrarySnapshots::maybeSnapshotDaily(root_, day(2));
        QVERIFY2(next.error.isEmpty(), qPrintable(next.error));
        QVERIFY(next.created);
        const auto snapshots = LibrarySnapshots::listSnapshots(root_);
        QCOMPARE(snapshots.size(), 2);
        QCOMPARE(snapshots.first().date, QDate(2026, 1, 2));
    }

    void utcDayKeyIgnoresLocalOffset()
    {
        // 23:30 at UTC-05:00 on Jan 1 is already Jan 2 in UTC.
        const QDateTime late(QDate(2026, 1, 1), QTime(23, 30), QTimeZone::fromSecondsAheadOfUtc(-5 * 3600));
        const LibrarySnapshotResult result = LibrarySnapshots::maybeSnapshotDaily(root_, late);
        QVERIFY(result.snapshotPath.endsWith(QStringLiteral("2026-01-02")));
    }

    void secondChangeSameDayKeepsOneArchivePerDay()
    {
        QVERIFY(LibrarySnapshots::maybeSnapshotDaily(root_, day(1)).created);
        writeFile(root_ + QStringLiteral("/library.json"), QByteArrayLiteral("{\"authors\":[1]}"));
        const LibrarySnapshotResult again = LibrarySnapshots::maybeSnapshotDaily(root_, day(1));
        QVERIFY(!again.created);
        QVERIFY(again.skipped);
        QCOMPARE(LibrarySnapshots::listSnapshots(root_).size(), 1);
    }

    void keepsOnlyFourteenValidDaysAndPrunesOldest()
    {
        for (int d = 1; d <= 16; ++d) {
            writeFile(root_ + QStringLiteral("/book-1/chapters/chapter-a.html"),
                      QByteArray("<p>day ") + QByteArray::number(d) + "</p>");
            const LibrarySnapshotResult r = LibrarySnapshots::maybeSnapshotDaily(root_, day(d));
            QVERIFY2(r.error.isEmpty() && r.created, qPrintable(r.error));
        }
        const auto snapshots = LibrarySnapshots::listSnapshots(root_);
        QCOMPARE(snapshots.size(), 14);
        QCOMPARE(snapshots.first().date, QDate(2026, 1, 16));
        QCOMPARE(snapshots.last().date, QDate(2026, 1, 3));
        QVERIFY(!QFileInfo::exists(LibrarySnapshots::snapshotBucket(root_) + QStringLiteral("/2026-01-01")));
    }

    void invalidFinalizedDayIsNotCountedAndNotDeleted()
    {
        for (int d = 1; d <= 3; ++d) {
            writeFile(root_ + QStringLiteral("/library.json"), QByteArray("{\"n\":") + QByteArray::number(d) + "}");
            QVERIFY(LibrarySnapshots::maybeSnapshotDaily(root_, day(d)).created);
        }
        const QString broken = LibrarySnapshots::snapshotBucket(root_) + QStringLiteral("/2026-01-02");
        QFile victim(broken + QStringLiteral("/library/library.json"));
        QVERIFY(victim.open(QIODevice::WriteOnly | QIODevice::Truncate));
        victim.write("tampered");
        victim.close();
        QString error;
        QVERIFY(!LibrarySnapshots::verifySnapshot(broken, &error));

        // An invalid folder is never counted or deleted, and while it exists nothing is pruned.
        LibrarySnapshotResult last;
        for (int d = 4; d <= 17; ++d) {
            writeFile(root_ + QStringLiteral("/library.json"), QByteArray("{\"n\":") + QByteArray::number(d) + "}");
            last = LibrarySnapshots::maybeSnapshotDaily(root_, day(d));
            QVERIFY(last.created);
        }
        QVERIFY(!last.warning.isEmpty());
        QStringList invalid;
        const auto valid = LibrarySnapshots::listSnapshots(root_, &invalid);
        QCOMPARE(valid.size(), 16);
        QCOMPARE(invalid.size(), 1);
        // Kept for inspection under a set-aside name.
        QVERIFY(invalid.first().contains(QStringLiteral("2026-01-02.invalid-")));
        QVERIFY(QFileInfo(invalid.first()).isDir());
    }

    void newerInvalidFolderStopsPruningOfOlderValidArchives()
    {
        for (int d = 1; d <= 14; ++d) {
            writeFile(root_ + QStringLiteral("/library.json"), QByteArray("{\"n\":") + QByteArray::number(d) + "}");
            QVERIFY(LibrarySnapshots::maybeSnapshotDaily(root_, day(d)).created);
        }
        QFile victim(LibrarySnapshots::snapshotBucket(root_) + QStringLiteral("/2026-01-14/library/library.json"));
        QVERIFY(victim.open(QIODevice::WriteOnly | QIODevice::Truncate));
        victim.write("tampered");
        victim.close();
        for (int d = 15; d <= 16; ++d) {
            writeFile(root_ + QStringLiteral("/library.json"), QByteArray("{\"n\":") + QByteArray::number(d) + "}");
            QVERIFY(LibrarySnapshots::maybeSnapshotDaily(root_, day(d)).created);
        }
        QVERIFY(QFileInfo(LibrarySnapshots::snapshotBucket(root_) + QStringLiteral("/2026-01-01")).isDir());
    }

    void futureDatedFolderWarnsButDoesNotStopArchivingOrPruning()
    {
        for (int d = 1; d <= 14; ++d) {
            writeFile(root_ + QStringLiteral("/library.json"), QByteArray("{\"n\":") + QByteArray::number(d) + "}");
            QVERIFY(LibrarySnapshots::maybeSnapshotDaily(root_, day(d)).created);
        }
        const QString bucket = LibrarySnapshots::snapshotBucket(root_);
        QVERIFY(QDir().rename(bucket + QStringLiteral("/2026-01-14"), bucket + QStringLiteral("/2026-06-01")));
        writeFile(root_ + QStringLiteral("/library.json"), "{\"n\":99}");
        const LibrarySnapshotResult r = LibrarySnapshots::maybeSnapshotDaily(root_, day(15));
        QVERIFY2(r.created, qPrintable(r.error));
        QVERIFY(!r.warning.isEmpty());
        QCOMPARE(LibrarySnapshots::listSnapshots(root_).size(), 15);
        QVERIFY(QFileInfo(bucket + QStringLiteral("/2026-01-01")).isDir());
    }

    void invalidFoldersAreReportedOnEveryTick()
    {
        const LibrarySnapshotResult first = LibrarySnapshots::maybeSnapshotDaily(root_, day(1));
        QFile victim(first.snapshotPath + QStringLiteral("/library/library.json"));
        QVERIFY(victim.open(QIODevice::WriteOnly | QIODevice::Truncate));
        victim.write("tampered");
        victim.close();
        writeFile(root_ + QStringLiteral("/book-1/chapters/chapter-a.html"), "<p>later</p>");
        QVERIFY(!LibrarySnapshots::maybeSnapshotDaily(root_, day(1)).invalidSnapshots.isEmpty());
        const LibrarySnapshotResult skip = LibrarySnapshots::maybeSnapshotDaily(root_, day(1));
        QVERIFY(!skip.created);
        QVERIFY(!skip.invalidSnapshots.isEmpty());
    }

    void damagedDriveCopyIsReplaced()
    {
        QTemporaryDir drive;
        writeFile(QDir(AppPathsConfig()).filePath(QStringLiteral("snapshot-drives.json")),
                  QJsonDocument(QJsonObject{{LibrarySnapshots::libraryHash(root_), drive.path()}}).toJson());
        QVERIFY(LibrarySnapshots::maybeSnapshotDaily(root_, day(1)).driveError.isEmpty());
        const QString copy = drive.path() + QStringLiteral("/leo-snapshots/") +
                             LibrarySnapshots::libraryHash(root_) + QStringLiteral("/2026-01-01");
        QFile victim(copy + QStringLiteral("/library/library.json"));
        QVERIFY(victim.open(QIODevice::WriteOnly | QIODevice::Truncate));
        victim.write("tampered");
        victim.close();
        QString error;
        QVERIFY(!LibrarySnapshots::verifySnapshot(copy, &error));
        const LibrarySnapshotResult r = LibrarySnapshots::maybeSnapshotDaily(root_, day(1));
        QVERIFY2(r.driveError.isEmpty(), qPrintable(r.driveError));
        QVERIFY2(LibrarySnapshots::verifySnapshot(copy, &error), qPrintable(error));
        // The damaged copy is kept under a set-aside name, never deleted.
        const QDir driveDay(QFileInfo(copy).absolutePath());
        const QStringList aside = driveDay.entryList({QStringLiteral("2026-01-01.invalid-*")}, QDir::Dirs);
        QCOMPARE(aside.size(), 1);
        QCOMPARE(readFile(driveDay.filePath(aside.first()) + QStringLiteral("/library/library.json")),
                 QByteArrayLiteral("tampered"));
    }

    void pendingLeftoversAreRemovedAfterSuccessfulSnapshot()
    {
        const QString bucket = LibrarySnapshots::snapshotBucket(root_);
        writeFile(bucket + QStringLiteral("/.pending-crashed/library/x"), "partial");
        const LibrarySnapshotResult r = LibrarySnapshots::maybeSnapshotDaily(root_, day(1));
        QVERIFY(r.created);
        QVERIFY(!QFileInfo::exists(bucket + QStringLiteral("/.pending-crashed")));
    }

    void backupsAndExportsAreExcludedUnknownFilesIncluded()
    {
        writeFile(root_ + QStringLiteral("/Backups/old.zip"), "backup");
        writeFile(root_ + QStringLiteral("/Exports/book.epub"), "export");
        writeFile(root_ + QStringLiteral("/book-1/Backups/keep.txt"), "nested stays");
        writeFile(root_ + QStringLiteral("/book-1/notes/mystery.xyz"), "unknown");
        const LibrarySnapshotResult r = LibrarySnapshots::maybeSnapshotDaily(root_, day(1));
        QVERIFY2(r.created, qPrintable(r.error));
        const QString copy = r.snapshotPath + QStringLiteral("/library");
        QVERIFY(!QFileInfo::exists(copy + QStringLiteral("/Backups")));
        QVERIFY(!QFileInfo::exists(copy + QStringLiteral("/Exports")));
        QCOMPARE(readFile(copy + QStringLiteral("/book-1/notes/mystery.xyz")), QByteArrayLiteral("unknown"));
        QVERIFY(QFileInfo::exists(copy + QStringLiteral("/book-1/Backups/keep.txt")));
    }

    void changeOnlyInBackupsOrExportsIsNotAChange()
    {
        QVERIFY(LibrarySnapshots::maybeSnapshotDaily(root_, day(1)).created);
        writeFile(root_ + QStringLiteral("/Exports/new.epub"), "export");
        const LibrarySnapshotResult r = LibrarySnapshots::maybeSnapshotDaily(root_, day(2));
        QVERIFY(!r.created);
        QVERIFY(r.skipped);
    }

    void sourceChangingDuringCopyCreatesNoSnapshot()
    {
        const LibrarySnapshotResult r = LibrarySnapshots::maybeSnapshotDaily(
            root_, day(1), [this] {
                writeFile(root_ + QStringLiteral("/book-1/chapters/chapter-a.html"),
                          QByteArrayLiteral("<p>edited mid-copy</p>"));
            });
        QVERIFY(!r.created);
        QVERIFY(!r.error.isEmpty());
        QCOMPARE(LibrarySnapshots::listSnapshots(root_).size(), 0);
        const QDir bucket(LibrarySnapshots::snapshotBucket(root_));
        QVERIFY(bucket.entryList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
        QVERIFY(!QFileInfo::exists(bucket.filePath(QStringLiteral("latest.json"))));
    }

    void missingOptionalDriveIsReportedAndLocalSnapshotStays()
    {
        const QString drive = config_->path() + QStringLiteral("/no-such-drive");
        writeFile(QDir(AppPathsConfig()).filePath(QStringLiteral("snapshot-drives.json")),
                  QJsonDocument(QJsonObject{{LibrarySnapshots::libraryHash(root_), drive}}).toJson());
        const LibrarySnapshotResult r = LibrarySnapshots::maybeSnapshotDaily(root_, day(1));
        QVERIFY(r.created);
        QVERIFY(r.error.isEmpty());
        QVERIFY(!r.driveError.isEmpty());
        QCOMPARE(LibrarySnapshots::listSnapshots(root_).size(), 1);
    }

    void presentOptionalDriveReceivesVerifiedCopy()
    {
        QTemporaryDir drive;
        QVERIFY(drive.isValid());
        writeFile(QDir(AppPathsConfig()).filePath(QStringLiteral("snapshot-drives.json")),
                  QJsonDocument(QJsonObject{{LibrarySnapshots::libraryHash(root_), drive.path()}}).toJson());
        const LibrarySnapshotResult r = LibrarySnapshots::maybeSnapshotDaily(root_, day(1));
        QVERIFY2(r.created && r.driveError.isEmpty(), qPrintable(r.driveError));
        const QString copy = drive.path() + QStringLiteral("/leo-snapshots/") +
                             LibrarySnapshots::libraryHash(root_) + QStringLiteral("/2026-01-01");
        QString error;
        QVERIFY2(LibrarySnapshots::verifySnapshot(copy, &error), qPrintable(error));
    }

    void driveBackOnlineIsFilledOnNextUnchangedRun()
    {
        QTemporaryDir parent;
        const QString drive = parent.path() + QStringLiteral("/usb");
        writeFile(QDir(AppPathsConfig()).filePath(QStringLiteral("snapshot-drives.json")),
                  QJsonDocument(QJsonObject{{LibrarySnapshots::libraryHash(root_), drive}}).toJson());
        QVERIFY(!LibrarySnapshots::maybeSnapshotDaily(root_, day(1)).driveError.isEmpty());
        QVERIFY(QDir().mkpath(drive));
        const LibrarySnapshotResult r = LibrarySnapshots::maybeSnapshotDaily(root_, day(2));
        QVERIFY(r.skipped);
        QVERIFY2(r.driveError.isEmpty(), qPrintable(r.driveError));
        QVERIFY(QFileInfo::exists(drive + QStringLiteral("/leo-snapshots/") +
                                  LibrarySnapshots::libraryHash(root_) + QStringLiteral("/2026-01-01/manifest.json")));
    }

    void restoreCreatesSeparateRecoveredLibraryAndLeavesLibraryUntouched()
    {
        QVERIFY(LibrarySnapshots::maybeSnapshotDaily(root_, day(1)).created);
        const QByteArray snapshotListing = treeListing(
            LibrarySnapshots::snapshotBucket(root_) + QStringLiteral("/2026-01-01/library"));
        writeFile(root_ + QStringLiteral("/book-1/chapters/chapter-a.html"), "<p>newer</p>");
        const QByteArray before = treeListing(root_);

        QString recovered;
        QString error;
        QVERIFY2(LibrarySnapshots::restoreToRecoveredLibrary(
                     root_, LibrarySnapshots::snapshotBucket(root_) + QStringLiteral("/2026-01-01"),
                     day(5), &recovered, &error),
                 qPrintable(error));
        QCOMPARE(treeListing(root_), before);
        QVERIFY(!recovered.startsWith(root_));
        QVERIFY(QFileInfo(recovered).fileName().startsWith(QStringLiteral("Recovered library 20260105T120000Z-")));
        QCOMPARE(treeListing(recovered), snapshotListing);
        QCOMPARE(readFile(recovered + QStringLiteral("/book-1/chapters/chapter-a.html")),
                 QByteArrayLiteral("<p>First paragraph.</p>"));
        // A fresh safety snapshot of the current Library was taken first.
        const QDir safety(data_->path() + QStringLiteral("/leo-writer/safety-snapshots/") +
                          LibrarySnapshots::libraryHash(root_));
        QCOMPARE(safety.entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 1);
        const QString safetyDir = safety.filePath(safety.entryList(QDir::Dirs | QDir::NoDotAndDotDot).first());
        QCOMPARE(readFile(safetyDir + QStringLiteral("/book-1/chapters/chapter-a.html")),
                 QByteArrayLiteral("<p>newer</p>"));
    }

    void sameDayChangeWithMissingDriveStillReportsAndLaterMirrors()
    {
        QTemporaryDir parent;
        const QString drive = parent.path() + QStringLiteral("/usb");
        writeFile(QDir(AppPathsConfig()).filePath(QStringLiteral("snapshot-drives.json")),
                  QJsonDocument(QJsonObject{{LibrarySnapshots::libraryHash(root_), drive}}).toJson());
        QVERIFY(!LibrarySnapshots::maybeSnapshotDaily(root_, day(1)).driveError.isEmpty());
        writeFile(root_ + QStringLiteral("/book-1/chapters/chapter-a.html"), "<p>same day edit</p>");
        const LibrarySnapshotResult r = LibrarySnapshots::maybeSnapshotDaily(root_, day(1));
        QVERIFY(!r.created);
        QVERIFY(!r.driveError.isEmpty());
        QCOMPARE(LibrarySnapshots::listSnapshots(root_).size(), 1);
        QVERIFY(QDir().mkpath(drive));
        const LibrarySnapshotResult again = LibrarySnapshots::maybeSnapshotDaily(root_, day(1));
        QVERIFY2(again.driveError.isEmpty(), qPrintable(again.driveError));
        QVERIFY(QFileInfo::exists(drive + QStringLiteral("/leo-snapshots/") +
                                  LibrarySnapshots::libraryHash(root_) + QStringLiteral("/2026-01-01/manifest.json")));
    }

    void backwardClockStillSnapshotsButNeverPrunes()
    {
        for (int d = 1; d <= 16; ++d) {
            writeFile(root_ + QStringLiteral("/library.json"), QByteArray("{\"n\":") + QByteArray::number(d) + "}");
            QVERIFY(LibrarySnapshots::maybeSnapshotDaily(root_, day(d)).created);
        }
        writeFile(root_ + QStringLiteral("/library.json"), "{\"n\":99}");
        const LibrarySnapshotResult r = LibrarySnapshots::maybeSnapshotDaily(root_, day(2));
        QVERIFY2(r.created, qPrintable(r.error));
        QVERIFY(!r.warning.isEmpty());
        const auto snapshots = LibrarySnapshots::listSnapshots(root_);
        QCOMPARE(snapshots.size(), 15);
        QCOMPARE(snapshots.first().date, QDate(2026, 1, 16));
        QVERIFY(QFileInfo(LibrarySnapshots::snapshotBucket(root_) + QStringLiteral("/2026-01-03")).isDir());
        // latest.json points at a folder that exists.
        const QJsonObject latest = QJsonDocument::fromJson(
            readFile(LibrarySnapshots::snapshotBucket(root_) + QStringLiteral("/latest.json"))).object();
        QVERIFY(QFileInfo(LibrarySnapshots::snapshotBucket(root_) + QLatin1Char('/') +
                          latest.value(QStringLiteral("date")).toString()).isDir());
    }

    void olderInvalidFolderIsSetAsideAndStillReportedOnSkipTick()
    {
        for (int d = 1; d <= 3; ++d) {
            writeFile(root_ + QStringLiteral("/library.json"), QByteArray("{\"n\":") + QByteArray::number(d) + "}");
            QVERIFY(LibrarySnapshots::maybeSnapshotDaily(root_, day(d)).created);
        }
        QFile victim(LibrarySnapshots::snapshotBucket(root_) + QStringLiteral("/2026-01-01/library/library.json"));
        QVERIFY(victim.open(QIODevice::WriteOnly | QIODevice::Truncate));
        victim.write("tampered");
        victim.close();
        writeFile(root_ + QStringLiteral("/library.json"), "{\"n\":4}");
        const LibrarySnapshotResult created = LibrarySnapshots::maybeSnapshotDaily(root_, day(4));
        QVERIFY(created.created);
        QCOMPARE(created.invalidSnapshots.size(), 1);
        QVERIFY(!QFileInfo::exists(LibrarySnapshots::snapshotBucket(root_) + QStringLiteral("/2026-01-01")));
        const LibrarySnapshotResult skip = LibrarySnapshots::maybeSnapshotDaily(root_, day(5));
        QVERIFY(skip.skipped);
        QCOMPARE(skip.invalidSnapshots.size(), 1);
    }

    void staleLatestPointerWithTamperedManifestCreatesFreshSnapshot()
    {
        const LibrarySnapshotResult first = LibrarySnapshots::maybeSnapshotDaily(root_, day(1));
        QVERIFY(first.created);
        QFile manifest(first.snapshotPath + QStringLiteral("/manifest.json"));
        QVERIFY(manifest.open(QIODevice::Append));
        manifest.write(" ");
        manifest.close();
        const LibrarySnapshotResult next = LibrarySnapshots::maybeSnapshotDaily(root_, day(2));
        QVERIFY2(next.created, qPrintable(next.error));
    }

    void corruptTodayFolderIsSetAsideAndRecreated()
    {
        const LibrarySnapshotResult first = LibrarySnapshots::maybeSnapshotDaily(root_, day(1));
        QVERIFY(first.created);
        QFile victim(first.snapshotPath + QStringLiteral("/library/library.json"));
        QVERIFY(victim.open(QIODevice::WriteOnly | QIODevice::Truncate));
        victim.write("tampered");
        victim.close();
        writeFile(root_ + QStringLiteral("/book-1/chapters/chapter-a.html"), "<p>later</p>");
        const LibrarySnapshotResult next = LibrarySnapshots::maybeSnapshotDaily(root_, day(1));
        QVERIFY2(next.created, qPrintable(next.error));
        QCOMPARE(next.invalidSnapshots.size(), 1);
        QVERIFY(next.invalidSnapshots.first().contains(QStringLiteral("2026-01-01.invalid-")));
        QVERIFY(QFileInfo(next.invalidSnapshots.first()).isDir());
        QStringList invalid;
        QCOMPARE(LibrarySnapshots::listSnapshots(root_, &invalid).size(), 1);
        QCOMPARE(invalid.size(), 1);
    }

    void restoreRefusesCorruptSnapshotAndLeavesNoRecoveredLibrary()
    {
        QVERIFY(LibrarySnapshots::maybeSnapshotDaily(root_, day(1)).created);
        const QString dayDir = LibrarySnapshots::snapshotBucket(root_) + QStringLiteral("/2026-01-01");
        QFile victim(dayDir + QStringLiteral("/library/book-1/book.json"));
        QVERIFY(victim.open(QIODevice::ReadWrite));
        victim.seek(2);
        victim.write("X");
        victim.close();
        const QByteArray before = treeListing(root_);

        QString recovered;
        QString error;
        QVERIFY(!LibrarySnapshots::restoreToRecoveredLibrary(root_, dayDir, day(5), &recovered, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(recovered.isEmpty());
        QCOMPARE(treeListing(root_), before);
        const QDir recoveredBase(data_->path() + QStringLiteral("/leo-writer/Recovered Libraries"));
        QVERIFY(recoveredBase.entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
    }

    void restoreRefusesPathOutsideSnapshotStore()
    {
        QString recovered;
        QString error;
        QVERIFY(!LibrarySnapshots::restoreToRecoveredLibrary(root_, root_, day(5), &recovered, &error));
        QVERIFY(!error.isEmpty());
    }

    void snapshotAndRestoreNeverWriteUnderLibraryRoot()
    {
        const QByteArray before = treeListing(root_);
        QVERIFY(LibrarySnapshots::maybeSnapshotDaily(root_, day(1)).created);
        QCOMPARE(treeListing(root_), before);
        QString recovered;
        QString error;
        QVERIFY(LibrarySnapshots::restoreToRecoveredLibrary(
            root_, LibrarySnapshots::snapshotBucket(root_) + QStringLiteral("/2026-01-01"), day(2),
            &recovered, &error));
        QCOMPARE(treeListing(root_), before);
    }

    void openingLibraryRunsStartupDayCheckAndCreatesOneSnapshot()
    {
        LibraryWindow window;
        QVERIFY2(window.openLibrary(root_), "fixture Library did not open");
        const auto snapshots = LibrarySnapshots::listSnapshots(root_);
        QCOMPARE(snapshots.size(), 1);
        QVERIFY(!snapshots.first().path.startsWith(root_));
        QVERIFY(snapshots.first().path.startsWith(data_->path()));
        QString error;
        QVERIFY2(LibrarySnapshots::verifySnapshot(snapshots.first().path, &error),
                 qPrintable(error));
        // Reopening the unchanged Library must not add a second archive for the same day.
        QVERIFY(window.openLibrary(root_));
        QCOMPARE(LibrarySnapshots::listSnapshots(root_).size(), 1);
    }

    void missingDriveFailureIsVisibleWhileLocalSnapshotStays()
    {
        const QString drive = config_->path() + QStringLiteral("/no-such-drive");
        writeFile(QDir(AppPathsConfig()).filePath(QStringLiteral("snapshot-drives.json")),
                  QJsonDocument(QJsonObject{{LibrarySnapshots::libraryHash(root_), drive}})
                      .toJson());

        LibraryWindow window;
        window.show();
        QVERIFY2(window.openLibrary(root_), "fixture Library did not open");

        // The local archive exists first...
        const auto snapshots = LibrarySnapshots::listSnapshots(root_);
        QCOMPARE(snapshots.size(), 1);
        QString error;
        QVERIFY2(LibrarySnapshots::verifySnapshot(snapshots.first().path, &error),
                 qPrintable(error));

        // ...and the failed drive copy shows up in the status bar without blocking anything.
        auto *notice = window.findChild<QLabel *>(QStringLiteral("library-snapshot-notice"));
        QVERIFY(notice);
        QTRY_VERIFY_WITH_TIMEOUT(notice->isVisible() &&
                                     notice->text().contains(QStringLiteral("Backup drive copy failed")),
                                 10000);
        QVERIFY(notice->text().contains(QStringLiteral("no-such-drive")));
    }

    void restoreSnapshotActionExtractsRecoveredLibraryAndLeavesLibraryUnchanged()
    {
        LibraryWindow window;
        QVERIFY2(window.openLibrary(root_), "fixture Library did not open");
        QCOMPARE(LibrarySnapshots::listSnapshots(root_).size(), 1);
        const QByteArray before = treeListing(root_);

        QAction *restoreAction = nullptr;
        if (!window.menuBar()->actions().isEmpty()) {
            QMenu *fileMenu = window.menuBar()->actions().first()->menu();
            for (QAction *action : fileMenu ? fileMenu->actions() : QList<QAction *>()) {
                if (action->text() == QStringLiteral("Restore snapshot…")) {
                    restoreAction = action;
                }
            }
        }
        QVERIFY(restoreAction);

        // Walks the modal dialogs: choose the listed snapshot, then decline opening it now.
        int stage = 0;
        QTimer hijack;
        hijack.setInterval(25);
        connect(&hijack, &QTimer::timeout, this, [&] {
            if (stage == 0) {
                if (auto *input = qobject_cast<QInputDialog *>(QApplication::activeModalWidget())) {
                    stage = 1;
                    input->accept();
                }
            } else if (stage == 1) {
                if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                    stage = 2;
                    QAbstractButton *no = box->button(QMessageBox::No);
                    if (no) {
                        no->click();
                    } else {
                        box->reject();
                    }
                }
            }
        });
        hijack.start();
        restoreAction->trigger();
        hijack.stop();
        QCOMPARE(stage, 2);

        // A separate Recovered library holds the snapshot's content...
        const QDir recoveredBase(
            QDir(data_->path()).filePath(QStringLiteral("leo-writer/Recovered Libraries")));
        const QStringList recovered = recoveredBase.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        QCOMPARE(recovered.size(), 1);
        QCOMPARE(readFile(recoveredBase.filePath(recovered.first() +
                                                  QStringLiteral("/book-1/chapters/chapter-a.html"))),
                 QByteArrayLiteral("<p>First paragraph.</p>"));
        // ...and the current Library is byte-for-byte unchanged.
        QCOMPARE(treeListing(root_), before);
    }

private:
    QString AppPathsConfig() const { return config_->path() + QStringLiteral("/leo-writer"); }

    std::unique_ptr<QTemporaryDir> data_, state_, config_, libraryDir_;
    std::unique_ptr<ScopedEnvironmentVariable> dataEnv_, stateEnv_, configEnv_;
    QString root_;
};

}

QTEST_MAIN(LibrarySnapshotTest)
#include "library_snapshot_test.moc"
