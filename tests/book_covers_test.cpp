#include "book_covers.h"
#include "ai_covers.h"
#include "library_persistence.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>

class BookCoversTest : public QObject {
    Q_OBJECT
private slots:
    void importFailureKeepsPriorCover();
    void modeSwitchAndRepaintPersist();
    void generatedArtworkPreservesImportedCoverAndExport();
    void unavailableSecretProviderReportsFailureAndKeepsCover();
};

static void write(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(bytes), bytes.size());
}

void BookCoversTest::importFailureKeepsPriorCover()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(QDir().mkpath(root.path() + QStringLiteral("/book-one")));
    const QString metadataPath = root.path() + QStringLiteral("/book-one/book.json");
    write(metadataPath, R"({"id":"book-one","title":"First","author":"Ada","chapterOrder":[]})");
    QImage image(20, 30, QImage::Format_RGB32);
    image.fill(Qt::red);
    const QString source = root.path() + QStringLiteral("/source.png");
    QVERIFY(image.save(source));
    const CoverResult imported = BookCovers::importImage(root.path(), QStringLiteral("book-one"), source);
    QVERIFY2(imported.ok, qPrintable(imported.error));
    QFile file(metadataPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray before = file.readAll();
    file.close();
    const QString filename = QJsonDocument::fromJson(before).object().value(QStringLiteral("coverImage")).toString();
    QVERIFY(QFile::exists(root.path() + QStringLiteral("/book-one/") + filename));
    QVERIFY(BookCovers::setMode(root.path(), QStringLiteral("book-one"), CoverMode::Abstract).ok);
    QVERIFY(BookCovers::render(root.path(), QStringLiteral("book-one"), QSize(20, 30)).pixelColor(10, 10) != QColor(Qt::red));
    QCOMPARE(BookCovers::exportCover(root.path(), QStringLiteral("book-one")).pixelColor(10, 10), QColor(Qt::red));
    QVERIFY(BookCovers::setMode(root.path(), QStringLiteral("book-one"), CoverMode::Image).ok);
    QCOMPARE(BookCovers::exportCover(root.path(), QStringLiteral("book-one")).pixelColor(10, 10), QColor(Qt::red));
    write(source, QByteArrayLiteral("not an image"));
    QVERIFY(!BookCovers::importImage(root.path(), QStringLiteral("book-one"), source).ok);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), before);
    file.close();
    QVERIFY(BookCovers::setMode(root.path(), QStringLiteral("book-one"), CoverMode::Abstract).ok);
    write(root.path() + QStringLiteral("/book-one/") + filename, QByteArrayLiteral("corrupt"));
    QVERIFY(!BookCovers::setMode(root.path(), QStringLiteral("book-one"), CoverMode::Image).ok);
    QVERIFY(BookCovers::render(root.path(), QStringLiteral("book-one"), QSize(20, 30)).pixelColor(10, 10) != QColor(Qt::red));
}

