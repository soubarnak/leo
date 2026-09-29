#include "planning_records.h"
#include "chapter_links.h"
#include "legacy_chapter_codec.h"
#include "chapter_structure.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

namespace {
void writeFile(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        qFatal("Could not create planning fixture");
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) qFatal("Could not read planning fixture");
    return file.readAll();
}

QString path(const QTemporaryDir &library, const QString &relative)
{
    return QDir(library.path()).filePath(QStringLiteral("book-1/") + relative);
}

void makeLibrary(const QTemporaryDir &library)
{
    writeFile(QDir(library.path()).filePath(QStringLiteral("library.json")),
              QByteArrayLiteral(R"({"authors":[{"id":"a","name":"Ada"}],"shelves":[]})"));
    writeFile(path(library, QStringLiteral("book.json")),
              QByteArrayLiteral(R"({"id":"book-1","chapterOrder":["a","b"],"chapterTitles":{"a":"One","b":"Two"}})"));
    writeFile(path(library, QStringLiteral("chapters/a.html")), QByteArrayLiteral("<p>One.</p>"));
    writeFile(path(library, QStringLiteral("chapters/b.html")), QByteArrayLiteral("<p>Two.</p>"));
    writeFile(path(library, QStringLiteral("stickies.json")), QByteArrayLiteral("[]"));
    writeFile(path(library, QStringLiteral("darlings.json")), QByteArrayLiteral("[]"));
}

void verifyChapter(const QTemporaryDir &library, const QString &chapterId)
{
    const QByteArray bytes = readFile(path(library, QStringLiteral("chapters/") + chapterId +
                                             QStringLiteral(".html")));
    const LegacyChapterDocument document = LegacyChapterCodec::decode(
        bytes, loadChapterLinks(library.path(), QStringLiteral("book-1"), chapterId));
    QVERIFY2(document.refusalReason.isEmpty(), qPrintable(document.refusalReason));
}
} // namespace

class PlanningRecordsTest final : public QObject {
    Q_OBJECT
private slots:
    void stickyAndSectionStayLinkedThroughCopyMoveUndoAndReopen()
    {
        QTemporaryDir data, state, library;
        QVERIFY(data.isValid() && state.isValid() && library.isValid());
        qputenv("XDG_DATA_HOME", data.path().toLocal8Bit());
        qputenv("XDG_STATE_HOME", state.path().toLocal8Bit());
        makeLibrary(library);
        PlanningRecords planning(library.path(), QStringLiteral("book-1"));

        const auto sticky = planning.addSticky(QStringLiteral("a"), QStringLiteral("Check this"));
        QVERIFY2(sticky.ok, qPrintable(sticky.error));
        verifyChapter(library, QStringLiteral("a"));
        const auto section = planning.addSection(QStringLiteral("a"), QStringLiteral("Unwritten scene"));
        QVERIFY2(section.ok, qPrintable(section.error));
        verifyChapter(library, QStringLiteral("a"));
        const auto editedSection = planning.updateSection(
            QStringLiteral("a"), section.id, QStringLiteral("A revised scene"));
        QVERIFY2(editedSection.ok, qPrintable(editedSection.error));
        verifyChapter(library, QStringLiteral("a"));
        const QByteArray revisedChapter = readFile(path(library, QStringLiteral("chapters/a.html")));
        const int breakAt = revisedChapter.indexOf("data-sec-brk");
        const int ghostAt = revisedChapter.indexOf("data-sec-id");
        QVERIFY(breakAt >= 0 && ghostAt > breakAt);
        QVERIFY(revisedChapter.contains("A revised scene"));

        const auto stickyCopy = planning.transferSticky(sticky.id, QStringLiteral("b"), true);
        QVERIFY2(stickyCopy.ok, qPrintable(stickyCopy.error));
        QVERIFY(stickyCopy.id != sticky.id);
        verifyChapter(library, QStringLiteral("a"));
        verifyChapter(library, QStringLiteral("b"));
        const auto stickyMove = planning.transferSticky(sticky.id, QStringLiteral("b"), false);
        QVERIFY2(stickyMove.ok, qPrintable(stickyMove.error));
        verifyChapter(library, QStringLiteral("a"));
        verifyChapter(library, QStringLiteral("b"));

        const auto sectionCopy = planning.transferSection(
            QStringLiteral("a"), section.id, QStringLiteral("b"), true);
        QVERIFY2(sectionCopy.ok, qPrintable(sectionCopy.error));
        QVERIFY(sectionCopy.id != section.id);
        verifyChapter(library, QStringLiteral("a"));
        verifyChapter(library, QStringLiteral("b"));
        const auto sectionMove = planning.transferSection(
            QStringLiteral("a"), section.id, QStringLiteral("b"), false);
        QVERIFY2(sectionMove.ok, qPrintable(sectionMove.error));
        verifyChapter(library, QStringLiteral("a"));
        verifyChapter(library, QStringLiteral("b"));

        QVERIFY(planning.undo().ok);
        verifyChapter(library, QStringLiteral("a"));
        verifyChapter(library, QStringLiteral("b"));
        QVERIFY(readFile(path(library, QStringLiteral("chapters/a.html")))
                    .contains(section.id.toUtf8()));
        PlanningRecords reopened(library.path(), QStringLiteral("book-1"));
        const auto revised = reopened.updateSticky(stickyCopy.id, QStringLiteral("Revised"));
        QVERIFY2(revised.ok, qPrintable(revised.error));
        const auto records = QJsonDocument::fromJson(
            readFile(path(library, QStringLiteral("stickies.json")))).array();
        QCOMPARE(records.size(), 2);
        QCOMPARE(records.at(1).toObject().value(QStringLiteral("text")).toString(),
                 QStringLiteral("Revised"));
    }

