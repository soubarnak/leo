#include "app_paths.h"
#include "release_check_dialog.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
#include <QTimer>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

#include <cstring>
#include <utility>

namespace {

class SavedEnvironment final {
public:
    explicit SavedEnvironment(const QStringList &names)
    {
        for (const QString &name : names) {
            const QByteArray key = name.toLocal8Bit();
            if (qEnvironmentVariableIsSet(key.constData())) {
                values_.insert(key, qgetenv(key.constData()));
                wasSet_.insert(key);
            }
        }
    }

    ~SavedEnvironment()
    {
        for (auto it = values_.cbegin(); it != values_.cend(); ++it) {
            qputenv(it.key().constData(), it.value());
        }
        for (const QByteArray &key : wasUnset_) {
            qunsetenv(key.constData());
        }
    }

    void set(const char *name, const QByteArray &value)
    {
        const QByteArray key(name);
        if (!wasSet_.contains(key) && !values_.contains(key)) {
            wasUnset_.insert(key);
        }
        qputenv(name, value);
    }

private:
    QHash<QByteArray, QByteArray> values_;
    QSet<QByteArray> wasSet_;
    QSet<QByteArray> wasUnset_;
};

class StubNetworkReply final : public QNetworkReply {
public:
    StubNetworkReply(const QNetworkRequest &request,
                     QByteArray responseBody,
                     QObject *parent = nullptr)
        : QNetworkReply(parent)
        , responseBody_(std::move(responseBody))
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::GetOperation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 200);
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(0, this, [this] {
            setFinished(true);
            emit readyRead();
            emit finished();
        });
    }

    void abort() override
    {
        setError(QNetworkReply::OperationCanceledError, QStringLiteral("Request cancelled"));
        setFinished(true);
        emit finished();
    }

    bool isSequential() const override
    {
        return true;
    }

    qint64 bytesAvailable() const override
    {
        return responseBody_.size() - readOffset_ + QNetworkReply::bytesAvailable();
    }

protected:
    qint64 readData(char *data, qint64 maximumSize) override
    {
        const qint64 remaining = responseBody_.size() - readOffset_;
        if (remaining <= 0) {
            return -1;
        }
        const qint64 count = qMin(maximumSize, remaining);
        std::memcpy(data, responseBody_.constData() + readOffset_, static_cast<size_t>(count));
        readOffset_ += count;
        return count;
    }

private:
    QByteArray responseBody_;
    qint64 readOffset_ = 0;
};

class StubNetworkAccessManager final : public QNetworkAccessManager {
public:
    explicit StubNetworkAccessManager(QByteArray responseBody, QObject *parent = nullptr)
        : QNetworkAccessManager(parent)
        , responseBody_(std::move(responseBody))
    {
    }

    QUrl requestedUrl() const
    {
        return requestedUrl_;
    }

protected:
    QNetworkReply *createRequest(Operation operation,
                                 const QNetworkRequest &request,
                                 QIODevice *outgoingData = nullptr) override
    {
        Q_UNUSED(operation);
        Q_UNUSED(outgoingData);
        requestedUrl_ = request.url();
        return new StubNetworkReply(request, responseBody_, this);
    }

private:
    QByteArray responseBody_;
    QUrl requestedUrl_;
};

bool writeLibrary(const QString &directory, const QByteArray &metadata)
{
    QFile file(QDir(directory).filePath(QStringLiteral("library.json")));
    return file.open(QIODevice::WriteOnly) && file.write(metadata) == metadata.size();
}

QProcessEnvironment testEnvironment(const QString &runtime, const QString &state)
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    // A desktop platform theme such as gtk3 opens the display through
    // XDG_RUNTIME_DIR; with a private runtime directory it fails and exits.
    environment.remove(QStringLiteral("QT_QPA_PLATFORMTHEME"));
    environment.remove(QStringLiteral("QT_QPA_PLATFORMTHEME_QT6"));
    environment.insert(QStringLiteral("XDG_RUNTIME_DIR"), runtime);
    environment.insert(QStringLiteral("XDG_STATE_HOME"), state);
    return environment;
}

