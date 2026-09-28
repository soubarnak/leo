#include "library_window.h"

#include <QApplication>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
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

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
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

QTemporaryDir makeSingleChapterLibrary(const QByteArray &chapter,
                                      const QByteArray &stickies = QByteArrayLiteral("[]"),
                                      const QByteArray &darlings = QByteArrayLiteral("[]"),
                                      const QByteArray &bookMetadata = {})
{
    QTemporaryDir temporary;
    if (!temporary.isValid()) {
        qFatal("Could not create temporary chapter Library fixture");
    }

    writeFile(QDir(temporary.path()).filePath("library.json"), QByteArray(R"json({
  "authors": [{ "id": "a1", "name": "Ada" }],
  "shelves": [{ "id": "s1", "name": "Drafts", "authorId": "a1", "bookIds": ["book-1"] }],
  "futureLibraryField": { "keep": true }
})json"));
    const QString bookPath = QDir(temporary.path()).filePath("book-1");
    if (!QDir().mkpath(QDir(bookPath).filePath("chapters"))) {
        qFatal("Could not create chapter fixture folder");
    }
    const QByteArray metadata = bookMetadata.isEmpty() ? QByteArray(R"json({
  "id": "book-1",
  "title": "First Title",
  "author": "Ada",
  "chapterOrder": ["chapter-a"],
  "chapterTitles": { "chapter-a": "Arrival" },
  "futureBookField": { "keep": [1, "future"] }
})json") : bookMetadata;
    writeFile(QDir(bookPath).filePath("book.json"), metadata);
    writeFile(QDir(bookPath).filePath("chapters/chapter-a.html"), chapter);
    writeFile(QDir(bookPath).filePath("stickies.json"), stickies);
    writeFile(QDir(bookPath).filePath("darlings.json"), darlings);
    writeFile(QDir(bookPath).filePath("unknown-supporting-data.bin"),
              QByteArray("\0future\xff", 8));
    return temporary;
}