    void missingMarkerIsRefusedWithoutCreatingAnOrphan()
    {
        QTemporaryDir data, state, library;
        QVERIFY(data.isValid() && state.isValid() && library.isValid());
        qputenv("XDG_DATA_HOME", data.path().toLocal8Bit());
        qputenv("XDG_STATE_HOME", state.path().toLocal8Bit());
        makeLibrary(library);
        const QByteArray originalA = readFile(path(library, QStringLiteral("chapters/a.html")));
        const QByteArray originalB = readFile(path(library, QStringLiteral("chapters/b.html")));
        const QByteArray originalStickies = readFile(path(library, QStringLiteral("stickies.json")));
        PlanningRecords planning(library.path(), QStringLiteral("book-1"));
        const PlanningResult moved = planning.transferSticky(
            QStringLiteral("s-missing"), QStringLiteral("b"), false);
        QVERIFY(!moved.ok);
        QCOMPARE(readFile(path(library, QStringLiteral("chapters/a.html"))), originalA);
        QCOMPARE(readFile(path(library, QStringLiteral("chapters/b.html"))), originalB);
        QCOMPARE(readFile(path(library, QStringLiteral("stickies.json"))), originalStickies);
    }

    void emptyChapterAcceptsFirstGhostAndRemainsEditable()
    {
        QTemporaryDir data, state, library;
        QVERIFY(data.isValid() && state.isValid() && library.isValid());
        qputenv("XDG_DATA_HOME", data.path().toLocal8Bit());
        qputenv("XDG_STATE_HOME", state.path().toLocal8Bit());
        makeLibrary(library);
        writeFile(path(library, QStringLiteral("chapters/a.html")), QByteArray());
        PlanningRecords planning(library.path(), QStringLiteral("book-1"));
        const auto section = planning.addSection(QStringLiteral("a"), QStringLiteral("First scene"));
        QVERIFY2(section.ok, qPrintable(section.error));
        const auto document = LegacyChapterCodec::decode(
            readFile(path(library, QStringLiteral("chapters/a.html"))),
            loadChapterLinks(library.path(), QStringLiteral("book-1"), QStringLiteral("a")));
        QVERIFY(document.refusalReason.isEmpty());
        QVERIFY(document.editable());
        QVERIFY(document.text.contains(QStringLiteral("First scene")));
        const QByteArray beforeSplit = readFile(path(library, QStringLiteral("chapters/a.html")));
        ChapterStructure structure(library.path(), QStringLiteral("book-1"));
        QString error;
        QVERIFY2(structure.load(&error), qPrintable(error));
        const auto split = structure.splitChapter(
            QStringLiteral("a"), 0,
            {beforeSplit, LibraryPersistence::hash(beforeSplit), beforeSplit},
            QStringLiteral("Planned chapter"));
        QVERIFY2(split.ok, qPrintable(split.error));
    }
};

QTEST_GUILESS_MAIN(PlanningRecordsTest)
#include "planning_records_test.moc"