void BookCoversTest::modeSwitchAndRepaintPersist()
{
    QTemporaryDir root;
    QVERIFY(QDir().mkpath(root.path() + QStringLiteral("/book-one")));
    const QString metadataPath = root.path() + QStringLiteral("/book-one/book.json");
    write(metadataPath, R"({"id":"book-one","title":"First","author":"Ada","chapterOrder":[]})");
    QVERIFY(BookCovers::repaint(root.path(), QStringLiteral("book-one")).ok);
    QFile file(metadataPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject first = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    QVERIFY(!first.value(QStringLiteral("coverSeed")).toString().isEmpty());
    QVERIFY(BookCovers::repaint(root.path(), QStringLiteral("book-one")).ok);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject second = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    QVERIFY(first.value(QStringLiteral("coverSeed")) != second.value(QStringLiteral("coverSeed")));
    QVERIFY(!BookCovers::setMode(root.path(), QStringLiteral("book-one"), CoverMode::Image).ok);
    QVERIFY(BookCovers::removeImage(root.path(), QStringLiteral("book-one")).ok);
    QVERIFY(!BookCovers::render(root.path(), QStringLiteral("book-one"), QSize(72, 108)).isNull());
}

void BookCoversTest::generatedArtworkPreservesImportedCoverAndExport()
{
    QTemporaryDir root;
    QVERIFY(QDir().mkpath(root.path() + QStringLiteral("/book-one")));
    const QString metadataPath = root.path() + QStringLiteral("/book-one/book.json");
    write(metadataPath, R"({"id":"book-one","title":"First","author":"Ada","chapterOrder":[]})");
    QImage imported(20, 30, QImage::Format_RGB32);
    imported.fill(Qt::red);
    const QString source = root.path() + QStringLiteral("/source.png");
    QVERIFY(imported.save(source));
    QVERIFY(BookCovers::importImage(root.path(), QStringLiteral("book-one"), source).ok);
    QImage painted(20, 30, QImage::Format_RGB32);
    painted.fill(Qt::blue);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    QVERIFY(buffer.open(QIODevice::WriteOnly));
    QVERIFY(painted.save(&buffer, "PNG"));
    QVERIFY(BookCovers::savePainting(root.path(), QStringLiteral("book-one"), bytes).ok);
    QCOMPARE(BookCovers::render(root.path(), QStringLiteral("book-one"), QSize(20, 30))
                 .pixelColor(10, 10), QColor(Qt::red));
    QVERIFY(BookCovers::setMode(root.path(), QStringLiteral("book-one"), CoverMode::Painted).ok);
    QCOMPARE(BookCovers::render(root.path(), QStringLiteral("book-one"), QSize(20, 30))
                 .pixelColor(10, 10), QColor(Qt::blue));
    QCOMPARE(BookCovers::exportCover(root.path(), QStringLiteral("book-one"))
                 .pixelColor(10, 10), QColor(Qt::red));
    QFile file(metadataPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray before = file.readAll();
    file.close();
    QVERIFY(!BookCovers::savePainting(root.path(), QStringLiteral("book-one"),
                                      QByteArrayLiteral("invalid image")).ok);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), before);
}

void BookCoversTest::unavailableSecretProviderReportsFailureAndKeepsCover()
{
    QTemporaryDir root;
    QTemporaryDir emptyPath;
    QVERIFY(root.isValid());
    QVERIFY(emptyPath.isValid());
    QVERIFY(QDir().mkpath(root.path() + QStringLiteral("/book-one/chapters")));
    const QByteArray metadata = QByteArrayLiteral(
        "{\"id\":\"book-one\",\"coverMode\":\"abstract\",\"coverSeed\":\"original\",\"chapterOrder\":[\"one\"]}");
    const QString metadataPath = root.path() + QStringLiteral("/book-one/book.json");
    write(metadataPath, metadata);
    write(root.path() + QStringLiteral("/book-one/chapters/one.html"),
          QByteArrayLiteral("<p>") + QByteArrayLiteral("word ").repeated(1000) + QByteArrayLiteral("</p>"));
    const QImage before = BookCovers::render(root.path(), QStringLiteral("book-one"), QSize(72, 108));
    const bool pathWasSet = qEnvironmentVariableIsSet("PATH");
    const QByteArray originalPath = qgetenv("PATH");
    const auto restorePath = qScopeGuard([pathWasSet, originalPath] {
        if (pathWasSet) qputenv("PATH", originalPath);
        else qunsetenv("PATH");
    });
    QVERIFY(qputenv("PATH", emptyPath.path().toUtf8()));
    AiCovers service;
    int callbacks = 0;
    bool succeeded = true;
    QString message;
    const auto completed = [&](bool ok, const QString &result) {
        ++callbacks;
        succeeded = ok;
        message = result;
    };
    service.storeKey(QStringLiteral("test-only-dummy-key"), completed);
    QTRY_COMPARE_WITH_TIMEOUT(callbacks, 1, 5000);
    QVERIFY(!succeeded);
    QVERIFY(!message.isEmpty());
    service.generate(root.path(), QStringLiteral("book-one"), completed);
    QTRY_COMPARE_WITH_TIMEOUT(callbacks, 2, 5000);
    QVERIFY(!succeeded);
    QVERIFY(message.contains(QStringLiteral("secret store")));
    service.generate(root.path(), QStringLiteral("book-one"), completed);
    QTRY_COMPARE_WITH_TIMEOUT(callbacks, 3, 5000);
    QVERIFY(!succeeded);
    QVERIFY(message.contains(QStringLiteral("secret store")));
    QFile file(metadataPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), metadata);
    QCOMPARE(BookCovers::render(root.path(), QStringLiteral("book-one"), QSize(72, 108)), before);
}

QTEST_MAIN(BookCoversTest)
#include "book_covers_test.moc"
