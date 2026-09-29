#include "chapter_structure.h"
#include "library_reader.h"
#include "planning_records.h"
#include "chapter_links.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

namespace {

void writeFile(const QString &path, const QByteArray &bytes)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        qFatal("Could not create a chapter structure fixture folder");
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        qFatal("Could not write a chapter structure fixture file");
    }
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

QTemporaryDir makeLibrary()
{
    QTemporaryDir temporary;
    if (!temporary.isValid()) {
        qFatal("Could not create a chapter structure fixture Library");
    }
    const QString bookPath = QDir(temporary.path()).filePath(QStringLiteral("book-1"));
    writeFile(QDir(temporary.path()).filePath(QStringLiteral("library.json")), QByteArrayLiteral(
        R"json({"authors":[{"id":"a1","name":"Ada"}],"shelves":[{"id":"s1","name":"Drafts","authorId":"a1","bookIds":["book-1"]}]})json"));
    writeFile(QDir(bookPath).filePath(QStringLiteral("book.json")), QByteArrayLiteral(
        R"json({"id":"book-1","title":"A Book","author":"Ada","chapterOrder":["chapter-a","chapter-b"],"chapterTitles":{"chapter-a":"Arrival","chapter-b":"Crossing"},"lastPosition":{"chapterId":"chapter-b","scroll":4},"futureBookField":{"keep":["opaque",7]}})json"));
    writeFile(QDir(bookPath).filePath(QStringLiteral("chapters/chapter-a.html")),
              QByteArrayLiteral("<p>Before the storm.</p>"));
    writeFile(QDir(bookPath).filePath(QStringLiteral("chapters/chapter-b.html")),
              QByteArrayLiteral("<p>Second chapter.</p>"));
    writeFile(QDir(bookPath).filePath(QStringLiteral("stickies.json")), QByteArrayLiteral("[]"));
    writeFile(QDir(bookPath).filePath(QStringLiteral("darlings.json")), QByteArrayLiteral("[]"));
    return temporary;
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

void isolatePersistencePaths(QTemporaryDir *data, QTemporaryDir *state,
                            ScopedEnvironmentVariable **dataVariable,
                            ScopedEnvironmentVariable **stateVariable)
{
    if (!data->isValid() || !state->isValid()) {
        qFatal("Could not create private test paths");
    }
    *dataVariable = new ScopedEnvironmentVariable("XDG_DATA_HOME", data->path().toLocal8Bit());
    *stateVariable = new ScopedEnvironmentVariable("XDG_STATE_HOME", state->path().toLocal8Bit());
}

QJsonObject bookMetadata(const QString &libraryPath)
{
    return QJsonDocument::fromJson(readFile(QDir(libraryPath).filePath(
               QStringLiteral("book-1/book.json"))))
        .object();
}

QStringList chapterOrder(const QString &libraryPath)
{
    QStringList ids;
    for (const QJsonValue &value : bookMetadata(libraryPath)
                                       .value(QStringLiteral("chapterOrder")).toArray()) {
        ids.append(value.toString());
    }
    return ids;
}

} // namespace

class ChapterStructureTest final : public QObject {
    Q_OBJECT

private slots:
    void splitPersistsAndStructuralUndoRedoRestoresTextAndOrder()
    {
        QTemporaryDir data;
        QTemporaryDir state;
        ScopedEnvironmentVariable *dataVariable = nullptr;
        ScopedEnvironmentVariable *stateVariable = nullptr;
        isolatePersistencePaths(&data, &state, &dataVariable, &stateVariable);
        const auto cleanupEnvironment = qScopeGuard([&] {
            delete dataVariable;
            delete stateVariable;
        });
        QTemporaryDir library = makeLibrary();
        QVERIFY(library.isValid());

        ChapterStructure structure(library.path(), QStringLiteral("book-1"));
        QString error;
        QVERIFY2(structure.load(&error), qPrintable(error));
        const QString sourcePath = QDir(library.path()).filePath(
            QStringLiteral("book-1/chapters/chapter-a.html"));
        const QByteArray original = readFile(sourcePath);
        const QByteArray hash = LibraryPersistence::hash(original);
        const QByteArray edited("<p>Before the storm.</p>");
        const ChapterStructureResult split = structure.splitChapter(
            QStringLiteral("chapter-a"), QStringLiteral("Before ").size(),
            {edited, hash, original}, QStringLiteral("The Storm"));
        QVERIFY2(split.ok, qPrintable(split.error));
        QVERIFY(!split.chapterId.isEmpty());
        QCOMPARE(chapterOrder(library.path()),
                 QStringList({QStringLiteral("chapter-a"), split.chapterId,
                              QStringLiteral("chapter-b")}));
        QCOMPARE(readFile(sourcePath), QByteArrayLiteral("<p>Before </p>"));
        QCOMPARE(readFile(QDir(library.path()).filePath(
                     QStringLiteral("book-1/chapters/") + split.chapterId +
                     QStringLiteral(".html"))), QByteArrayLiteral("<p>the storm.</p>"));
        QCOMPARE(bookMetadata(library.path()).value(QStringLiteral("chapterTitles")).toObject()
                     .value(split.chapterId).toString(), QStringLiteral("The Storm"));
        QVERIFY(structure.canUndo());
        QVERIFY(!structure.canRedo());

        const LibraryReadResult reopened = LibraryReader::read(library.path());
        QVERIFY2(reopened.ok(), qPrintable(reopened.error));
        QCOMPARE(reopened.library.authors.first().shelves.first().books.first().chapters.size(), 3);

        const ChapterStructureResult undone = structure.undo();
        QVERIFY2(undone.ok, qPrintable(undone.error));
        QCOMPARE(undone.chapterId, QStringLiteral("chapter-a"));
        QCOMPARE(chapterOrder(library.path()),
                 QStringList({QStringLiteral("chapter-a"), QStringLiteral("chapter-b")}));
        QCOMPARE(readFile(sourcePath), original);
        QVERIFY(structure.canRedo());

        const ChapterStructureResult redone = structure.redo();
        QVERIFY2(redone.ok, qPrintable(redone.error));
        QCOMPARE(redone.chapterId, split.chapterId);
        QCOMPARE(chapterOrder(library.path()).size(), 3);
        QCOMPARE(readFile(sourcePath), QByteArrayLiteral("<p>Before </p>"));
    }

