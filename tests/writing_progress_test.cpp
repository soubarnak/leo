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
    void initTestCase()
    {
        // Conflict drafts and safety snapshots must never land in the real user data directory.
        QVERIFY(dataHome_.isValid());
        qputenv("XDG_DATA_HOME", dataHome_.path().toLocal8Bit());
    }
    void countsAndHistorySurviveReopen();
    void lateSessionAndClockRollbackUseOneDay();
    void reopenAfterClockRollbackKeepsLastWritingDay();
    void wordsSeparatedByParagraphBreaksOrNonBreakingSpacesAreCounted();
    void writingDayBoundaryIsConfigurableAndExclusiveAtHour();
    void refreshDayFollowsClockWithoutSaving();
    void savesSurviveOtherLibraryRewritesAfterLoad();
    void sprintCompletionSurvivesLaterDeletions();
    void sprintCountReductionBeforeTargetKeepsSprintRunning();
    void sprintCancelOnlyEndsRunningSprint();
    void sprintIsScopedToItsBook();

private:
    QTemporaryDir dataHome_;
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

void WritingProgressTest::reopenAfterClockRollbackKeepsLastWritingDay()
{
    QTemporaryDir library;
    QVERIFY(library.isValid());
    writeFixture(library.path(), "library.json", R"({"dayEndsAt":3})");
    writeFixture(library.path(), "book/book.json", R"({"chapterOrder":[]})");
    QString error;
    WritingProgress first(library.path());
    QVERIFY(first.load("book", &error, QDateTime::fromString("2026-10-01T04:00:00", Qt::ISODate)));
    QVERIFY(first.updateCount(20, QDateTime::fromString("2026-10-01T04:00:00", Qt::ISODate), &error));
    // Clock rolled back before the recorded day, then the book is reopened.
    WritingProgress reopened(library.path());
    QVERIFY(reopened.load("book", &error, QDateTime::fromString("2026-09-30T22:00:00", Qt::ISODate)));
    QCOMPARE(reopened.snapshot().day, QDate(2026, 10, 1));
    QVERIFY(reopened.updateCount(25, QDateTime::fromString("2026-09-30T22:05:00", Qt::ISODate), &error));
    QCOMPARE(reopened.snapshot().history.size(), 1);
    QCOMPARE(reopened.snapshot().history.value(QDate(2026, 10, 1)).start, 0);
    QCOMPARE(reopened.snapshot().history.value(QDate(2026, 10, 1)).end, 25);
    QCOMPARE(reopened.snapshot().todayWords(), 25);
}

void WritingProgressTest::wordsSeparatedByParagraphBreaksOrNonBreakingSpacesAreCounted()
{
    // QTextCursor::selectedText() joins lines with U+2029; manuscripts use U+00A0.
    QCOMPARE(WritingProgress::countWords(QStringLiteral("alpha\u2029beta")), 2);
    QCOMPARE(WritingProgress::countWords(QStringLiteral("alpha\u00A0beta")), 2);
    QCOMPARE(WritingProgress::countWords(QStringLiteral("alpha\nbeta\tgamma")), 3);
}

void WritingProgressTest::writingDayBoundaryIsConfigurableAndExclusiveAtHour()
{
    const auto at = [](const char *iso) { return QDateTime::fromString(QString::fromLatin1(iso), Qt::ISODate); };
    QCOMPARE(WritingProgress::writingDay(at("2026-10-01T02:59:59"), 3), QDate(2026, 9, 30));
    QCOMPARE(WritingProgress::writingDay(at("2026-10-01T03:00:00"), 3), QDate(2026, 10, 1));
    QCOMPARE(WritingProgress::writingDay(at("2026-10-01T00:30:00"), 0), QDate(2026, 10, 1));
    QCOMPARE(WritingProgress::writingDay(at("2026-10-01T05:30:00"), 6), QDate(2026, 9, 30));
}

void WritingProgressTest::refreshDayFollowsClockWithoutSaving()
{
    QTemporaryDir library;
    QVERIFY(library.isValid());
    writeFixture(library.path(), "library.json", R"({"dayEndsAt":3})");
    writeFixture(library.path(), "book/book.json", R"({"chapterOrder":[]})");
    QString error;
    WritingProgress progress(library.path());
    QVERIFY(progress.load("book", &error, QDateTime::fromString("2026-09-30T23:00:00", Qt::ISODate)));
    QCOMPARE(progress.snapshot().day, QDate(2026, 9, 30));
    // Open past the boundary with no save: "today" must move to the new writing day.
    progress.refreshDay(QDateTime::fromString("2026-10-01T04:00:00", Qt::ISODate));
    QCOMPARE(progress.snapshot().day, QDate(2026, 10, 1));
}

