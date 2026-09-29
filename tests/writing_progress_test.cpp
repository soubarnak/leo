#include "writing_progress.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

class WritingProgressTest final : public QObject {
    Q_OBJECT
private slots:
    void countsAndHistorySurviveReopen();
    void lateSessionAndClockRollbackUseOneDay();
};

static void writeFixture(const QString &path, const QString &relative, const QByteArray &bytes)
{
    const QString filePath = QDir(path).filePath(relative);
    QVERIFY(QDir().mkpath(QFileInfo(filePath).absolutePath()));
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(bytes), bytes.size());
}

void WritingProgressTest::countsAndHistorySurviveReopen()
{
    QTemporaryDir library;
    QVERIFY(library.isValid());
    writeFixture(library.path(), "library.json", R"({"dailyGoal":100,"dayEndsAt":3,"future":{"keep":true}})");
    writeFixture(library.path(), "book/book.json", R"({"chapterOrder":["one","two"],"future":{"keep":true}})");
    writeFixture(library.path(), "book/chapters/one.html", "<p>Alpha beta</p>");
    writeFixture(library.path(), "book/chapters/two.html", "<p>Gamma delta epsilon</p>");
    QString error;
    QCOMPARE(WritingProgress::countBook(library.path(), "book", &error), 5);
    WritingProgress progress(library.path());
    QVERIFY2(progress.load("book", &error), qPrintable(error));
    QCOMPARE(progress.snapshot().bookWords, 5);
    QVERIFY2(progress.updateCount(5, QDateTime::fromString("2026-09-30T12:00:00", Qt::ISODate), &error), qPrintable(error));
    QVERIFY2(progress.updateCount(8, QDateTime::fromString("2026-09-30T13:00:00", Qt::ISODate), &error), qPrintable(error));
    QVERIFY2(progress.setGoals(120, 1000, 3, &error), qPrintable(error));
    WritingProgress reopened(library.path());
    QVERIFY2(reopened.load("book", &error), qPrintable(error));
    QCOMPARE(reopened.snapshot().bookWords, 8);
    QCOMPARE(reopened.snapshot().dailyGoal, 120);
    QCOMPARE(reopened.snapshot().bookGoal, 1000);
    QCOMPARE(reopened.snapshot().history.value(QDate(2026, 9, 30)).end, 8);
    QCOMPARE(reopened.snapshot().history.value(QDate(2026, 9, 30)).start, 5);
    QFile bookFile(QDir(library.path()).filePath("book/book.json"));
    QVERIFY(bookFile.open(QIODevice::ReadOnly));
    QVERIFY(QJsonDocument::fromJson(bookFile.readAll()).object().value("future").toObject().value("keep").toBool());
}

void WritingProgressTest::lateSessionAndClockRollbackUseOneDay()
{
    QTemporaryDir library;
    QVERIFY(library.isValid());
    writeFixture(library.path(), "library.json", R"({"dayEndsAt":3})");
    writeFixture(library.path(), "book/book.json", R"({"chapterOrder":[]})");
    QString error;
    WritingProgress progress(library.path());
    QVERIFY(progress.load("book", &error));
    QVERIFY(progress.updateCount(10, QDateTime::fromString("2026-09-30T23:50:00", Qt::ISODate), &error));
    QVERIFY(progress.updateCount(12, QDateTime::fromString("2026-10-01T01:30:00", Qt::ISODate), &error));
    QCOMPARE(progress.snapshot().history.size(), 1);
    QCOMPARE(progress.snapshot().history.value(QDate(2026, 9, 30)).start, 0);
    QCOMPARE(progress.snapshot().history.value(QDate(2026, 9, 30)).end, 12);
    QVERIFY(progress.updateCount(14, QDateTime::fromString("2026-10-01T04:00:00", Qt::ISODate), &error));
    QVERIFY(progress.updateCount(15, QDateTime::fromString("2026-09-30T22:00:00", Qt::ISODate), &error));
    QCOMPARE(progress.snapshot().history.size(), 2);
    QCOMPARE(progress.snapshot().history.value(QDate(2026, 10, 1)).start, 12);
    QCOMPARE(progress.snapshot().history.value(QDate(2026, 10, 1)).end, 15);
    QCOMPARE(progress.snapshot().history.value(QDate(2026, 10, 1)).end -
             progress.snapshot().history.value(QDate(2026, 10, 1)).start, 3);
}

QTEST_GUILESS_MAIN(WritingProgressTest)
#include "writing_progress_test.moc"
