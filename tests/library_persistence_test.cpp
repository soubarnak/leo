#include "library_persistence.h"
#include "library_window.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace {

const QString chapterRelativePath = QStringLiteral("book-1/chapters/chapter-a.html");
const QByteArray oldChapter("<p>Saved paragraph.</p>");
const QByteArray newChapter("<p>Recovered paragraph.</p>");

void writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        qFatal("Could not create persistence test fixture");
    }
}

bool readFile(const QString &path, QByteArray *bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    *bytes = file.readAll();
    return file.error() == QFileDevice::NoError;
}

bool writeLibrary(const QString &root)
{
    const QString chapterDirectory = QDir(root).filePath(QStringLiteral("book-1/chapters"));
    if (!QDir().mkpath(chapterDirectory)) {
        return false;
    }
    writeFile(QDir(root).filePath(QStringLiteral("library.json")), QByteArrayLiteral(
        R"json({"authors":[{"id":"a1","name":"Ada"}],"shelves":[{"id":"s1","name":"Drafts","authorId":"a1","bookIds":["book-1"]}]})json"));
    writeFile(QDir(root).filePath(QStringLiteral("book-1/book.json")), QByteArrayLiteral(
        R"json({"id":"book-1","title":"First Title","author":"Ada","chapterOrder":["chapter-a"],"chapterTitles":{"chapter-a":"Arrival"}})json"));
    writeFile(QDir(root).filePath(chapterRelativePath), oldChapter);
    return true;
}