void WritingProgressTest::savesSurviveOtherLibraryRewritesAfterLoad()
{
    QTemporaryDir library;
    QVERIFY(library.isValid());
    writeFixture(library.path(), "library.json", R"({"dayEndsAt":3})");
    writeFixture(library.path(), "book/book.json", R"({"chapterOrder":[]})");
    QString error;
    WritingProgress progress(library.path());
    QVERIFY(progress.load("book", &error));
    // Preferences, spellcheck and cover edits rewrite these files after progress loaded.
    writeFixture(library.path(), "library.json", R"({"dayEndsAt":3,"spellcheck":true})");
    writeFixture(library.path(), "book/book.json", R"({"chapterOrder":[],"coverAdded":true})");
    QVERIFY2(progress.setGoals(120, 1000, 3, &error), qPrintable(error));
    QVERIFY2(progress.updateCount(7, QDateTime::fromString("2026-10-01T10:00:00", Qt::ISODate), &error),
             qPrintable(error));
    WritingProgress reopened(library.path());
    QVERIFY2(reopened.load("book", &error), qPrintable(error));
    QCOMPARE(reopened.snapshot().dailyGoal, 120);
    QCOMPARE(reopened.snapshot().bookGoal, 1000);
    QCOMPARE(reopened.snapshot().bookWords, 7);
    QFile libraryFile(QDir(library.path()).filePath("library.json"));
    QVERIFY(libraryFile.open(QIODevice::ReadOnly));
    QVERIFY(QJsonDocument::fromJson(libraryFile.readAll()).object().value("spellcheck").toBool());
    QFile bookFile(QDir(library.path()).filePath("book/book.json"));
    QVERIFY(bookFile.open(QIODevice::ReadOnly));
    QVERIFY(QJsonDocument::fromJson(bookFile.readAll()).object().value("coverAdded").toBool());
}

void WritingProgressTest::sprintCompletionSurvivesLaterDeletions()
{
    const QString book = QStringLiteral("book");
    WritingSprint sprint;
    sprint.start(book, 100, 5);
    QVERIFY(sprint.running(book));
    sprint.observe(book, 103);
    QVERIFY(sprint.running(book));
    QCOMPARE(sprint.progress(103), 3);
    sprint.observe(book, 105);
    QVERIFY(sprint.complete(book));
    QVERIFY(!sprint.running(book));
    QCOMPARE(sprint.progress(105), 5);
    sprint.observe(book, 96);
    QVERIFY(sprint.complete(book));
    QCOMPARE(sprint.progress(96), 5);
    sprint.start(book, 96, 5);
    QVERIFY(sprint.running(book));
    QCOMPARE(sprint.progress(96), 0);
}

void WritingProgressTest::sprintCountReductionBeforeTargetKeepsSprintRunning()
{
    const QString book = QStringLiteral("book");
    WritingSprint sprint;
    sprint.start(book, 100, 5);
    sprint.observe(book, 90);
    QVERIFY(sprint.running(book));
    QVERIFY(!sprint.complete(book));
    QCOMPARE(sprint.progress(90), -10);
}

void WritingProgressTest::sprintCancelOnlyEndsRunningSprint()
{
    const QString book = QStringLiteral("book");
    WritingSprint sprint;
    sprint.start(book, 0, 5);
    sprint.observe(book, 2);
    sprint.cancel();
    QVERIFY(!sprint.running(book));
    QVERIFY(!sprint.complete(book));
    sprint.start(book, 0, 5);
    sprint.observe(book, 5);
    QVERIFY(sprint.complete(book));
    sprint.cancel();
    QVERIFY(sprint.complete(book));
}

void WritingProgressTest::sprintIsScopedToItsBook()
{
    WritingSprint sprint;
    sprint.start(QStringLiteral("a"), 0, 5);
    sprint.observe(QStringLiteral("b"), 50);
    QVERIFY(sprint.running(QStringLiteral("a")));
    QVERIFY(!sprint.running(QStringLiteral("b")));
    QVERIFY(!sprint.complete(QStringLiteral("a")));
    QCOMPARE(sprint.target(), 5);
}

QTEST_GUILESS_MAIN(WritingProgressTest)
#include "writing_progress_test.moc"