    void joinAndDeletePersistAndUndoPreservesChapterOwnership()
    {
        QTemporaryDir data;
        QTemporaryDir state;
        ScopedEnvironmentVariable *dataVariable = nullptr;
        ScopedEnvironmentVariable *stateVariable = nullptr;
        isolatePersistencePaths(&data, &state, &dataVariable, &stateVariable);
        const auto cleanupEnvironment = qScopeGuard([&] {
            delete dataVariable;
            delete stateVariable;
        });
        QTemporaryDir library = makeLibrary();
        QVERIFY(library.isValid());
        ChapterStructure structure(library.path(), QStringLiteral("book-1"));
        QString error;
        QVERIFY2(structure.load(&error), qPrintable(error));

        const QString firstPath = QDir(library.path()).filePath(
            QStringLiteral("book-1/chapters/chapter-a.html"));
        const QString secondPath = QDir(library.path()).filePath(
            QStringLiteral("book-1/chapters/chapter-b.html"));
        const QByteArray firstBytes = readFile(firstPath);
        const QByteArray secondBytes = readFile(secondPath);
        const ChapterStructureResult joined = structure.joinChapter(
            QStringLiteral("chapter-a"), false,
            {firstBytes, LibraryPersistence::hash(firstBytes), firstBytes});
        QVERIFY2(joined.ok, qPrintable(joined.error));
        QCOMPARE(chapterOrder(library.path()), QStringList({QStringLiteral("chapter-a")}));
        QCOMPARE(readFile(firstPath), QByteArrayLiteral("<p>Before the storm.</p><p>Second chapter.</p>"));
        QCOMPARE(readFile(secondPath), secondBytes);
        QVERIFY2(structure.undo().ok, "Join should be undoable");
        QCOMPARE(chapterOrder(library.path()),
                 QStringList({QStringLiteral("chapter-a"), QStringLiteral("chapter-b")}));
        QCOMPARE(readFile(firstPath), firstBytes);

        ChapterStructure deletion(library.path(), QStringLiteral("book-1"));
        QVERIFY2(deletion.load(&error), qPrintable(error));
        const ChapterStructureResult deleted = deletion.deleteChapter(
            QStringLiteral("chapter-b"),
            {secondBytes, LibraryPersistence::hash(secondBytes), secondBytes});
        QVERIFY2(deleted.ok, qPrintable(deleted.error));
        QCOMPARE(chapterOrder(library.path()), QStringList({QStringLiteral("chapter-a")}));
        QCOMPARE(bookMetadata(library.path()).value(QStringLiteral("lastPosition")).toObject()
                     .value(QStringLiteral("chapterId")).toString(), QStringLiteral("chapter-a"));
        QVERIFY2(deletion.undo().ok, "Delete should be undoable");
        QCOMPARE(chapterOrder(library.path()),
                 QStringList({QStringLiteral("chapter-a"), QStringLiteral("chapter-b")}));
        QCOMPARE(readFile(secondPath), secondBytes);
    }