void writePreparedJournal(const QString &stateHome,
                          const QString &libraryPath,
                          const QString &relativePath,
                          const QByteArray &oldBytes,
                          const QByteArray &newBytes)
{
    const QString directory = QDir(stateHome).filePath("leo-writer/save-journal");
    if (!QDir().mkpath(directory)) {
        qFatal("Could not create test save journal directory");
    }
    const QString id = QStringLiteral("11111111-1111-4111-8111-111111111111");
    const QJsonObject journal{
        {QStringLiteral("id"), id},
        {QStringLiteral("library_path"), QDir(libraryPath).canonicalPath()},
        {QStringLiteral("relative_path"), relativePath},
        {QStringLiteral("old_bytes"), QString::fromLatin1(oldBytes.toBase64())},
        {QStringLiteral("new_bytes"), QString::fromLatin1(newBytes.toBase64())},
        {QStringLiteral("old_sha256"), QString::fromLatin1(
             QCryptographicHash::hash(oldBytes, QCryptographicHash::Sha256).toHex())},
        {QStringLiteral("new_sha256"), QString::fromLatin1(
             QCryptographicHash::hash(newBytes, QCryptographicHash::Sha256).toHex())},
        {QStringLiteral("state"), QStringLiteral("prepared")}};
    writeFile(QDir(directory).filePath(id + QStringLiteral(".json")),
              QJsonDocument(journal).toJson(QJsonDocument::Compact));
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

QTreeWidgetItem *singleChapterItem(QTreeWidget *tree)
{
    return tree->topLevelItem(0)->child(0)->child(0)->child(0);
}

void openSingleChapter(LibraryWindow *window)
{
    window->show();
    QApplication::processEvents();
    auto *tree = window->findChild<QTreeWidget *>("library-tree");
    if (!tree) {
        qFatal("Library tree is missing in chapter fixture");
    }
    tree->expandAll();
    QTreeWidgetItem *chapter = singleChapterItem(tree);
    tree->setCurrentItem(chapter);
    emit tree->itemActivated(chapter, 0);
    QApplication::processEvents();
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
        QVERIFY(refusal->text().contains(QStringLiteral("Device handoff")));
        QFile corruptMetadata(QDir(temporary.path()).filePath("library.json"));
        QVERIFY(corruptMetadata.open(QIODevice::ReadOnly));
        QCOMPARE(corruptMetadata.readAll(), QByteArray("{broken"));
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

    void unreadableChapterStaysReadOnlyAndExplainsDeviceHandoff()
    {
        QTemporaryDir library = makeSingleChapterLibrary(QByteArrayLiteral("<p>Saved text.</p>"));
        QVERIFY(library.isValid());
        const QString chapterPath = QDir(library.path()).filePath(
            QStringLiteral("book-1/chapters/chapter-a.html"));
        QVERIFY(QFile::remove(chapterPath));

        LibraryWindow window;
        QVERIFY(window.openLibrary(library.path()));
        openSingleChapter(&window);

        auto *editor = window.findChild<QPlainTextEdit *>("chapter-editor");
        auto *state = window.findChild<QLabel *>("chapter-save-state");
        QVERIFY(editor);
        QVERIFY(state);
        QVERIFY(editor->isReadOnly());
        QVERIFY(state->text().contains(QStringLiteral("Device handoff")));
        QVERIFY(!QFileInfo::exists(chapterPath));
    }

    void editsSafeProseAroundProtectedContentWithoutChangingIt()
    {
        QTemporaryDir privateData;
        QVERIFY(privateData.isValid());
        ScopedEnvironmentVariable dataHome("XDG_DATA_HOME", privateData.path().toLocal8Bit());

        const QByteArray protectedHtml(
            "<div data-future=\"keep&amp;exact\"><span data-version=\"9\">future</span></div>");
        const QByteArray protectedMarker(
            "<p>Question <span class=\"ph-mark\" data-sid=\"s-existing\" "
            "contenteditable=\"false\">⚑</span></p>");
        const QByteArray originalChapter = QByteArray("<p>Before prose.</p>\n") + protectedHtml +
                                           QByteArray("\n") + protectedMarker +
                                           QByteArray("\r\n<p>After prose.</p>");
        const QByteArray originalStickies(
            "[ { \"id\": \"s-existing\", \"chapterId\": \"chapter-a\", "
            "\"text\": \"keep this link\" } ]");
        QTemporaryDir library = makeSingleChapterLibrary(originalChapter, originalStickies);
        QVERIFY(library.isValid());
        auto originalFileHashes = libraryFileHashes(library.path());
        const QString bookPath = QDir(library.path()).filePath("book-1");
        QFile originalBookFile(QDir(bookPath).filePath("book.json"));
        QVERIFY(originalBookFile.open(QIODevice::ReadOnly));
        const QByteArray originalBook = originalBookFile.readAll();

        LibraryWindow window;
        QVERIFY(window.openLibrary(library.path()));
        openSingleChapter(&window);

        auto *editor = window.findChild<QPlainTextEdit *>("chapter-editor");
        auto *save = window.findChild<QPushButton *>("chapter-save");
        QVERIFY(editor);
        QVERIFY(save);
        QVERIFY(!editor->isReadOnly());
        const QString originalView = editor->toPlainText();
        QVERIFY(originalView.contains("Protected legacy content"));

        QString edited = originalView;
        edited.replace(QStringLiteral("Before prose."),
                        QStringLiteral("Revised prose.\nAdded safe prose."));
        edited.replace(QStringLiteral("After prose."), QStringLiteral("Finished prose."));
        editor->setPlainText(edited);
        QApplication::processEvents();
        QVERIFY(save->isEnabled());
        save->click();
        QApplication::processEvents();

        QFile savedChapter(QDir(bookPath).filePath("chapters/chapter-a.html"));
        QVERIFY(savedChapter.open(QIODevice::ReadOnly));
        const QByteArray savedBytes = savedChapter.readAll();
        QVERIFY(savedBytes.contains("<p>Revised prose.</p>"));
        QVERIFY(savedBytes.contains("<p>Added safe prose.</p>"));
        QVERIFY(savedBytes.contains("<p>Finished prose.</p>"));
        QVERIFY(savedBytes.contains(protectedHtml));
        QVERIFY(savedBytes.contains(protectedMarker));
        QVERIFY(savedBytes.contains("\r\n"));
        QVERIFY(savedBytes.indexOf("<p>Revised prose.</p>") < savedBytes.indexOf(protectedHtml));
        QVERIFY(savedBytes.indexOf("<p>Added safe prose.</p>") < savedBytes.indexOf(protectedHtml));
        QVERIFY(savedBytes.indexOf(protectedHtml) < savedBytes.indexOf(protectedMarker));
        QVERIFY(savedBytes.indexOf(protectedMarker) < savedBytes.indexOf("<p>Finished prose.</p>"));

        QFile savedBook(QDir(bookPath).filePath("book.json"));
        QVERIFY(savedBook.open(QIODevice::ReadOnly));
        QCOMPARE(savedBook.readAll(), originalBook);
        QFile savedStickies(QDir(bookPath).filePath("stickies.json"));
        QVERIFY(savedStickies.open(QIODevice::ReadOnly));
        QCOMPARE(savedStickies.readAll(), originalStickies);

        auto savedFileHashes = libraryFileHashes(library.path());
        originalFileHashes.remove(QStringLiteral("book-1/chapters/chapter-a.html"));
        savedFileHashes.remove(QStringLiteral("book-1/chapters/chapter-a.html"));
        QCOMPARE(savedFileHashes, originalFileHashes);
    }

    void refusesAnEditThatChangesProtectedContent()
    {
        QTemporaryDir privateData;
        QVERIFY(privateData.isValid());
        ScopedEnvironmentVariable dataHome("XDG_DATA_HOME", privateData.path().toLocal8Bit());

        const QByteArray originalChapter(
            "<p>Before.</p><p>Question <span class=\"ph-mark\" data-sid=\"s-existing\" "
            "contenteditable=\"false\">⚑</span></p><p>After.</p>");
        const QByteArray originalStickies(
            "[{\"id\":\"s-existing\",\"chapterId\":\"chapter-a\"}]");
        QTemporaryDir library = makeSingleChapterLibrary(originalChapter, originalStickies);
        QVERIFY(library.isValid());

        LibraryWindow window;
        QVERIFY(window.openLibrary(library.path()));
        openSingleChapter(&window);
        auto *editor = window.findChild<QPlainTextEdit *>("chapter-editor");
        auto *state = window.findChild<QLabel *>("chapter-save-state");
        auto *save = window.findChild<QPushButton *>("chapter-save");
        QVERIFY(editor);
        QVERIFY(state);
        QVERIFY(save);
        const QString originalView = editor->toPlainText();
        const qsizetype protectedStart = originalView.indexOf(QStringLiteral("Protected legacy content"));
        QVERIFY(protectedStart >= 0);

        QTextCursor selection(editor->document());
        selection.setPosition(static_cast<int>(protectedStart - 1));
        selection.setPosition(static_cast<int>(originalView.indexOf(QLatin1Char('\n'), protectedStart) + 1),
                              QTextCursor::KeepAnchor);
        editor->setTextCursor(selection);
        QTest::keyClick(editor, Qt::Key_Backspace);
        QCOMPARE(editor->toPlainText(), originalView);
        QVERIFY(state->text().contains("refused", Qt::CaseInsensitive));
        QVERIFY(!save->isEnabled());

        QString invalidEdit = originalView;
        invalidEdit.remove(protectedStart, invalidEdit.indexOf(QLatin1Char('\n'), protectedStart) - protectedStart);

        editor->setPlainText(invalidEdit);
        QApplication::processEvents();

        QCOMPARE(editor->toPlainText(), originalView);
        QVERIFY(state->text().contains("refused", Qt::CaseInsensitive));
        QVERIFY(!save->isEnabled());
        QFile unchanged(QDir(library.path()).filePath("book-1/chapters/chapter-a.html"));
        QVERIFY(unchanged.open(QIODevice::ReadOnly));
        QCOMPARE(unchanged.readAll(), originalChapter);
        QFile unchangedStickies(QDir(library.path()).filePath("book-1/stickies.json"));
        QVERIFY(unchangedStickies.open(QIODevice::ReadOnly));
        QCOMPARE(unchangedStickies.readAll(), originalStickies);
    }

    void tracksClipboardRegionWhenMovingSafeProseAcrossProtectedContent()
    {
        QTemporaryDir privateData;
        QVERIFY(privateData.isValid());
        ScopedEnvironmentVariable dataHome("XDG_DATA_HOME", privateData.path().toLocal8Bit());

        const QByteArray originalChapter(
            "<p>Before prose.</p><div data-future=\"keep\"><span>legacy block</span></div>"
            "<p>After prose.</p>");
        QTemporaryDir library = makeSingleChapterLibrary(originalChapter);
        QVERIFY(library.isValid());

        LibraryWindow window;
        QVERIFY(window.openLibrary(library.path()));
        openSingleChapter(&window);
        auto *editor = window.findChild<QPlainTextEdit *>("chapter-editor");
        auto *state = window.findChild<QLabel *>("chapter-save-state");
        auto *save = window.findChild<QPushButton *>("chapter-save");
        QVERIFY(editor);
        QVERIFY(state);
        QVERIFY(save);
        QVERIFY(!editor->isReadOnly());
        const QString originalView = editor->toPlainText();
        const int sourceStart = originalView.indexOf(QStringLiteral("Before prose."));
        const int sourceEnd = sourceStart + QStringLiteral("Before prose.").size();
        QVERIFY(sourceStart >= 0);

        QTextCursor source(editor->document());
        source.setPosition(sourceStart);
        source.setPosition(sourceEnd, QTextCursor::KeepAnchor);
        editor->setTextCursor(source);
        QTest::keyClick(editor, Qt::Key_C, Qt::ControlModifier);

        QTextCursor safeDestination(editor->document());
        safeDestination.setPosition(sourceEnd);
        editor->setTextCursor(safeDestination);
        QTest::keyClick(editor, Qt::Key_V, Qt::ControlModifier);
        QVERIFY(editor->toPlainText().contains(QStringLiteral("Before prose.Before prose.")));
        save->click();
        QApplication::processEvents();
        QTextCursor cutSelection(editor->document());
        cutSelection.setPosition(sourceStart);
        cutSelection.setPosition(sourceEnd, QTextCursor::KeepAnchor);
        editor->setTextCursor(cutSelection);
        QTest::keyClick(editor, Qt::Key_X, Qt::ControlModifier);
        QVERIFY(!editor->toPlainText().contains(QStringLiteral("Before prose.Before prose.")));
        QVERIFY(save->isEnabled());
        save->click();
        QApplication::processEvents();
        const QString beforeCrossingView = editor->toPlainText();
        const auto beforeCrossingHashes = libraryFileHashes(library.path());

        QTextCursor destination(editor->document());
        destination.setPosition(beforeCrossingView.indexOf(QStringLiteral("After prose.")) +
                                QStringLiteral("After prose.").size());
        editor->setTextCursor(destination);
        QTest::keyClick(editor, Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(editor->toPlainText(), beforeCrossingView);
        QVERIFY(state->text().contains("refused", Qt::CaseInsensitive));
        QVERIFY(!save->isEnabled());

        QCOMPARE(editor->toPlainText(), beforeCrossingView);

        QTest::qWait(900);
        QCOMPARE(libraryFileHashes(library.path()), beforeCrossingHashes);
    }

    void refusesPrimarySelectionPasteAcrossProtectedContent()
    {
        if (!QApplication::clipboard()->supportsSelection()) {
            QSKIP("This platform has no primary-selection clipboard.");
        }

        QTemporaryDir privateData;
        QVERIFY(privateData.isValid());
        ScopedEnvironmentVariable dataHome("XDG_DATA_HOME", privateData.path().toLocal8Bit());

        const QByteArray originalChapter(
            "<p>Before prose.</p><div data-future=\"keep\"><span>legacy block</span></div>"
            "<p>After prose.</p>");
        QTemporaryDir library = makeSingleChapterLibrary(originalChapter);
        QVERIFY(library.isValid());

        LibraryWindow window;
        QVERIFY(window.openLibrary(library.path()));
        openSingleChapter(&window);
        auto *editor = window.findChild<QPlainTextEdit *>("chapter-editor");
        auto *state = window.findChild<QLabel *>("chapter-save-state");
        QVERIFY(editor);
        QVERIFY(state);
        const QString originalView = editor->toPlainText();
        const int sourceStart = originalView.indexOf(QStringLiteral("Before prose."));
        const int sourceEnd = sourceStart + QStringLiteral("Before prose.").size();
        QTextCursor source(editor->document());
        source.setPosition(sourceStart);
        source.setPosition(sourceEnd, QTextCursor::KeepAnchor);
        editor->setTextCursor(source);
        QApplication::clipboard()->setText(QStringLiteral("Before prose."), QClipboard::Selection);

        QTextCursor destination(editor->document());
        destination.setPosition(originalView.indexOf(QStringLiteral("After prose.")) +
                                QStringLiteral("After prose.").size());
        editor->setTextCursor(destination);
        QTest::mouseClick(editor, Qt::MiddleButton, Qt::NoModifier,
                          editor->cursorRect(destination).center());

        QCOMPARE(editor->toPlainText(), originalView);
        QVERIFY(state->text().contains("refused", Qt::CaseInsensitive));
    }

    void keepsValidSectionAndDarlingLinksProtectedDuringSafeEdit()
    {
        QTemporaryDir privateData;
        QVERIFY(privateData.isValid());
        ScopedEnvironmentVariable dataHome("XDG_DATA_HOME", privateData.path().toLocal8Bit());

        const QByteArray sceneBreak(
            "<p class=\"scene-break\" data-sec-brk=\"sec-1\">***</p>");
        const QByteArray ghost(
            "<p class=\"ghost\" data-sec-id=\"sec-1\">Outline text</p>");
        const QByteArray darlingAnchor(
            "<p><span class=\"darling-anchor\" data-did=\"d-existing\"></span></p>");
        const QByteArray originalChapter = QByteArray("<p>Before safe prose.</p>") + sceneBreak +
                                           ghost + darlingAnchor +
                                           QByteArray("<p>After safe prose.</p>");
        const QByteArray originalDarlings(
            "[{\"id\":\"d-existing\",\"chapterId\":\"chapter-a\",\"text\":\"saved prose\"}]");
        const QByteArray bookMetadata(R"json({
  "id": "book-1",
  "title": "First Title",
  "author": "Ada",
  "chapterOrder": ["chapter-a"],
  "chapterTitles": { "chapter-a": "Arrival" },
  "sectionNotes": {
    "chapter-a": [{ "id": "sec-1", "text": "Outline text" }]
  },
  "futureBookField": { "keep": [1, "future"] }
})json");
        QTemporaryDir library = makeSingleChapterLibrary(
            originalChapter, QByteArrayLiteral("[]"), originalDarlings, bookMetadata);
        QVERIFY(library.isValid());
        const auto originalFileHashes = libraryFileHashes(library.path());

        LibraryWindow window;
        QVERIFY(window.openLibrary(library.path()));
        openSingleChapter(&window);
        auto *editor = window.findChild<QPlainTextEdit *>("chapter-editor");
        auto *save = window.findChild<QPushButton *>("chapter-save");
        QVERIFY(editor);
        QVERIFY(save);
        QVERIFY(!editor->isReadOnly());

        QString edited = editor->toPlainText();
        edited.replace(QStringLiteral("After safe prose."), QStringLiteral("After revision."));
        editor->setPlainText(edited);
        QApplication::processEvents();
        save->click();
        QApplication::processEvents();

        QFile savedChapter(QDir(library.path()).filePath("book-1/chapters/chapter-a.html"));
        QVERIFY(savedChapter.open(QIODevice::ReadOnly));
        const QByteArray savedBytes = savedChapter.readAll();
        QVERIFY(savedBytes.contains(sceneBreak));
        QVERIFY(savedBytes.contains(ghost));
        QVERIFY(savedBytes.contains(darlingAnchor));
        QVERIFY(savedBytes.contains("<p>After revision.</p>"));
        QFile savedDarlings(QDir(library.path()).filePath("book-1/darlings.json"));
        QVERIFY(savedDarlings.open(QIODevice::ReadOnly));
        QCOMPARE(savedDarlings.readAll(), originalDarlings);

        auto expectedHashes = originalFileHashes;
        auto savedHashes = libraryFileHashes(library.path());
        expectedHashes.remove(QStringLiteral("book-1/chapters/chapter-a.html"));
        savedHashes.remove(QStringLiteral("book-1/chapters/chapter-a.html"));
        QCOMPARE(savedHashes, expectedHashes);
    }

    void opensBrokenStickyLinkReadOnlyAndOffersRepairCopy()
    {
        QTemporaryDir privateData;
        QVERIFY(privateData.isValid());
        ScopedEnvironmentVariable dataHome("XDG_DATA_HOME", privateData.path().toLocal8Bit());

        const QByteArray originalChapter(
            "<p>Before.</p><p>Question <span class=\"ph-mark\" data-sid=\"s-missing\" "
            "contenteditable=\"false\">⚑</span></p><p>After.</p>");
        const QByteArray originalStickies("[]");
        QTemporaryDir library = makeSingleChapterLibrary(originalChapter, originalStickies);
        QVERIFY(library.isValid());

        LibraryWindow window;
        QVERIFY(window.openLibrary(library.path()));
        openSingleChapter(&window);

        auto *editor = window.findChild<QPlainTextEdit *>("chapter-editor");
        auto *state = window.findChild<QLabel *>("chapter-save-state");
        auto *repairCopy = window.findChild<QPushButton *>("chapter-repair-copy");
        QVERIFY(editor);
        QVERIFY(state);
        QVERIFY(repairCopy);
        QVERIFY(editor->isReadOnly());
        QVERIFY(state->text().contains("s-missing"));
        QVERIFY(state->text().contains("stickies.json"));
        QVERIFY(repairCopy->isVisible());

        const QString copyPath = QDir(privateData.path()).filePath("chapter-repair.html");
        QTimer::singleShot(0, [&copyPath] {
            auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
            if (dialog) {
                dialog->selectFile(copyPath);
                QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
            }
        });
        repairCopy->click();

        QFile copy(copyPath);
        QVERIFY(copy.open(QIODevice::ReadOnly));
        QCOMPARE(copy.readAll(), originalChapter);
        QFile unchanged(QDir(library.path()).filePath("book-1/chapters/chapter-a.html"));
        QVERIFY(unchanged.open(QIODevice::ReadOnly));
        QCOMPARE(unchanged.readAll(), originalChapter);
        QFile unchangedStickies(QDir(library.path()).filePath("book-1/stickies.json"));
        QVERIFY(unchangedStickies.open(QIODevice::ReadOnly));
        QCOMPARE(unchangedStickies.readAll(), originalStickies);
    }

    void opensBrokenSectionGhostAndDarlingLinksReadOnly()
    {
        QTemporaryDir privateData;
        QVERIFY(privateData.isValid());
        ScopedEnvironmentVariable dataHome("XDG_DATA_HOME", privateData.path().toLocal8Bit());

        struct BrokenLinkCase {
            QByteArray chapter;
            QString id;
            QString recordFile;
        };
        const BrokenLinkCase cases[] = {
            {QByteArrayLiteral("<p class=\"ghost\" data-sec-id=\"sec-missing\">Outline</p>"),
             QStringLiteral("sec-missing"), QStringLiteral("book.json sectionNotes")},
            {QByteArrayLiteral("<p class=\"scene-break\" data-sec-brk=\"sec-missing\">***</p>"),
             QStringLiteral("sec-missing"), QStringLiteral("book.json sectionNotes")},
            {QByteArrayLiteral("<p>Text<span class=\"darling-anchor\" data-did=\"d-missing\"></span></p>"),
             QStringLiteral("d-missing"), QStringLiteral("darlings.json")}};

        for (const BrokenLinkCase &testCase : cases) {
            QTemporaryDir library = makeSingleChapterLibrary(testCase.chapter);
            QVERIFY(library.isValid());
            const auto originalHashes = libraryFileHashes(library.path());

            LibraryWindow window;
            QVERIFY(window.openLibrary(library.path()));
            openSingleChapter(&window);

            auto *editor = window.findChild<QPlainTextEdit *>("chapter-editor");
            auto *state = window.findChild<QLabel *>("chapter-save-state");
            auto *repairCopy = window.findChild<QPushButton *>("chapter-repair-copy");
            QVERIFY(editor);
            QVERIFY(state);
            QVERIFY(repairCopy);
            QVERIFY(editor->isReadOnly());
            QVERIFY(state->text().contains(testCase.id));
            QVERIFY(state->text().contains(testCase.recordFile));
            QVERIFY(repairCopy->isVisible());

            window.close();
            QApplication::processEvents();
            QCOMPARE(libraryFileHashes(library.path()), originalHashes);
        }
    }

    void recoversInterruptedSaveBeforeOpeningLibrary()
    {
        QTemporaryDir privateData;
        QTemporaryDir privateState;
        QVERIFY(privateData.isValid());
        QVERIFY(privateState.isValid());
        ScopedEnvironmentVariable dataHome("XDG_DATA_HOME", privateData.path().toLocal8Bit());
        ScopedEnvironmentVariable stateHome("XDG_STATE_HOME", privateState.path().toLocal8Bit());

        const QByteArray oldBytes("<p>Saved paragraph.</p>");
        const QByteArray newBytes("<p>Recovered paragraph.</p>");
        QTemporaryDir library = makeSingleChapterLibrary(oldBytes);
        QVERIFY(library.isValid());
        const QString relativePath = QStringLiteral("book-1/chapters/chapter-a.html");
        writePreparedJournal(privateState.path(), library.path(), relativePath,
                             oldBytes, newBytes);

        LibraryWindow window;
        QVERIFY(window.openLibrary(library.path()));
        QFile chapter(QDir(library.path()).filePath(relativePath));
        QVERIFY(chapter.open(QIODevice::ReadOnly));
        QCOMPARE(chapter.readAll(), newBytes);

        auto *notice = window.findChild<QLabel *>("library-recovery-notice");
        QVERIFY(notice);
        window.show();
        QApplication::processEvents();
        QVERIFY(notice->isVisible());
        QVERIFY(notice->text().contains(QStringLiteral("interrupted save"), Qt::CaseInsensitive));

        openSingleChapter(&window);
        QVERIFY(notice->isHidden());
    }

    void failedChapterSaveStaysDirtyAndRetryable()
    {
        QTemporaryDir privateData;
        QTemporaryDir stateParent;
        QVERIFY(privateData.isValid());
        QVERIFY(stateParent.isValid());

        const QString blockedStateHome = QDir(stateParent.path()).filePath("state-file");
        writeFile(blockedStateHome, QByteArrayLiteral("not a directory"));
        ScopedEnvironmentVariable dataHome("XDG_DATA_HOME", privateData.path().toLocal8Bit());
        ScopedEnvironmentVariable stateHome("XDG_STATE_HOME", blockedStateHome.toLocal8Bit());

        const QByteArray oldBytes("<p>Saved paragraph.</p>");
        QTemporaryDir library = makeSingleChapterLibrary(oldBytes);
        QVERIFY(library.isValid());

        LibraryWindow window;
        QVERIFY(window.openLibrary(library.path()));
        openSingleChapter(&window);
        auto *editor = window.findChild<QPlainTextEdit *>("chapter-editor");
        auto *state = window.findChild<QLabel *>("chapter-save-state");
        auto *saveButton = window.findChild<QPushButton *>("chapter-save");
        QVERIFY(editor);
        QVERIFY(state);
        QVERIFY(saveButton);

        editor->setPlainText(QStringLiteral("Draft that cannot save yet."));
        QVERIFY(saveButton->isEnabled());
        saveButton->click();

        QVERIFY(saveButton->isEnabled());
        QCOMPARE(saveButton->text(), QStringLiteral("Retry Save"));
        QVERIFY(state->text().contains(QStringLiteral("Unsaved changes")));
        QVERIFY(state->text().contains(QStringLiteral("Retry with Save")));

        QFile chapter(QDir(library.path()).filePath(
            QStringLiteral("book-1/chapters/chapter-a.html")));
        QVERIFY(chapter.open(QIODevice::ReadOnly));
        QCOMPARE(chapter.readAll(), oldBytes);
    }

    void externalEditPreservesDraftAndOffersExplicitRecoveredSwitch()
    {
        QTemporaryDir privateData;
        QTemporaryDir privateState;
        QVERIFY(privateData.isValid());
        QVERIFY(privateState.isValid());
        ScopedEnvironmentVariable dataHome("XDG_DATA_HOME", privateData.path().toLocal8Bit());
        ScopedEnvironmentVariable stateHome("XDG_STATE_HOME", privateState.path().toLocal8Bit());

        const QByteArray externalBytes("<p>External edit.</p>");
        QTemporaryDir library = makeSingleChapterLibrary(QByteArrayLiteral("<p>Saved paragraph.</p>"));
        QVERIFY(library.isValid());

        LibraryWindow window;
        QVERIFY(window.openLibrary(library.path()));
        openSingleChapter(&window);
        auto *editor = window.findChild<QPlainTextEdit *>("chapter-editor");
        auto *saveButton = window.findChild<QPushButton *>("chapter-save");
        auto *state = window.findChild<QLabel *>("chapter-save-state");
        auto *switchButton = window.findChild<QPushButton *>("chapter-open-recovered");
        QVERIFY(editor);
        QVERIFY(saveButton);
        QVERIFY(state);
        QVERIFY(switchButton);

        editor->setPlainText(QStringLiteral("Local chapter draft."));
        writeFile(QDir(library.path()).filePath(
                      QStringLiteral("book-1/chapters/chapter-a.html")),
                  externalBytes);
        saveButton->click();

        QVERIFY(!saveButton->isEnabled());
        QCOMPARE(saveButton->text(), QStringLiteral("Save paused"));
        QVERIFY(state->text().contains(QStringLiteral("Device handoff")));
        QVERIFY(switchButton->isVisible());
        QVERIFY(!editor->isReadOnly());

        editor->setPlainText(QStringLiteral("Newest local chapter draft."));
        const QString draftDirectory = QDir(privateData.path()).filePath(
            QStringLiteral("leo-writer/conflict-drafts"));
        const QStringList drafts = QDir(draftDirectory).entryList(
            {QStringLiteral("*.html")}, QDir::Files);
        QCOMPARE(drafts.size(), 1);
        const QString draftPath = QDir(draftDirectory).filePath(drafts.first());
        QTRY_VERIFY_WITH_TIMEOUT(
            readFile(draftPath).contains(QByteArrayLiteral("Newest local chapter draft.")),
            3000);
        const QString recoveredDirectory = QDir(privateData.path()).filePath(
            QStringLiteral("leo-writer/Recovered Libraries"));
        const QStringList recoveredLibraries = QDir(recoveredDirectory).entryList(
            {QStringLiteral("Recovered library *")}, QDir::Dirs | QDir::NoDotAndDotDot);
        QCOMPARE(recoveredLibraries.size(), 1);
        const QString recoveredChapter = QDir(recoveredDirectory)
                                             .filePath(recoveredLibraries.first() +
                                                       QStringLiteral("/book-1/chapters/chapter-a.html"));
        QTRY_VERIFY_WITH_TIMEOUT(
            readFile(recoveredChapter).contains(QByteArrayLiteral("Newest local chapter draft.")),
            3000);

        QFile sharedChapter(QDir(library.path()).filePath(
            QStringLiteral("book-1/chapters/chapter-a.html")));
        QVERIFY(sharedChapter.open(QIODevice::ReadOnly));
        QCOMPARE(sharedChapter.readAll(), externalBytes);

        switchButton->click();
        QApplication::processEvents();
        openSingleChapter(&window);
        editor = window.findChild<QPlainTextEdit *>("chapter-editor");
        QVERIFY(editor);
        QVERIFY(editor->toPlainText().contains(QStringLiteral("Newest local chapter draft.")));

        QFile sharedChapterAfterSwitch(QDir(library.path()).filePath(
            QStringLiteral("book-1/chapters/chapter-a.html")));
        QVERIFY(sharedChapterAfterSwitch.open(QIODevice::ReadOnly));
        QCOMPARE(sharedChapterAfterSwitch.readAll(), externalBytes);
    }

    void pausesRecoveryWhenLibraryBytesMatchNeitherJournalHash()
    {
        QTemporaryDir privateData;
        QTemporaryDir privateState;
        QVERIFY(privateData.isValid());
        QVERIFY(privateState.isValid());
        ScopedEnvironmentVariable dataHome("XDG_DATA_HOME", privateData.path().toLocal8Bit());
        ScopedEnvironmentVariable stateHome("XDG_STATE_HOME", privateState.path().toLocal8Bit());

        const QByteArray oldBytes("<p>Saved paragraph.</p>");
        const QByteArray newBytes("<p>Recovered paragraph.</p>");
        const QByteArray externalBytes("<p>External revision.</p>");
        QTemporaryDir library = makeSingleChapterLibrary(externalBytes);
        QVERIFY(library.isValid());
        const QString relativePath = QStringLiteral("book-1/chapters/chapter-a.html");
        writePreparedJournal(privateState.path(), library.path(), relativePath,
                             oldBytes, newBytes);

        LibraryWindow window;
        QVERIFY(!window.openLibrary(library.path()));
        QFile chapter(QDir(library.path()).filePath(relativePath));
        QVERIFY(chapter.open(QIODevice::ReadOnly));
        QCOMPARE(chapter.readAll(), externalBytes);

        const QString journalPath = QDir(privateState.path()).filePath(
            QStringLiteral("leo-writer/save-journal/11111111-1111-4111-8111-111111111111.json"));
        QVERIFY(QFileInfo::exists(journalPath));
        auto *refusal = window.findChild<QLabel *>("library-refusal");
        QVERIFY(refusal);
        window.show();
        QApplication::processEvents();
        QVERIFY(refusal->isVisible());
        QVERIFY(refusal->text().contains(QStringLiteral("paused"), Qt::CaseInsensitive));
        QVERIFY(refusal->text().contains(QStringLiteral("external change"), Qt::CaseInsensitive));
        auto *switchButton = window.findChild<QPushButton *>("library-open-recovered");
        QVERIFY(refusal->text().contains(QStringLiteral("Device handoff")));
        QVERIFY(switchButton);
        QVERIFY(switchButton->isVisible());
        switchButton->click();
        QApplication::processEvents();
        QVERIFY(!refusal->isVisible());
        openSingleChapter(&window);
        auto *editor = window.findChild<QPlainTextEdit *>("chapter-editor");
        QVERIFY(editor);
        QVERIFY(editor->toPlainText().contains(QStringLiteral("Recovered paragraph.")));

        QFile sharedChapter(QDir(library.path()).filePath(relativePath));
        QVERIFY(sharedChapter.open(QIODevice::ReadOnly));
        QCOMPARE(sharedChapter.readAll(), externalBytes);
    }

    void recoversInterruptedSaveWhenFlushedStageRemains()
    {
        QTemporaryDir privateData;
        QTemporaryDir privateState;
        QVERIFY(privateData.isValid());
        QVERIFY(privateState.isValid());
        ScopedEnvironmentVariable dataHome("XDG_DATA_HOME", privateData.path().toLocal8Bit());
        ScopedEnvironmentVariable stateHome("XDG_STATE_HOME", privateState.path().toLocal8Bit());

        const QByteArray oldBytes("<p>Saved paragraph.</p>");
        const QByteArray newBytes("<p>Recovered paragraph.</p>");
        QTemporaryDir library = makeSingleChapterLibrary(oldBytes);
        QVERIFY(library.isValid());
        const QString relativePath = QStringLiteral("book-1/chapters/chapter-a.html");
        const QString stagePath = QDir(library.path()).filePath(
            QStringLiteral("book-1/chapters/.chapter-a.html.leo-11111111-1111-4111-8111-111111111111.tmp"));
        writePreparedJournal(privateState.path(), library.path(), relativePath,
                             oldBytes, newBytes);
        writeFile(stagePath, newBytes);

        LibraryWindow window;
        QVERIFY(window.openLibrary(library.path()));
        QFile chapter(QDir(library.path()).filePath(relativePath));
        QVERIFY(chapter.open(QIODevice::ReadOnly));
        QCOMPARE(chapter.readAll(), newBytes);
        QVERIFY(!QFileInfo::exists(stagePath));
    }
};

QTEST_MAIN(LibraryBrowserTest)
#include "library_browser_test.moc"
