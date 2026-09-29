#include "darling_records.h"
#include "chapter_structure.h"
#include "legacy_chapter_codec.h"
#include "library_persistence.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QtTest>

namespace {

void writeFile(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        qFatal("Could not write Darling fixture");
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) qFatal("Could not read Darling fixture");
    return file.readAll();
}

void makeBook(const QString &root)
{
    const QString book = root + QStringLiteral("/book-1/");
    writeFile(book + QStringLiteral("book.json"), QByteArrayLiteral(
        R"({"id":"book-1","chapterOrder":["a","b"],"chapterTitles":{"a":"Alpha","b":"Beta"}})"));
    writeFile(book + QStringLiteral("darlings.json"), QByteArrayLiteral("[]"));
    writeFile(book + QStringLiteral("stickies.json"), QByteArrayLiteral("[]"));
    writeFile(book + QStringLiteral("chapters/a.html"), QByteArrayLiteral(
        "<p>Before <b>bright</b> middle.</p><p>Second line.</p><p>After.</p>"));
    writeFile(book + QStringLiteral("chapters/b.html"), QByteArrayLiteral("<p>Other.</p>"));
}

} // namespace

class DarlingRecordsTest final : public QObject {
    Q_OBJECT
private slots:
    void multiParagraphCutReopenAndRestoreExactlyOnce()
    {
        QTemporaryDir library;
        QVERIFY(library.isValid());
        makeBook(library.path());
        const QString chapterPath = library.path() + QStringLiteral("/book-1/chapters/a.html");
        const auto original = LegacyChapterCodec::decode(readFile(chapterPath));
        const int start = original.text.indexOf(QStringLiteral("bright"));
        const int end = original.text.indexOf(QStringLiteral("After."));
        DarlingRecords darlings(library.path(), QStringLiteral("book-1"));
        const DarlingResult cut = darlings.cut(QStringLiteral("a"), start, end);
        QVERIFY2(cut.ok, qPrintable(cut.error));
        QJsonArray records;
        QString error;
        DarlingRecords reopened(library.path(), QStringLiteral("book-1"));
        QVERIFY2(reopened.list(&records, &error), qPrintable(error));
        QCOMPARE(records.size(), 1);
        const QJsonObject record = records.at(0).toObject();
        QCOMPARE(record.value(QStringLiteral("chapterId")).toString(), QStringLiteral("a"));
        QCOMPARE(record.value(QStringLiteral("chapterLabel")).toString(), QStringLiteral("Alpha"));
        QVERIFY(record.value(QStringLiteral("html")).toString().contains(QStringLiteral("<b>bright</b>")));
        QCOMPARE(record.value(QStringLiteral("text")).toString(),
                 original.text.mid(start, end - start));
        QVERIFY(record.value(QStringLiteral("anchorPrefix")).isString());
        QVERIFY(record.value(QStringLiteral("anchorSuffix")).isString());
        const DarlingResult restored = reopened.restore(record.value(QStringLiteral("id")).toString());
        QVERIFY2(restored.ok, qPrintable(restored.error));
        QCOMPARE(LegacyChapterCodec::decode(readFile(chapterPath)).text, original.text);
        QVERIFY(readFile(chapterPath).contains(QByteArrayLiteral("<b>bright</b>")));
        QVERIFY(reopened.list(&records, &error));
        QCOMPARE(records.size(), 0);
    }

    void missingAnchorNeedsReviewAndKeepsRecord()
    {
        QTemporaryDir library;
        QVERIFY(library.isValid());
        makeBook(library.path());
        DarlingRecords darlings(library.path(), QStringLiteral("book-1"));
        const DarlingResult cut = darlings.cut(QStringLiteral("a"), 7, 13);
        QVERIFY2(cut.ok, qPrintable(cut.error));
        const QString path = library.path() + QStringLiteral("/book-1/chapters/a.html");
        writeFile(path, QByteArrayLiteral("<p>Changed surroundings.</p>"));
        QJsonArray records;
        QString error;
        QVERIFY(darlings.list(&records, &error));
        const QString id = records.at(0).toObject().value(QStringLiteral("id")).toString();
        const DarlingResult refused = darlings.restore(id);
        QVERIFY(!refused.ok);
        QVERIFY(refused.review);
        QCOMPARE(readFile(path), QByteArrayLiteral("<p>Changed surroundings.</p>"));
        QVERIFY(darlings.list(&records, &error));
        QCOMPARE(records.size(), 1);
    }

    void deletedChapterAndStructuralUndoKeepOneActiveCopy()
    {
        QTemporaryDir library;
        QVERIFY(library.isValid());
        makeBook(library.path());
        ChapterStructure structure(library.path(), QStringLiteral("book-1"));
        QString error;
        QVERIFY2(structure.load(&error), qPrintable(error));
        const QString chapterPath = library.path() + QStringLiteral("/book-1/chapters/a.html");
        const QByteArray bytes = readFile(chapterPath);
        const ChapterStructureResult deleted = structure.deleteChapter(
            QStringLiteral("a"), {bytes, LibraryPersistence::hash(bytes), bytes});
        QVERIFY2(deleted.ok, qPrintable(deleted.error));
        DarlingRecords darlings(library.path(), QStringLiteral("book-1"));
        QJsonArray records;
        QVERIFY(darlings.list(&records, &error));
        QCOMPARE(records.size(), 1);
        QCOMPARE(records.at(0).toObject().value(QStringLiteral("chapterId")).toString(),
                 QStringLiteral("a"));
        QVERIFY2(structure.undo().ok, "Structural undo should restore the chapter");
        QVERIFY(darlings.list(&records, &error));
        QCOMPARE(records.size(), 0);
        QCOMPARE(readFile(chapterPath), bytes);
    }