    void protectedMarkupAndLinkedRecordsRefuseContentStructureWithoutPartialWrites()
    {
        QTemporaryDir data;
        QTemporaryDir state;
        ScopedEnvironmentVariable *dataVariable = nullptr;
        ScopedEnvironmentVariable *stateVariable = nullptr;
        isolatePersistencePaths(&data, &state, &dataVariable, &stateVariable);
        const auto cleanupEnvironment = qScopeGuard([&] {
            delete dataVariable;
            delete stateVariable;
        });
        QTemporaryDir library = makeLibrary();
        QVERIFY(library.isValid());
        const QString sourcePath = QDir(library.path()).filePath(
            QStringLiteral("book-1/chapters/chapter-a.html"));
        const QByteArray protectedBytes(
            "<p>Plain prose.</p><div data-future=\"1\">unrecognized</div>");
        writeFile(sourcePath, protectedBytes);
        const QByteArray originalBook = readFile(QDir(library.path()).filePath(
            QStringLiteral("book-1/book.json")));
        ChapterStructure structure(library.path(), QStringLiteral("book-1"));
        QString error;
        QVERIFY2(structure.load(&error), qPrintable(error));
        const ChapterStructureResult split = structure.splitChapter(
            QStringLiteral("chapter-a"), 4,
            {protectedBytes, LibraryPersistence::hash(protectedBytes), protectedBytes},
            QStringLiteral("Next"));
        QVERIFY(!split.ok);
        QVERIFY(split.error.contains(QStringLiteral("protected"), Qt::CaseInsensitive));
        QCOMPARE(readFile(sourcePath), protectedBytes);
        QCOMPARE(readFile(QDir(library.path()).filePath(QStringLiteral("book-1/book.json"))),
                 originalBook);

        const QByteArray plainBytes("<p>Plain prose.</p>");
        writeFile(sourcePath, plainBytes);
        writeFile(QDir(library.path()).filePath(QStringLiteral("book-1/stickies.json")),
                  QByteArrayLiteral("[{\"id\":\"sticky-1\",\"chapterId\":\"chapter-a\"}]"));
        ChapterStructure linked(library.path(), QStringLiteral("book-1"));
        QVERIFY2(linked.load(&error), qPrintable(error));
        const ChapterStructureResult linkedSplit = linked.splitChapter(
            QStringLiteral("chapter-a"), 5,
            {plainBytes, LibraryPersistence::hash(plainBytes), plainBytes},
            QStringLiteral("Next"));
        QVERIFY(!linkedSplit.ok);
        QVERIFY(linkedSplit.error.contains(QStringLiteral("sticky"), Qt::CaseInsensitive));
        QCOMPARE(readFile(sourcePath), plainBytes);
        QCOMPARE(chapterOrder(library.path()),
                 QStringList({QStringLiteral("chapter-a"), QStringLiteral("chapter-b")}));
    }

    void titleAndOrderChangesPreserveUnknownMetadata()
    {
        QTemporaryDir data;
        QTemporaryDir state;
        ScopedEnvironmentVariable *dataVariable = nullptr;
        ScopedEnvironmentVariable *stateVariable = nullptr;
        isolatePersistencePaths(&data, &state, &dataVariable, &stateVariable);
        const auto cleanupEnvironment = qScopeGuard([&] {
            delete dataVariable;
            delete stateVariable;
        });
        QTemporaryDir library = makeLibrary();
        QVERIFY(library.isValid());
        ChapterStructure structure(library.path(), QStringLiteral("book-1"));
        QString error;
        QVERIFY2(structure.load(&error), qPrintable(error));
        QVERIFY2(structure.renameChapter(QStringLiteral("chapter-a"),
                                         QStringLiteral("  New Arrival  ")).ok,
                 "Chapter title should save");
        QVERIFY2(structure.moveChapter(QStringLiteral("chapter-a"), 1).ok,
                 "Chapter order should save");
        const ChapterStructureResult added = structure.addChapter(1, QStringLiteral("Bridge"));
        QVERIFY2(added.ok, qPrintable(added.error));
        const QJsonObject book = bookMetadata(library.path());
        QCOMPARE(chapterOrder(library.path()),
                 QStringList({QStringLiteral("chapter-b"), added.chapterId,
                              QStringLiteral("chapter-a")}));
        QCOMPARE(book.value(QStringLiteral("chapterTitles")).toObject()
                     .value(QStringLiteral("chapter-a")).toString(), QStringLiteral("New Arrival"));
        QCOMPARE(book.value(QStringLiteral("chapterTitles")).toObject()
                     .value(added.chapterId).toString(), QStringLiteral("Bridge"));
        QVERIFY(QFileInfo::exists(QDir(library.path()).filePath(
            QStringLiteral("book-1/chapters/") + added.chapterId + QStringLiteral(".html"))));
        QCOMPARE(book.value(QStringLiteral("futureBookField")).toObject()
                     .value(QStringLiteral("keep")).toArray().first().toString(),
                 QStringLiteral("opaque"));

        const LibraryReadResult reopened = LibraryReader::read(library.path());
        QVERIFY2(reopened.ok(), qPrintable(reopened.error));
        QCOMPARE(reopened.library.authors.first().shelves.first().books.first().chapters.size(), 3);
        structure.invalidateHistoryForChapterEdit(
            QStringLiteral("book-1/chapters/chapter-a.html"));
        QVERIFY(structure.canUndo());
        QVERIFY2(structure.undo().ok, "Adding a chapter should be undoable");
        QCOMPARE(chapterOrder(library.path()),
                 QStringList({QStringLiteral("chapter-b"), QStringLiteral("chapter-a")}));
        QVERIFY2(structure.redo().ok, "Adding a chapter should be redoable");
        QCOMPARE(chapterOrder(library.path()).size(), 3);
        structure.invalidateHistoryForChapterEdit(
            QStringLiteral("book-1/chapters/") + added.chapterId + QStringLiteral(".html"));
        QVERIFY(!structure.canUndo());
    }

