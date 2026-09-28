#include "library_window.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QLabel>
#include <QMap>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QtTest>

namespace {

void writeFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        qFatal("Could not create test Library fixture file");
    }
    if (file.write(contents) != contents.size()) {
        qFatal("Could not write test Library fixture file");
    }
}

void writeBook(const QString &root, const QString &id, const QByteArray &metadata)
{
    const QString directory = QDir(root).filePath(id);
    if (!QDir().mkpath(QDir(directory).filePath("chapters"))) {
        qFatal("Could not create test book directory");
    }
    writeFile(QDir(directory).filePath("book.json"), metadata);
    writeFile(QDir(directory).filePath("notes.html"), QByteArray("<p>unknown supporting data</p>\n"));
    writeFile(QDir(directory).filePath("chapters/chapter.html"), QByteArray("<p>Do not rewrite this file.</p>\n"));
}

QMap<QString, QByteArray> libraryFileHashes(const QString &root)
{
    QMap<QString, QByteArray> hashes;
    QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::System | QDir::NoSymLinks,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            qFatal("Could not inspect test Library fixture file");
        }
        hashes.insert(QDir(root).relativeFilePath(path),
                       QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256));
    }
    return hashes;
}

QTemporaryDir makeLibrary()
{
    QTemporaryDir temporary;
    if (!temporary.isValid()) {
        qFatal("Could not create temporary Library fixture");
    }

    writeFile(QDir(temporary.path()).filePath("library.json"), R"json({
  "authors": [
    { "id": "a1", "name": "Ada" },
    { "id": "a2", "name": "Babbage" }
  ],
  "shelves": [
    { "id": "s1", "name": "Drafts", "authorId": "a1", "bookIds": ["book-2", "book-1"] },
    { "id": "s2", "name": "Archive", "authorId": "a2", "bookIds": ["book-3"] }
  ],
  "futureLibraryField": { "keep": [1, true, "future"] }
})json");

    writeBook(temporary.path(), "book-1", R"json({
  "id": "book-1",
  "title": "First Title",
  "author": "Ada",
  "chapterOrder": ["chapter-b", "chapter-a"],
  "chapterTitles": { "chapter-a": "Arrival", "chapter-b": "Departure" },
  "futureBookField": { "keep": true }
})json");
    writeBook(temporary.path(), "book-2", R"json({
  "id": "book-2",
  "title": "Second Title",
  "author": "Ada",
  "chapterOrder": []
})json");
    writeBook(temporary.path(), "book-3", R"json({
  "id": "book-3",
  "title": "Third Title",
  "author": "Babbage",
  "chapterOrder": ["chapter-only"],
  "chapterTitles": { "chapter-only": "A chapter" }
})json");
    writeBook(temporary.path(), "book-unfiled", R"json({
  "id": "book-unfiled",
  "title": "Unfiled Title",
  "author": "Ada",
  "chapterOrder": []
})json");
    writeFile(QDir(temporary.path()).filePath("unknown.bin"), QByteArray("\0future\xff", 8));
    writeFile(QDir(temporary.path()).filePath("supporting-data.txt"), QByteArray("preserve me\n"));
    return temporary;
}

}

class LibraryBrowserTest final : public QObject {
    Q_OBJECT

private slots:
    void opensMultiAuthorLibraryInStoredOrderWithoutChangingFiles()
    {
        QTemporaryDir library = makeLibrary();
        const auto originalHashes = libraryFileHashes(library.path());

        LibraryWindow window;
        QVERIFY(window.openLibrary(library.path()));
        window.show();
        QApplication::processEvents();

        auto *tree = window.findChild<QTreeWidget *>("library-tree");
        QVERIFY(tree);
        QCOMPARE(tree->topLevelItemCount(), 3);
        QCOMPARE(tree->topLevelItem(0)->text(0), QString("Ada"));
        QCOMPARE(tree->topLevelItem(1)->text(0), QString("Babbage"));
        QCOMPARE(tree->topLevelItem(2)->text(0), QString("Unfiled books"));

        auto *drafts = tree->topLevelItem(0)->child(0);
        QCOMPARE(drafts->text(0), QString("Drafts"));
        QCOMPARE(drafts->childCount(), 2);
        QCOMPARE(drafts->child(0)->text(0), QString("Second Title"));
        QCOMPARE(drafts->child(1)->text(0), QString("First Title"));
        QCOMPARE(drafts->child(1)->childCount(), 2);
        QCOMPARE(drafts->child(1)->child(0)->text(0), QString("Chapter 1 — Departure"));
        QCOMPARE(drafts->child(1)->child(1)->text(0), QString("Chapter 2 — Arrival"));

        auto *archive = tree->topLevelItem(1)->child(0);
        QCOMPARE(archive->text(0), QString("Archive"));
        QCOMPARE(archive->childCount(), 1);
        QCOMPARE(archive->child(0)->text(0), QString("Third Title"));
        QCOMPARE(tree->topLevelItem(2)->child(0)->text(0), QString("Unfiled Title"));

        window.close();
        QApplication::processEvents();
        QCOMPARE(libraryFileHashes(library.path()), originalHashes);
    }

    void refusesMissingOrCorruptLibraryWithoutShowingEmptyLibrary()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());

        LibraryWindow window;
        QVERIFY(!window.openLibrary(temporary.path()));
        window.show();
        QApplication::processEvents();
        auto *refusal = window.findChild<QLabel *>("library-refusal");
        QVERIFY(refusal);
        QVERIFY(refusal->isVisible());
        QVERIFY(refusal->text().contains("library.json"));
        auto *tree = window.findChild<QTreeWidget *>("library-tree");
        QVERIFY(tree);
        QVERIFY(tree->isHidden());

        writeFile(QDir(temporary.path()).filePath("library.json"), QByteArray("{broken"));
        QVERIFY(!window.openLibrary(temporary.path()));
        QVERIFY(refusal->isVisible());
        QVERIFY(refusal->text().contains("JSON"));
        QVERIFY(tree->isHidden());
    }

    void refusesUnfiledBookWithoutMetadata()
    {
        QTemporaryDir library;
        QVERIFY(library.isValid());
        writeFile(QDir(library.path()).filePath("library.json"),
                  QByteArray(R"json({"authors":[{"id":"a1","name":"Ada"}],"shelves":[]})json"));
        QVERIFY(QDir().mkpath(QDir(library.path()).filePath("book-orphan")));

        LibraryWindow window;
        QVERIFY(!window.openLibrary(library.path()));
        window.show();
        QApplication::processEvents();
        auto *refusal = window.findChild<QLabel *>("library-refusal");
        QVERIFY(refusal);
        QVERIFY(refusal->isVisible());
        QVERIFY(refusal->text().contains("book.json"));
    }
};

QTEST_MAIN(LibraryBrowserTest)
#include "library_browser_test.moc"