void stop(QProcess *process)
{
    if (process->state() == QProcess::NotRunning) {
        return;
    }
    process->terminate();
    if (!process->waitForFinished(3000)) {
        process->kill();
        process->waitForFinished(3000);
    }
}

}

class ApplicationRuntimeTest final : public QObject {
    Q_OBJECT

private slots:
    void privateDirectoriesFollowXdgHomes()
    {
        SavedEnvironment environment({QStringLiteral("XDG_CONFIG_HOME"),
                                      QStringLiteral("XDG_DATA_HOME"),
                                      QStringLiteral("XDG_STATE_HOME"),
                                      QStringLiteral("XDG_CACHE_HOME")});
        environment.set("XDG_CONFIG_HOME", QByteArrayLiteral("/tmp/leo-config"));
        environment.set("XDG_DATA_HOME", QByteArrayLiteral("/tmp/leo-data"));
        environment.set("XDG_STATE_HOME", QByteArrayLiteral("/tmp/leo-state"));
        environment.set("XDG_CACHE_HOME", QByteArrayLiteral("/tmp/leo-cache"));

        QCOMPARE(AppPaths::configDirectory(), QStringLiteral("/tmp/leo-config/leo-writer"));
        QCOMPARE(AppPaths::dataDirectory(), QStringLiteral("/tmp/leo-data/leo-writer"));
        QCOMPARE(AppPaths::stateDirectory(), QStringLiteral("/tmp/leo-state/leo-writer"));
        QCOMPARE(AppPaths::cacheDirectory(), QStringLiteral("/tmp/leo-cache/leo-writer"));
    }

    void releaseCheckUsesLeoWriterGithubReleases()
    {
        QCOMPARE(ReleaseCheckDialog::latestReleaseApiUrl(),
                 QUrl(QStringLiteral("https://api.github.com/repos/soubarnak/leo/releases/latest")));
        QCOMPARE(ReleaseCheckDialog::releasePageUrl(),
                 QUrl(QStringLiteral("https://github.com/soubarnak/leo/releases")));
    }

    void releaseCheckReportsNewerVersionFromReleaseFeed()
    {
        StubNetworkAccessManager network(QByteArrayLiteral("{\"tag_name\":\"v0.2.0\"}"));
        ReleaseCheckDialog dialog(QStringLiteral("0.1.0"), nullptr, &network);
        dialog.show();
        auto *status = dialog.findChild<QLabel *>(QStringLiteral("release-check-status"));
        QVERIFY(status);
        QCOMPARE(network.requestedUrl(), ReleaseCheckDialog::latestReleaseApiUrl());
        QTRY_VERIFY_WITH_TIMEOUT(status->text().contains(QStringLiteral("LEO 0.2.0 is available")),
                                 5000);
        QVERIFY(status->text().contains(QStringLiteral("through APT")));
    }

    void secondLaunchReusesRunningInstance()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString runtime = QDir(temporary.path()).filePath(QStringLiteral("runtime"));
        const QString state = QDir(temporary.path()).filePath(QStringLiteral("state"));
        QVERIFY(QDir().mkpath(runtime));
        QVERIFY(QDir().mkpath(state));

        QTemporaryDir primaryWorkingDirectory;
        QVERIFY(primaryWorkingDirectory.isValid());
        QTemporaryDir secondaryWorkingDirectory;
        QVERIFY(secondaryWorkingDirectory.isValid());
        const QString requestedLibrary =
            QDir(secondaryWorkingDirectory.path()).filePath(QStringLiteral("requested-library"));
        QVERIFY(QDir().mkpath(requestedLibrary));

        QTemporaryDir library;
        QVERIFY(library.isValid());
        QVERIFY(writeLibrary(library.path(), QByteArray("{\"authors\":[],\"shelves\":[]}")));