    void deletionRefusesUnopenableDarlings()
    {
        QTemporaryDir library;
        QVERIFY(library.isValid());
        makeBook(library.path());
        const QString chapterPath = library.path() + QStringLiteral("/book-1/chapters/a.html");
        const QByteArray original = readFile(chapterPath);
        const QString darlingsPath = library.path() + QStringLiteral("/book-1/darlings.json");
        const QByteArray malformed = QByteArrayLiteral(R"([{"id":"same"},{"id":"same"}])");
        writeFile(darlingsPath, malformed);
        ChapterStructure structure(library.path(), QStringLiteral("book-1"));
        QString error;
        QVERIFY2(structure.load(&error), qPrintable(error));
        QVERIFY(!structure.deleteChapter(QStringLiteral("a"),
            {original, LibraryPersistence::hash(original), original}).ok);
        QCOMPARE(readFile(chapterPath), original);
        QCOMPARE(readFile(darlingsPath), malformed);
    }

    void deletedChapterRestoreRequiresReviewAndCannotBeUndoneTwice()
    {
        QTemporaryDir library;
        QVERIFY(library.isValid());
        makeBook(library.path());
        ChapterStructure structure(library.path(), QStringLiteral("book-1"));
        QString error;
        QVERIFY2(structure.load(&error), qPrintable(error));
        const QString originalPath = library.path() + QStringLiteral("/book-1/chapters/a.html");
        const QByteArray originalBytes = readFile(originalPath);
        QVERIFY(structure.deleteChapter(QStringLiteral("a"),
            {originalBytes, LibraryPersistence::hash(originalBytes), originalBytes}).ok);
        DarlingRecords darlings(library.path(), QStringLiteral("book-1"));
        QJsonArray records;
        QVERIFY(darlings.list(&records, &error));
        const QString id = records.at(0).toObject().value(QStringLiteral("id")).toString();
        const DarlingResult unreviewed = darlings.restore(id);
        QVERIFY(!unreviewed.ok);
        QVERIFY(unreviewed.review);
        const DarlingResult restored = darlings.restore(id, true);
        QVERIFY2(restored.ok, qPrintable(restored.error));
        QCOMPARE(restored.chapterId, QStringLiteral("b"));
        const QString activeText = LegacyChapterCodec::decode(readFile(
            library.path() + QStringLiteral("/book-1/chapters/b.html"))).text;
        QCOMPARE(activeText.count(QStringLiteral("Before bright middle.")), 1);
        QCOMPARE(activeText.count(QStringLiteral("Second line.")), 1);
        QVERIFY(darlings.list(&records, &error));
        QCOMPARE(records.size(), 0);
        QVERIFY(!structure.undo().ok);
        QCOMPARE(LegacyChapterCodec::decode(readFile(
            library.path() + QStringLiteral("/book-1/chapters/b.html"))).text, activeText);
        QVERIFY(darlings.list(&records, &error));
        QCOMPARE(records.size(), 0);
    }

    void legacyDarlingWithMismatchedHtmlStaysAvailable()
    {
        QTemporaryDir library;
        QVERIFY(library.isValid());
        makeBook(library.path());
        const QString chapter = library.path() + QStringLiteral("/book-1/chapters/a.html");
        const QByteArray before = readFile(chapter);
        writeFile(library.path() + QStringLiteral("/book-1/darlings.json"), QByteArrayLiteral(
            R"([{"id":"old","chapterId":"a","html":"<p>Different text</p>","text":"Saved text","anchorPrefix":"Before ","anchorSuffix":"bright"}])"));
        DarlingRecords darlings(library.path(), QStringLiteral("book-1"));
        const DarlingResult restored = darlings.restore(QStringLiteral("old"));
        QVERIFY(!restored.ok);
        QCOMPARE(readFile(chapter), before);
        QJsonArray records;
        QString error;
        QVERIFY(darlings.list(&records, &error));
        QCOMPARE(records.size(), 1);
    }

    void legacyDeletedChapterRestoresAllHtmlAfterReview()
    {
        QTemporaryDir library;
        QVERIFY(library.isValid());
        makeBook(library.path());
        const QString metadata = library.path() + QStringLiteral("/book-1/book.json");
        writeFile(metadata, QByteArrayLiteral(
            R"({"id":"book-1","chapterOrder":["b"],"chapterTitles":{"b":"Beta"}})"));
        writeFile(library.path() + QStringLiteral("/book-1/darlings.json"), QByteArrayLiteral(
            R"([{"id":"old-deleted","chapterId":null,"chapterLabel":"deleted Chapter 1","html":"<p>Entire old chapter.</p><p>Second paragraph.</p>","text":"Entire old"}])"));
        DarlingRecords darlings(library.path(), QStringLiteral("book-1"));
        QVERIFY(darlings.restore(QStringLiteral("old-deleted")).review);
        const DarlingResult restored = darlings.restore(QStringLiteral("old-deleted"), true);
        QVERIFY2(restored.ok, qPrintable(restored.error));
        const QString text = LegacyChapterCodec::decode(readFile(
            library.path() + QStringLiteral("/book-1/chapters/b.html"))).text;
        QCOMPARE(text, QStringLiteral("Other.\nEntire old chapter.\nSecond paragraph."));
    }
};

QTEST_MAIN(DarlingRecordsTest)
#include "darling_records_test.moc"