    void splitAndJoinTransferPlanningLinksWithUndo()
    {
        QTemporaryDir data, state;
        ScopedEnvironmentVariable *dataVariable = nullptr;
        ScopedEnvironmentVariable *stateVariable = nullptr;
        isolatePersistencePaths(&data, &state, &dataVariable, &stateVariable);
        const auto cleanupEnvironment = qScopeGuard([&] {
            delete dataVariable;
            delete stateVariable;
        });
        QTemporaryDir library = makeLibrary();
        QVERIFY(library.isValid());
        PlanningRecords planning(library.path(), QStringLiteral("book-1"));
        const auto sticky = planning.addSticky(QStringLiteral("chapter-a"), QStringLiteral("Check"));
        const auto section = planning.addSection(QStringLiteral("chapter-a"), QStringLiteral("Scene"));
        QVERIFY(sticky.ok && section.ok);
        const QString originalPath = QDir(library.path()).filePath(
            QStringLiteral("book-1/chapters/chapter-a.html"));
        const QByteArray original = readFile(originalPath);
        const auto links = loadChapterLinks(library.path(), QStringLiteral("book-1"),
                                            QStringLiteral("chapter-a"));
        const LegacyChapterDocument document = LegacyChapterCodec::decode(original, links);
        QVERIFY(document.refusalReason.isEmpty());
        ChapterStructure structure(library.path(), QStringLiteral("book-1"));
        QString error;
        QVERIFY2(structure.load(&error), qPrintable(error));
        const int splitAt = document.text.indexOf(QLatin1Char('\n')) + 1;
        const auto split = structure.splitChapter(
            QStringLiteral("chapter-a"), splitAt,
            {original, LibraryPersistence::hash(original), original}, QStringLiteral("Later"));
        QVERIFY2(split.ok, qPrintable(split.error));
        QVERIFY(!split.chapterId.isEmpty());
        const QJsonObject afterSplit = bookMetadata(library.path());
        QCOMPARE(afterSplit.value(QStringLiteral("sectionNotes")).toObject()
                     .value(split.chapterId).toArray().first().toObject()
                     .value(QStringLiteral("id")).toString(), section.id);
        const QJsonArray stickies = QJsonDocument::fromJson(readFile(
            QDir(library.path()).filePath(QStringLiteral("book-1/stickies.json")))).array();
        QCOMPARE(stickies.first().toObject().value(QStringLiteral("chapterId")).toString(),
                 split.chapterId);
        const QString newPath = QDir(library.path()).filePath(
            QStringLiteral("book-1/chapters/") + split.chapterId + QStringLiteral(".html"));
        const QByteArray moved = readFile(newPath);
        QVERIFY(moved.contains(sticky.id.toUtf8()));
        QVERIFY(moved.contains(section.id.toUtf8()));
        QVERIFY(structure.undo().ok);
        QCOMPARE(readFile(originalPath), original);
        QVERIFY(structure.redo().ok);

        const QByteArray first = readFile(originalPath);
        const auto joined = structure.joinChapter(
            QStringLiteral("chapter-a"), false,
            {first, LibraryPersistence::hash(first), first});
        QVERIFY2(joined.ok, qPrintable(joined.error));
        QCOMPARE(readFile(originalPath), original);
        QCOMPARE(QJsonDocument::fromJson(readFile(
            QDir(library.path()).filePath(QStringLiteral("book-1/stickies.json"))))
                     .array().first().toObject().value(QStringLiteral("chapterId")).toString(),
                 QStringLiteral("chapter-a"));
    }
};

QTEST_GUILESS_MAIN(ChapterStructureTest)
#include "chapter_structure_test.moc"