        const QString executable = qEnvironmentVariable("LEO_WRITER_TEST_EXECUTABLE");
        QVERIFY(!executable.isEmpty());
        const QProcessEnvironment environment = testEnvironment(runtime, state);
        QProcess first;
        first.setProcessEnvironment(environment);
        first.setWorkingDirectory(primaryWorkingDirectory.path());
        first.start(executable, {library.path()});
        QVERIFY(first.waitForStarted());
        QTRY_VERIFY_WITH_TIMEOUT(first.state() == QProcess::Running, 5000);
        QElapsedTimer startupElapsed;
        startupElapsed.start();
        while (QDir(runtime).entryList({QStringLiteral("leo-writer-*.lock")}, QDir::Files).isEmpty() &&
               startupElapsed.elapsed() < 5000) {
            QTest::qWait(25);
        }
        const bool primaryReady =
            !QDir(runtime).entryList({QStringLiteral("leo-writer-*.lock")}, QDir::Files).isEmpty();
        if (!primaryReady) {
            stop(&first);
        }
        QVERIFY2(primaryReady, "First application did not acquire its single-instance lock");

        QProcess second;
        second.setProcessEnvironment(environment);
        second.setWorkingDirectory(secondaryWorkingDirectory.path());
        second.start(executable, {QStringLiteral("requested-library")});
        QVERIFY(second.waitForStarted());
        const bool secondExited = second.waitForFinished(5000);
        const int secondExitCode = second.exitCode();
        const QProcess::ProcessState firstState = first.state();

        const QString logPath = QDir(state).filePath(QStringLiteral("leo-writer/leo-writer.log"));
        const QByteArray expectedPath =
            QDir(requestedLibrary).filePath(QStringLiteral("library.json")).toUtf8();
        QByteArray logContents;
        QElapsedTimer requestElapsed;
        requestElapsed.start();
        while (!logContents.contains(expectedPath) && requestElapsed.elapsed() < 5000) {
            QFile log(logPath);
            if (log.open(QIODevice::ReadOnly)) {
                logContents = log.readAll();
            }
            if (!logContents.contains(expectedPath)) {
                QTest::qWait(25);
            }
        }

        stop(&second);
        stop(&first);
        QVERIFY2(secondExited, "Second launch started a second application process");
        QVERIFY2(secondExitCode == 0, second.readAllStandardError().constData());
        QCOMPARE(firstState, QProcess::Running);
        QVERIFY2(logContents.contains(expectedPath), logContents.constData());
    }

    void writesStartupErrorsToPrivateXdgState()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString runtime = QDir(temporary.path()).filePath(QStringLiteral("runtime"));
        const QString state = QDir(temporary.path()).filePath(QStringLiteral("state"));
        QVERIFY(QDir().mkpath(runtime));
        QVERIFY(QDir().mkpath(state));

        QTemporaryDir invalidLibrary;
        QVERIFY(invalidLibrary.isValid());
        const QString executable = qEnvironmentVariable("LEO_WRITER_TEST_EXECUTABLE");
        QVERIFY(!executable.isEmpty());
        QProcess process;
        process.setProcessEnvironment(testEnvironment(runtime, state));
        process.start(executable, {invalidLibrary.path()});
        QVERIFY(process.waitForStarted());

        const QString logPath = QDir(state).filePath(QStringLiteral("leo-writer/leo-writer.log"));
        QElapsedTimer elapsed;
        elapsed.start();
        while (!QFile::exists(logPath) && elapsed.elapsed() < 5000) {
            QTest::qWait(50);
        }
        QByteArray contents;
        QFile log(logPath);
        if (log.open(QIODevice::ReadOnly)) {
            contents = log.readAll();
        }

        stop(&process);
        QVERIFY2(QFile::exists(logPath), "Application did not create its private error log");
        QVERIFY2(contents.contains("Library open refused"), contents.constData());
        QVERIFY2(contents.contains("library.json"), contents.constData());
    }
};

QTEST_MAIN(ApplicationRuntimeTest)
#include "application_runtime_test.moc"