QByteArray sha256(const QByteArray &bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

QString checkpointArgument(PersistenceCheckpoint checkpoint)
{
    switch (checkpoint) {
    case PersistenceCheckpoint::BeforeTargetStaging:
        return QStringLiteral("before-staging");
    case PersistenceCheckpoint::AfterTargetStaging:
        return QStringLiteral("after-staging");
    case PersistenceCheckpoint::BeforeTargetFlush:
        return QStringLiteral("before-flush");
    case PersistenceCheckpoint::AfterTargetFlush:
        return QStringLiteral("after-flush");
    case PersistenceCheckpoint::BeforeTargetRename:
        return QStringLiteral("before-rename");
    case PersistenceCheckpoint::AfterTargetRename:
        return QStringLiteral("after-rename");
    case PersistenceCheckpoint::BeforeTargetDirectoryFlush:
        return QStringLiteral("before-directory-flush");
    case PersistenceCheckpoint::AfterTargetDirectoryFlush:
        return QStringLiteral("after-directory-flush");
    case PersistenceCheckpoint::BeforeJournalCompletion:
        return QStringLiteral("before-journal-completion");
    case PersistenceCheckpoint::AfterJournalCompletion:
        return QStringLiteral("after-journal-completion");
    }
    return QStringLiteral("unknown");
}

bool checkpointFromArgument(const QString &argument, PersistenceCheckpoint *checkpoint)
{
    const QList<PersistenceCheckpoint> checkpoints{
        PersistenceCheckpoint::BeforeTargetStaging,
        PersistenceCheckpoint::AfterTargetStaging,
        PersistenceCheckpoint::BeforeTargetFlush,
        PersistenceCheckpoint::AfterTargetFlush,
        PersistenceCheckpoint::BeforeTargetRename,
        PersistenceCheckpoint::AfterTargetRename,
        PersistenceCheckpoint::BeforeTargetDirectoryFlush,
        PersistenceCheckpoint::AfterTargetDirectoryFlush,
        PersistenceCheckpoint::BeforeJournalCompletion,
        PersistenceCheckpoint::AfterJournalCompletion};
    for (const PersistenceCheckpoint candidate : checkpoints) {
        if (checkpointArgument(candidate) == argument) {
            *checkpoint = candidate;
            return true;
        }
    }
    return false;
}

int workerSave(int argc, char **argv)
{
    if (argc != 7) {
        return 40;
    }
    const QString libraryPath = QString::fromLocal8Bit(argv[2]);
    const QByteArray oldBytes(argv[3]);
    const QByteArray newBytes(argv[4]);
    const QString requestedCheckpoint = QString::fromLocal8Bit(argv[5]);
    const QString markerPath = QString::fromLocal8Bit(argv[6]);

    PersistenceCheckpoint targetCheckpoint;
    PersistenceCheckpointHook hook;
    if (requestedCheckpoint != QStringLiteral("none")) {
        if (!checkpointFromArgument(requestedCheckpoint, &targetCheckpoint)) {
            return 41;
        }
        hook = [targetCheckpoint, markerPath](PersistenceCheckpoint checkpoint,
                                               QString *) {
            if (checkpoint != targetCheckpoint) {
                return true;
            }
            const QByteArray nativeMarker = QFile::encodeName(markerPath);
            const int descriptor = ::open(nativeMarker.constData(),
                                          O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
            if (descriptor < 0) {
                return false;
            }
            const QByteArray contents = checkpointArgument(checkpoint).toUtf8();
            const ssize_t written = ::write(descriptor, contents.constData(),
                                            static_cast<size_t>(contents.size()));
            const int syncResult = ::fsync(descriptor);
            ::close(descriptor);
            if (written != contents.size() || syncResult != 0) {
                return false;
            }
            for (;;) {
                ::pause();
            }
        };
    }

    const PersistenceResult result = LibraryPersistence::saveFile(
        libraryPath, chapterRelativePath, sha256(oldBytes), newBytes, hook);
    return result.ok ? 0 : 42;
}

int workerOpen(int argc, char **argv)
{
    if (argc != 5) {
        return 50;
    }
    const QString libraryPath = QString::fromLocal8Bit(argv[2]);
    const QByteArray expectedBytes(argv[3]);
    const bool expectRecoveryNotice = QByteArray(argv[4]) == QByteArrayLiteral("yes");
    LibraryWindow window;
    if (!window.openLibrary(libraryPath)) {
        return 51;
    }

    QByteArray chapterBytes;
    if (!readFile(QDir(libraryPath).filePath(chapterRelativePath), &chapterBytes) ||
        chapterBytes != expectedBytes) {
        return 52;
    }
    window.show();
    QApplication::processEvents();
    auto *notice = window.findChild<QLabel *>("library-recovery-notice");
    if (expectRecoveryNotice != (notice && notice->isVisible())) {
        return 53;
    }
    return 0;
}

QProcessEnvironment testEnvironment(const QString &dataHome, const QString &stateHome)
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    environment.insert(QStringLiteral("XDG_DATA_HOME"), dataHome);
    environment.insert(QStringLiteral("XDG_STATE_HOME"), stateHome);
    return environment;
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

QList<PersistenceCheckpoint> allCheckpoints()
{
    return {PersistenceCheckpoint::BeforeTargetStaging,
            PersistenceCheckpoint::AfterTargetStaging,
            PersistenceCheckpoint::BeforeTargetFlush,
            PersistenceCheckpoint::AfterTargetFlush,
            PersistenceCheckpoint::BeforeTargetRename,
            PersistenceCheckpoint::AfterTargetRename,
            PersistenceCheckpoint::BeforeTargetDirectoryFlush,
            PersistenceCheckpoint::AfterTargetDirectoryFlush,
            PersistenceCheckpoint::BeforeJournalCompletion,
            PersistenceCheckpoint::AfterJournalCompletion};
}

}

class LibraryPersistenceTest final : public QObject {
    Q_OBJECT

private slots:
    void externalWriteAtRenameBoundaryIsPreserved()
    {
        QTemporaryDir privateData;
        QTemporaryDir privateState;
        QTemporaryDir library;
        QVERIFY(privateData.isValid());
        QVERIFY(privateState.isValid());
        QVERIFY(library.isValid());
        QVERIFY(writeLibrary(library.path()));
        ScopedEnvironmentVariable dataHome("XDG_DATA_HOME", privateData.path().toLocal8Bit());
        ScopedEnvironmentVariable stateHome("XDG_STATE_HOME", privateState.path().toLocal8Bit());

        const QByteArray externalBytes("<p>External revision.</p>");
        const PersistenceCheckpointHook hook = [libraryPath = library.path(), externalBytes](
            PersistenceCheckpoint checkpoint, QString *) {
            if (checkpoint == PersistenceCheckpoint::BeforeTargetRename) {
                writeFile(QDir(libraryPath).filePath(chapterRelativePath), externalBytes);
            }
            return true;
        };
        const PersistenceResult result = LibraryPersistence::saveFile(
            library.path(), chapterRelativePath, sha256(oldChapter), newChapter, hook);
        QVERIFY(!result.ok);
        QVERIFY(result.conflict);

        QByteArray bytes;
        QVERIFY(readFile(QDir(library.path()).filePath(chapterRelativePath), &bytes));
        QCOMPARE(bytes, externalBytes);
        const PersistenceResult recovery = LibraryPersistence::recoverPendingSaves(library.path());
        QVERIFY(!recovery.ok);
        QVERIFY(recovery.conflict);
        QVERIFY(readFile(QDir(library.path()).filePath(chapterRelativePath), &bytes));
        QCOMPARE(bytes, externalBytes);
    }

    void returnedFaultsRetainRecoverableJournal()
    {
        for (const PersistenceCheckpoint checkpoint : allCheckpoints()) {
            QTemporaryDir privateData;
            QTemporaryDir privateState;
            QTemporaryDir library;
            QVERIFY(privateData.isValid());
            QVERIFY(privateState.isValid());
            QVERIFY(library.isValid());
            QVERIFY(writeLibrary(library.path()));
            ScopedEnvironmentVariable dataHome("XDG_DATA_HOME", privateData.path().toLocal8Bit());
            ScopedEnvironmentVariable stateHome("XDG_STATE_HOME", privateState.path().toLocal8Bit());

            const PersistenceCheckpointHook hook = [checkpoint](PersistenceCheckpoint current,
                                                                  QString *error) {
                if (current != checkpoint) {
                    return true;
                }
                *error = QStringLiteral("Injected persistence fault.");
                return false;
            };
            const PersistenceResult failed = LibraryPersistence::saveFile(
                library.path(), chapterRelativePath, sha256(oldChapter), newChapter, hook);
            QVERIFY2(!failed.ok, qPrintable(checkpointArgument(checkpoint)));

            QByteArray preRecoveryBytes;
            QVERIFY(readFile(QDir(library.path()).filePath(chapterRelativePath), &preRecoveryBytes));
            QVERIFY(preRecoveryBytes == oldChapter || preRecoveryBytes == newChapter);

            LibraryWindow window;
            QVERIFY2(window.openLibrary(library.path()), qPrintable(checkpointArgument(checkpoint)));
            QByteArray recoveredBytes;
            QVERIFY(readFile(QDir(library.path()).filePath(chapterRelativePath), &recoveredBytes));
            QCOMPARE(recoveredBytes, newChapter);
            window.show();
            QApplication::processEvents();
            auto *notice = window.findChild<QLabel *>("library-recovery-notice");
            QVERIFY(notice);
            QVERIFY2(notice->isVisible(), qPrintable(checkpointArgument(checkpoint)));
        }
    }

    void processInterruptionRecoversAtEverySaveBoundary()
    {
        const QString workerPath = QCoreApplication::applicationFilePath();
        for (const PersistenceCheckpoint checkpoint : allCheckpoints()) {
            QTemporaryDir root;
            QVERIFY(root.isValid());
            const QString dataHome = QDir(root.path()).filePath(QStringLiteral("data"));
            const QString stateHome = QDir(root.path()).filePath(QStringLiteral("state"));
            const QString libraryPath = QDir(root.path()).filePath(QStringLiteral("library"));
            const QString markerPath = QDir(root.path()).filePath(QStringLiteral("checkpoint"));
            QVERIFY(QDir().mkpath(dataHome));
            QVERIFY(QDir().mkpath(stateHome));
            QVERIFY(QDir().mkpath(libraryPath));
            QVERIFY(writeLibrary(libraryPath));

            QProcess interruptedSave;
            interruptedSave.setProcessEnvironment(testEnvironment(dataHome, stateHome));
            interruptedSave.start(workerPath,
                {QStringLiteral("--worker-save"), libraryPath,
                 QString::fromUtf8(oldChapter), QString::fromUtf8(newChapter),
                 checkpointArgument(checkpoint), markerPath});
            QVERIFY(interruptedSave.waitForStarted());
            QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(markerPath), 10000);
            interruptedSave.kill();
            QVERIFY(interruptedSave.waitForFinished(5000));
            QCOMPARE(interruptedSave.exitStatus(), QProcess::CrashExit);

            QProcess reopened;
            reopened.setProcessEnvironment(testEnvironment(dataHome, stateHome));
            reopened.start(workerPath,
                {QStringLiteral("--worker-open"), libraryPath,
                 QString::fromUtf8(newChapter), QStringLiteral("yes")});
            QVERIFY(reopened.waitForStarted());
            QVERIFY(reopened.waitForFinished(10000));
            QVERIFY2(reopened.exitCode() == 0,
                     qPrintable(QStringLiteral("%1: %2 %3")
                         .arg(checkpointArgument(checkpoint),
                              QString::fromLocal8Bit(reopened.readAllStandardOutput()),
                              QString::fromLocal8Bit(reopened.readAllStandardError()))));
        }
    }

    void reportedSuccessSurvivesProcessRestart()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString dataHome = QDir(root.path()).filePath(QStringLiteral("data"));
        const QString stateHome = QDir(root.path()).filePath(QStringLiteral("state"));
        const QString libraryPath = QDir(root.path()).filePath(QStringLiteral("library"));
        QVERIFY(QDir().mkpath(dataHome));
        QVERIFY(QDir().mkpath(stateHome));
        QVERIFY(QDir().mkpath(libraryPath));
        QVERIFY(writeLibrary(libraryPath));

        const QString workerPath = QCoreApplication::applicationFilePath();
        QProcess save;
        save.setProcessEnvironment(testEnvironment(dataHome, stateHome));
        save.start(workerPath,
            {QStringLiteral("--worker-save"), libraryPath,
             QString::fromUtf8(oldChapter), QString::fromUtf8(newChapter),
             QStringLiteral("none"), QString()});
        QVERIFY(save.waitForStarted());
        QVERIFY(save.waitForFinished(10000));
        QVERIFY2(save.exitCode() == 0,
                 qPrintable(QString::fromLocal8Bit(save.readAllStandardError())));

        QProcess reopened;
        reopened.setProcessEnvironment(testEnvironment(dataHome, stateHome));
        reopened.start(workerPath,
            {QStringLiteral("--worker-open"), libraryPath,
             QString::fromUtf8(newChapter), QStringLiteral("no")});
        QVERIFY(reopened.waitForStarted());
        QVERIFY(reopened.waitForFinished(10000));
        QVERIFY2(reopened.exitCode() == 0,
                 qPrintable(QString::fromLocal8Bit(reopened.readAllStandardError())));
    }
};

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    if (argc > 1 && std::strcmp(argv[1], "--worker-save") == 0) {
        return workerSave(argc, argv);
    }
    if (argc > 1 && std::strcmp(argv[1], "--worker-open") == 0) {
        return workerOpen(argc, argv);
    }
    LibraryPersistenceTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "library_persistence_test.moc"
