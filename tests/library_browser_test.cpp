#include "library_window.h"

#include <QApplication>
#include <QAction>
#include <QAbstractButton>
#include <QClipboard>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QtTest>

#include <functional>

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

QTemporaryDir makeNeoLibrary(const QByteArray &chapter,
                             const QByteArray &stickies,
                             const QByteArray &darlings)
{
    QTemporaryDir temporary;
    if (!temporary.isValid()) {
        qFatal("Could not create temporary NEO Library fixture");
    }

    writeFile(QDir(temporary.path()).filePath("library.json"), R"json({
  "authorName": "Ada Lovelace",
  "penNames": ["Ada Lovelace", "A. L."],
  "firstRunDone": true,
  "pageTheme": "night",
  "shelves": [{ "id": "shelf-1", "name": "Works in Progress", "bookIds": ["book-1"] }],
  "futureLibraryField": { "keep": true }
})json");
    const QString bookPath = QDir(temporary.path()).filePath("book-1");
    if (!QDir().mkpath(QDir(bookPath).filePath("chapters"))) {
        qFatal("Could not create NEO book folder");
    }
    writeFile(QDir(bookPath).filePath("book.json"), R"json({
  "id": "book-1",
  "title": "First Title",
  "subtitle": "A subtitle",
  "series": "A series",
  "author": "Ada Lovelace",
  "wordGoal": 1200,
  "created": "2026-09-01T00:00:00.000Z",
  "modified": "2026-09-28T00:00:00.000Z",
  "chapterOrder": ["chapter-a"],
  "chapterTitles": { "chapter-a": "Arrival" },
  "lastPosition": { "chapterId": "chapter-a", "scroll": 12 },
  "futureBookField": { "keep": [1, "future"] }
})json");
    writeFile(QDir(bookPath).filePath("chapters/chapter-a.html"), chapter);
    writeFile(QDir(bookPath).filePath("notes.html"), QByteArrayLiteral("<p>Library notes</p>"));
    writeFile(QDir(bookPath).filePath("outline.html"), QByteArrayLiteral("<p>Library outline</p>"));
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

QAction *deviceHandoffAction(LibraryWindow *window)
{
    if (window->menuBar()->actions().isEmpty()) {
        return nullptr;
    }
    QMenu *fileMenu = window->menuBar()->actions().first()->menu();
    if (!fileMenu) {
        return nullptr;
    }
    for (QAction *action : fileMenu->actions()) {
        if (action->text() == QStringLiteral("Prepare Device Handoff…")) {
            return action;
        }
    }
    return nullptr;
}

QAbstractButton *buttonWithText(QMessageBox *dialog, const QString &text)
{
    for (QAbstractButton *button : dialog->buttons()) {
        if (button->text() == text) {
            return button;
        }
    }
    return nullptr;
}

struct HandoffDialogResult {
    bool shown = false;
    bool answered = false;
    QString title;
    QString text;
};

HandoffDialogResult prepareDeviceHandoff(
    LibraryWindow *window,
    const std::function<QAbstractButton *(QMessageBox *)> &chooseButton)
{
    HandoffDialogResult result;
    QAction *action = deviceHandoffAction(window);
    if (!action) {
        return result;
    }

    QTimer::singleShot(0, window, [&] {
        auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (!dialog) {
            return;
        }
        result.shown = true;
        result.title = dialog->windowTitle();
        result.text = dialog->text();
        QAbstractButton *button = chooseButton(dialog);
        if (!button && !dialog->buttons().isEmpty()) {
            button = dialog->buttons().first();
        }
        if (button) {
            result.answered = true;
            button->click();
        }
    });
    action->trigger();
    return result;
}

}

class LibraryBrowserTest final : public QObject {
    Q_OBJECT

private slots:
    void newWriterCreatesLibraryWithPreferencesAndOpensOutline()
    {
        QTemporaryDir privateData;
        QVERIFY(privateData.isValid());
        ScopedEnvironmentVariable dataHome("XDG_DATA_HOME", privateData.path().toLocal8Bit());
        QTemporaryDir privateState;
        QVERIFY(privateState.isValid());
        ScopedEnvironmentVariable stateHome("XDG_STATE_HOME", privateState.path().toLocal8Bit());
        QTemporaryDir locationRoot;
        QVERIFY(locationRoot.isValid());
        const QString libraryPath = QDir(locationRoot.path()).filePath("Writer Library");

        LibraryWindow window;
        window.show();
        QApplication::processEvents();

        auto *createButton = window.findChild<QPushButton *>("new-library-button");
        QVERIFY(createButton);
        createButton->click();

        auto *author = window.findChild<QLineEdit *>("onboarding-author");
        auto *location = window.findChild<QLineEdit *>("onboarding-location");
        auto *mode = window.findChild<QComboBox *>("onboarding-mode");
        auto *bodyFont = window.findChild<QComboBox *>("onboarding-body-font");
        auto *dropCap = window.findChild<QComboBox *>("onboarding-drop-cap");
        auto *submit = window.findChild<QPushButton *>("onboarding-submit");
        QVERIFY(author);
        QVERIFY(location);
        QVERIFY(mode);
        QVERIFY(bodyFont);
        QVERIFY(dropCap);
        QVERIFY(submit);

        author->setText(QStringLiteral("Ada Lovelace"));
        location->setText(libraryPath);
        mode->setCurrentIndex(mode->findData(QStringLiteral("plotter")));
        dropCap->setCurrentIndex(dropCap->findData(QStringLiteral("scifi")));
        const QString selectedBodyFont = bodyFont->currentData().toString();
        QVERIFY(!selectedBodyFont.isEmpty());
        submit->click();
        QApplication::processEvents();

        QFile libraryFile(QDir(libraryPath).filePath(QStringLiteral("library.json")));
        QVERIFY(libraryFile.open(QIODevice::ReadOnly));
        const QJsonObject library = QJsonDocument::fromJson(libraryFile.readAll()).object();
        QCOMPARE(library.value(QStringLiteral("writingStyle")).toString(), QStringLiteral("plotter"));
        const QJsonObject fonts = library.value(QStringLiteral("fonts")).toObject();
        QCOMPARE(fonts.value(QStringLiteral("body")).toString(), selectedBodyFont);
        QCOMPARE(fonts.value(QStringLiteral("dropcap")).toString(), QStringLiteral("scifi"));

        const QJsonArray authors = library.value(QStringLiteral("authors")).toArray();
        QCOMPARE(authors.size(), 1);
        QCOMPARE(authors.first().toObject().value(QStringLiteral("name")).toString(),
                 QStringLiteral("Ada Lovelace"));
        const QJsonArray shelves = library.value(QStringLiteral("shelves")).toArray();
        QCOMPARE(shelves.size(), 1);
        const QJsonArray bookIds = shelves.first().toObject()
                                       .value(QStringLiteral("bookIds")).toArray();
        QCOMPARE(bookIds.size(), 1);

        const QString bookPath = QDir(libraryPath).filePath(bookIds.first().toString());
        QFile bookFile(QDir(bookPath).filePath(QStringLiteral("book.json")));
        QVERIFY(bookFile.open(QIODevice::ReadOnly));
        const QJsonObject book = QJsonDocument::fromJson(bookFile.readAll()).object();
        QVERIFY(book.value(QStringLiteral("chapterOrder")).toArray().isEmpty());
        QVERIFY(QFileInfo::exists(QDir(bookPath).filePath(QStringLiteral("outline.html"))));

        auto *editor = window.findChild<QPlainTextEdit *>("chapter-editor");
        auto *title = window.findChild<QLabel *>("chapter-title");
        QVERIFY(editor);
        QVERIFY(title);
        QVERIFY(editor->isVisible());
        QVERIFY(!editor->isReadOnly());
        QVERIFY(title->text().contains(QStringLiteral("Outline")));
        QCOMPARE(editor->font().family(), selectedBodyFont);
        editor->setPlainText(QStringLiteral("Chapter one begins."));
        auto *save = window.findChild<QPushButton *>("chapter-save");
        QVERIFY(save);
        QVERIFY(save->isEnabled());
        save->click();
        QVERIFY(window.findChild<QLabel *>("chapter-save-state")
                    ->text().contains(QStringLiteral("Outline text is editable. Changes are saved.")));
        const QByteArray outlineBytes =
            readFile(QDir(bookPath).filePath(QStringLiteral("outline.html")));
        QVERIFY(outlineBytes.contains(QByteArrayLiteral("Chapter one begins.")));

        const QString returningChapterId = QStringLiteral("returning-chapter");
        QJsonObject returningBook = book;
        returningBook.insert(QStringLiteral("chapterOrder"), QJsonArray{returningChapterId});
        returningBook.insert(QStringLiteral("chapterTitles"),
                             QJsonObject{{returningChapterId, QStringLiteral("Chapter 1")}});
        writeFile(QDir(bookPath).filePath(QStringLiteral("book.json")),
                  QJsonDocument(returningBook).toJson(QJsonDocument::Indented));
        QVERIFY(QDir().mkpath(QDir(bookPath).filePath(QStringLiteral("chapters"))));
        writeFile(QDir(bookPath).filePath(QStringLiteral("chapters/") + returningChapterId +
                                         QStringLiteral(".html")),
                  QByteArrayLiteral("<p>Already started.</p>\n"));

        const auto originalHashes = libraryFileHashes(libraryPath);
        window.close();
        QApplication::processEvents();

        LibraryWindow reopened;
        QVERIFY(reopened.openLibrary(libraryPath));
        reopened.show();
        QApplication::processEvents();
        auto *onboarding = reopened.findChild<QWidget *>("library-onboarding");
        auto *tree = reopened.findChild<QTreeWidget *>("library-tree");
        QVERIFY(onboarding);
        QVERIFY(onboarding->isHidden());
        QVERIFY(tree);
        auto *reopenedEditor = reopened.findChild<QPlainTextEdit *>("chapter-editor");
        auto *reopenedTitle = reopened.findChild<QLabel *>("chapter-title");
        QVERIFY(reopenedEditor);
        QVERIFY(reopenedTitle->text().contains(QStringLiteral("Outline")));
        QVERIFY(!reopenedEditor->isReadOnly());
        QCOMPARE(reopenedEditor->toPlainText(), QStringLiteral("Chapter one begins."));
        QTreeWidgetItem *bookItem = tree->topLevelItem(0)->child(0)->child(0);
        QTreeWidgetItem *outlineItem = nullptr;
        for (int childIndex = 0; childIndex < bookItem->childCount(); ++childIndex) {
            if (bookItem->child(childIndex)->text(0) == QStringLiteral("Outline")) {
                outlineItem = bookItem->child(childIndex);
                break;
            }
        }
        QVERIFY(outlineItem);
        QCOMPARE(reopenedEditor->font().family(), selectedBodyFont);
        QCOMPARE(libraryFileHashes(libraryPath), originalHashes);
    }

    void cancelAndExistingLocationLeaveLibraryFilesUntouched()
    {
        QTemporaryDir existing = makeLibrary();
        const auto originalHashes = libraryFileHashes(existing.path());

        QTemporaryDir locationRoot;
        QVERIFY(locationRoot.isValid());
        const QString canceledPath = QDir(locationRoot.path()).filePath("Canceled Library");
        LibraryWindow window;
        window.show();
        QApplication::processEvents();
        auto *createButton = window.findChild<QPushButton *>("new-library-button");
        QVERIFY(createButton);
        createButton->click();
        auto *location = window.findChild<QLineEdit *>("onboarding-location");
        auto *submit = window.findChild<QPushButton *>("onboarding-submit");
        auto *cancel = window.findChild<QPushButton *>("onboarding-cancel");
        auto *error = window.findChild<QLabel *>("onboarding-error");
        QVERIFY(location);
        QVERIFY(submit);
        QVERIFY(cancel);
        QVERIFY(error);

        location->setText(existing.path());
        submit->click();
        QVERIFY(error->text().contains(QStringLiteral("already exists")));
        QCOMPARE(libraryFileHashes(existing.path()), originalHashes);

        location->setText(canceledPath);
        cancel->click();
        QVERIFY(!QFileInfo::exists(canceledPath));
        QVERIFY(window.findChild<QWidget *>("library-welcome")->isVisible());
        QCOMPARE(libraryFileHashes(existing.path()), originalHashes);

        LibraryWindow returningWriter;
        QVERIFY(returningWriter.openLibrary(existing.path()));
        returningWriter.show();
        QApplication::processEvents();
        QMenu *fileMenu = returningWriter.menuBar()->actions().first()->menu();
        QVERIFY(fileMenu);
        QAction *newLibraryAction = nullptr;
        for (QAction *action : fileMenu->actions()) {
            if (action->text() == QStringLiteral("New Library…")) {
                newLibraryAction = action;
                break;
            }
        }
        QVERIFY(newLibraryAction);
        newLibraryAction->trigger();
        auto *returningCancel = returningWriter.findChild<QPushButton *>("onboarding-cancel");
        QVERIFY(returningCancel);
        returningCancel->click();
        QVERIFY(returningWriter.findChild<QTreeWidget *>("library-tree")->isVisible());
        QCOMPARE(libraryFileHashes(existing.path()), originalHashes);
    }

    void pantserSetupOpensWritableFirstChapter()
    {
        QTemporaryDir privateData;
        QVERIFY(privateData.isValid());
        ScopedEnvironmentVariable dataHome("XDG_DATA_HOME", privateData.path().toLocal8Bit());
        QTemporaryDir privateState;
        QVERIFY(privateState.isValid());
        ScopedEnvironmentVariable stateHome("XDG_STATE_HOME", privateState.path().toLocal8Bit());
        QTemporaryDir locationRoot;
        QVERIFY(locationRoot.isValid());
        const QString libraryPath = QDir(locationRoot.path()).filePath("Pantser Library");

        LibraryWindow window;
        window.show();
        QApplication::processEvents();
        auto *createButton = window.findChild<QPushButton *>("new-library-button");
        QVERIFY(createButton);
        createButton->click();
        auto *location = window.findChild<QLineEdit *>("onboarding-location");
        auto *submit = window.findChild<QPushButton *>("onboarding-submit");
        QVERIFY(location);
        QVERIFY(submit);
        location->setText(libraryPath);
        submit->click();
        QApplication::processEvents();

        QFile libraryFile(QDir(libraryPath).filePath(QStringLiteral("library.json")));
        QVERIFY(libraryFile.open(QIODevice::ReadOnly));
        const QJsonObject library = QJsonDocument::fromJson(libraryFile.readAll()).object();
        QCOMPARE(library.value(QStringLiteral("writingStyle")).toString(), QStringLiteral("pantser"));
        QCOMPARE(library.value(QStringLiteral("authorName")).toString(), QStringLiteral("Anonymous"));
        const QString bookId = library.value(QStringLiteral("shelves")).toArray().first()
                                   .toObject().value(QStringLiteral("bookIds")).toArray().first()
                                   .toString();
        QFile bookFile(QDir(libraryPath).filePath(bookId + QStringLiteral("/book.json")));
        QVERIFY(bookFile.open(QIODevice::ReadOnly));
        const QJsonObject book = QJsonDocument::fromJson(bookFile.readAll()).object();
        const QJsonArray chapters = book.value(QStringLiteral("chapterOrder")).toArray();
        QCOMPARE(chapters.size(), 1);

        auto *editor = window.findChild<QPlainTextEdit *>("chapter-editor");
        auto *title = window.findChild<QLabel *>("chapter-title");
        auto *state = window.findChild<QLabel *>("chapter-save-state");
        auto *save = window.findChild<QPushButton *>("chapter-save");
        QVERIFY(editor);
        QVERIFY(title);
        QVERIFY(state);
        QVERIFY(save);
        QVERIFY(title->text().contains(QStringLiteral("Chapter 1")));
        QVERIFY(!editor->isReadOnly());
        editor->setPlainText(QStringLiteral("The first sentence."));
        QApplication::processEvents();
        QVERIFY(save->isEnabled());
        save->click();
        const QString chapterPath = QDir(libraryPath).filePath(
            bookId + QStringLiteral("/chapters/") + chapters.first().toString() +
            QStringLiteral(".html"));
        QVERIFY2(readFile(chapterPath).contains(QByteArrayLiteral("<p>The first sentence.</p>")),
                 qPrintable(state->text()));
    }

    void invalidOnboardingChoicesUseSafeDefaults()
    {
        QTemporaryDir privateData;
        QVERIFY(privateData.isValid());
        ScopedEnvironmentVariable dataHome("XDG_DATA_HOME", privateData.path().toLocal8Bit());
        QTemporaryDir privateState;
        QVERIFY(privateState.isValid());
        ScopedEnvironmentVariable stateHome("XDG_STATE_HOME", privateState.path().toLocal8Bit());
        QTemporaryDir locationRoot;
        QVERIFY(locationRoot.isValid());
        const QString libraryPath = QDir(locationRoot.path()).filePath("Fallback Library");

        LibraryWindow window;
        window.show();
        QApplication::processEvents();
        auto *createButton = window.findChild<QPushButton *>("new-library-button");
        QVERIFY(createButton);
        createButton->click();

        auto *location = window.findChild<QLineEdit *>("onboarding-location");
        auto *mode = window.findChild<QComboBox *>("onboarding-mode");
        auto *bodyFont = window.findChild<QComboBox *>("onboarding-body-font");
        auto *dropCap = window.findChild<QComboBox *>("onboarding-drop-cap");
        auto *submit = window.findChild<QPushButton *>("onboarding-submit");
        QVERIFY(location);
        QVERIFY(mode);
        QVERIFY(bodyFont);
        QVERIFY(dropCap);
        QVERIFY(submit);
        location->setText(libraryPath);
        mode->setCurrentIndex(-1);
        bodyFont->setCurrentIndex(-1);
        dropCap->setCurrentIndex(-1);
        submit->click();

        const QJsonObject library = QJsonDocument::fromJson(
                                       readFile(QDir(libraryPath).filePath(
                                           QStringLiteral("library.json"))))
                                       .object();
        QCOMPARE(library.value(QStringLiteral("writingStyle")).toString(), QStringLiteral("pantser"));
        const QJsonObject fonts = library.value(QStringLiteral("fonts")).toObject();
        QVERIFY(!fonts.value(QStringLiteral("body")).toString().isEmpty());
        QCOMPARE(fonts.value(QStringLiteral("dropcap")).toString(), QStringLiteral("literary"));
        auto *title = window.findChild<QLabel *>("chapter-title");
        auto *state = window.findChild<QLabel *>("chapter-save-state");
        QVERIFY(title);
        QVERIFY(state);
        QVERIFY(title->text().contains(QStringLiteral("Chapter 1")));
        QVERIFY(state->text().contains(QStringLiteral("safe defaults")));
    }

    void unavailableSavedPreferencesUseFallbackWithoutLibraryWrites()
    {
        QTemporaryDir library = makeSingleChapterLibrary(QByteArrayLiteral("<p>Opening line.</p>"));
        QVERIFY(library.isValid());
        QJsonObject metadata = QJsonDocument::fromJson(
                                   readFile(QDir(library.path()).filePath(
                                       QStringLiteral("library.json"))))
                                   .object();
        metadata.insert(QStringLiteral("writingStyle"), QStringLiteral("unknown-mode"));
        metadata.insert(QStringLiteral("fonts"), QJsonObject{
            {QStringLiteral("body"), QStringLiteral("Font That Does Not Exist")},
            {QStringLiteral("dropcap"), QStringLiteral("invalid-style")}});
        writeFile(QDir(library.path()).filePath(QStringLiteral("library.json")),
                  QJsonDocument(metadata).toJson(QJsonDocument::Indented));
        const auto originalHashes = libraryFileHashes(library.path());

        LibraryWindow window;
        QVERIFY(window.openLibrary(library.path()));
        openSingleChapter(&window);
        auto *editor = window.findChild<QPlainTextEdit *>("chapter-editor");
        auto *state = window.findChild<QLabel *>("chapter-save-state");
        QVERIFY(editor);
        QVERIFY(state);
        QVERIFY(!editor->isReadOnly());
        QVERIFY(editor->font().family() != QStringLiteral("Font That Does Not Exist"));
        QVERIFY(state->text().contains(QStringLiteral("unavailable")));
        QVERIFY(state->text().contains(QStringLiteral("Unknown writing mode")));
        QVERIFY(state->text().contains(QStringLiteral("Unknown drop-cap choice")));

        window.close();
        QApplication::processEvents();
        QCOMPARE(libraryFileHashes(library.path()), originalHashes);
    }

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

    void libraryBrowserExposesContextOrganizationAndDragReordering()
    {
        QTemporaryDir library = makeLibrary();
        LibraryWindow window;
        QVERIFY(window.openLibrary(library.path()));
        window.show();
        QApplication::processEvents();

        auto *tree = window.findChild<QTreeWidget *>("library-tree");
        QVERIFY(tree);
        QVERIFY(tree->dragEnabled());
        QCOMPARE(tree->dragDropMode(), QAbstractItemView::InternalMove);
        QCOMPARE(tree->contextMenuPolicy(), Qt::CustomContextMenu);
        QVERIFY(!window.findChild<QPushButton *>("library-new-book"));
        QVERIFY(!window.findChild<QPushButton *>("library-new-shelf"));
        QVERIFY(!window.findChild<QPushButton *>("library-pen-names"));
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
        QVERIFY(!tree->isVisible());

        writeFile(QDir(temporary.path()).filePath("library.json"), QByteArray("{broken"));
        QVERIFY(!window.openLibrary(temporary.path()));
        QVERIFY(refusal->isVisible());
        QVERIFY(refusal->text().contains("JSON"));
        QVERIFY(!tree->isVisible());
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

    void prepareDeviceHandoffRequiresAnOpenLibrary()
    {
        LibraryWindow window;
        window.show();
        QApplication::processEvents();
        const HandoffDialogResult result = prepareDeviceHandoff(
            &window, [](QMessageBox *dialog) {
                if (dialog->windowTitle() == QStringLiteral("No Library open")) {
                    return dialog->button(QMessageBox::Ok);
                }
                return buttonWithText(dialog, QStringLiteral("Keep LEO Open"));
            });

        QVERIFY(result.shown);
        QVERIFY(result.answered);
        QCOMPARE(result.title, QStringLiteral("No Library open"));
        QVERIFY(window.isVisible());
    }

    void prepareDeviceHandoffSavesAndClosesNeoLibrary()
    {
        QTemporaryDir privateData;
        QTemporaryDir privateState;
        QVERIFY(privateData.isValid());
        QVERIFY(privateState.isValid());
        ScopedEnvironmentVariable dataHome("XDG_DATA_HOME", privateData.path().toLocal8Bit());
        ScopedEnvironmentVariable stateHome("XDG_STATE_HOME", privateState.path().toLocal8Bit());

        const QByteArray protectedMarker(
            "<p>Question <span class=\"ph-mark\" data-sid=\"s-existing\" "
            "contenteditable=\"false\">⚑</span></p>");
        const QByteArray futureMarkup(
            "<div data-future=\"keep&amp;exact\"><span>future</span></div>");
        const QByteArray originalChapter = QByteArrayLiteral("<p>Before prose.</p>") +
                                           protectedMarker + futureMarkup +
                                           QByteArrayLiteral("<p>After prose.</p>");
        const QByteArray stickies(R"json([
  {
    "id": "s-existing",
    "chapterId": "chapter-a",
    "text": "keep this link",
    "resolved": false,
    "futureStickyField": { "keep": true }
  }
])json");
        const QByteArray darlings(R"json([
  {
    "id": "d-existing",
    "html": "<p>Saved line</p>",
    "text": "Saved line",
    "chapterId": "chapter-a",
    "chapterLabel": "Chapter 1",
    "anchorPrefix": "before",
    "anchorSuffix": "after",
    "date": "2026-09-20T00:00:00.000Z",
    "futureDarlingField": { "keep": true }
  }
])json");
        QTemporaryDir library = makeNeoLibrary(originalChapter, stickies, darlings);
        QVERIFY(library.isValid());
        QMap<QString, QByteArray> originalHashes = libraryFileHashes(library.path());

        LibraryWindow window;
        QVERIFY(window.openLibrary(library.path()));
        openSingleChapter(&window);
        auto *editor = window.findChild<QPlainTextEdit *>("chapter-editor");
        QVERIFY(editor);
        QVERIFY(!editor->isReadOnly());
        QString revisedText = editor->toPlainText();
        revisedText.replace(QStringLiteral("Before prose."), QStringLiteral("Revised prose."));
        editor->setPlainText(revisedText);

        bool saveWasCompleteBeforeCloseChoice = false;
        const HandoffDialogResult result = prepareDeviceHandoff(
            &window, [&](QMessageBox *dialog) {
                saveWasCompleteBeforeCloseChoice =
                    readFile(QDir(library.path()).filePath(
                        QStringLiteral("book-1/chapters/chapter-a.html")))
                        .contains(QByteArrayLiteral("Revised prose."));
                return buttonWithText(dialog, QStringLiteral("Close LEO"));
            });

        QVERIFY(result.shown);
        QVERIFY(result.answered);
        QVERIFY(saveWasCompleteBeforeCloseChoice);
        QVERIFY(result.text.contains(QStringLiteral("Syncthing")));
        QVERIFY(result.text.contains(QStringLiteral("Up to Date")));
        QVERIFY(result.text.contains(QStringLiteral("one device at a time")));
        QCOMPARE(result.title, QStringLiteral("Device handoff"));
        QVERIFY(!window.isVisible());

        const QString chapterPath = QDir(library.path()).filePath(
            QStringLiteral("book-1/chapters/chapter-a.html"));
        const QByteArray savedChapter = readFile(chapterPath);
        QVERIFY(savedChapter.contains(QByteArrayLiteral("Revised prose.")));
        QVERIFY(savedChapter.contains(protectedMarker));
        QVERIFY(savedChapter.contains(futureMarkup));
        QMap<QString, QByteArray> savedHashes = libraryFileHashes(library.path());
        originalHashes.remove(QStringLiteral("book-1/chapters/chapter-a.html"));
        savedHashes.remove(QStringLiteral("book-1/chapters/chapter-a.html"));
        QCOMPARE(savedHashes, originalHashes);

        LibraryWindow reopened;
        QVERIFY(reopened.openLibrary(library.path()));
        openSingleChapter(&reopened);
        editor = reopened.findChild<QPlainTextEdit *>("chapter-editor");
        QVERIFY(editor);
        QVERIFY(editor->toPlainText().contains(QStringLiteral("Revised prose.")));
    }

    void prepareDeviceHandoffKeepsLeoOpenWhenSaveFails()
    {
        QTemporaryDir privateData;
        QVERIFY(privateData.isValid());
        const QString blockedStateHome = QDir(privateData.path()).filePath(
            QStringLiteral("blocked-state-home"));
        writeFile(blockedStateHome, QByteArrayLiteral("not a directory"));
        ScopedEnvironmentVariable dataHome("XDG_DATA_HOME", privateData.path().toLocal8Bit());
        ScopedEnvironmentVariable stateHome("XDG_STATE_HOME", blockedStateHome.toLocal8Bit());

        const QByteArray savedBytes("<p>Saved paragraph.</p>");
        QTemporaryDir sourceLibrary = makeSingleChapterLibrary(savedBytes);
        QVERIFY(sourceLibrary.isValid());
        LibraryWindow window;
        QVERIFY(window.openLibrary(sourceLibrary.path()));
        openSingleChapter(&window);
        auto *editor = window.findChild<QPlainTextEdit *>("chapter-editor");
        auto *saveButton = window.findChild<QPushButton *>("chapter-save");
        QVERIFY(editor);
        QVERIFY(saveButton);
        editor->setPlainText(QStringLiteral("Draft that cannot be saved yet."));
        QVERIFY(saveButton->isEnabled());

        const HandoffDialogResult result = prepareDeviceHandoff(
            &window, [](QMessageBox *dialog) {
                return dialog->windowTitle() ==
                               QStringLiteral("Device handoff is not ready")
                           ? dialog->button(QMessageBox::Ok)
                           : nullptr;
            });

        QVERIFY(result.shown);
        QVERIFY(result.answered);
        QCOMPARE(result.title, QStringLiteral("Device handoff is not ready"));
        QVERIFY(result.text.contains(QStringLiteral("could not finish saving")));
        QVERIFY(window.isVisible());
        QVERIFY(editor->toPlainText().contains(
            QStringLiteral("Draft that cannot be saved yet.")));
        QCOMPARE(saveButton->text(), QStringLiteral("Retry Save"));
        QCOMPARE(readFile(QDir(sourceLibrary.path()).filePath(
                     QStringLiteral("book-1/chapters/chapter-a.html"))),
                 savedBytes);
    }

    void editsSafeProseAroundProtectedContentWithoutChangingIt()
    {
        QTemporaryDir privateData;
        QVERIFY(privateData.isValid());
        ScopedEnvironmentVariable dataHome("XDG_DATA_HOME", privateData.path().toLocal8Bit());
        QTemporaryDir privateState;
        QVERIFY(privateState.isValid());
        ScopedEnvironmentVariable stateHome("XDG_STATE_HOME", privateState.path().toLocal8Bit());

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
        QTemporaryDir privateState;
        QVERIFY(privateState.isValid());
        ScopedEnvironmentVariable stateHome("XDG_STATE_HOME", privateState.path().toLocal8Bit());

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

        const HandoffDialogResult handoff = prepareDeviceHandoff(
            &window, [](QMessageBox *dialog) {
                return buttonWithText(dialog, QStringLiteral("Keep LEO Open"));
            });
        QVERIFY(handoff.shown);
        QVERIFY(handoff.answered);
        QCOMPARE(handoff.title, QStringLiteral("Device handoff"));
        QVERIFY(handoff.text.contains(QStringLiteral("draft outside the shared Library")));
        QVERIFY(handoff.text.contains(QStringLiteral("Recovered Library is ready")));
        QVERIFY(window.isVisible());
        QVERIFY(switchButton->isVisible());

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

    void secondEnterCreatesAndPersistsANeoSceneBreak()
    {
        QTemporaryDir privateData;
        QTemporaryDir privateState;
        QVERIFY(privateData.isValid());
        QVERIFY(privateState.isValid());
        ScopedEnvironmentVariable dataHome("XDG_DATA_HOME", privateData.path().toLocal8Bit());
        ScopedEnvironmentVariable stateHome("XDG_STATE_HOME", privateState.path().toLocal8Bit());
        QTemporaryDir library = makeSingleChapterLibrary(QByteArrayLiteral("<p>Before.</p>"));
        QVERIFY(library.isValid());

        LibraryWindow window;
        QVERIFY(window.openLibrary(library.path()));
        openSingleChapter(&window);
        auto *editor = window.findChild<QPlainTextEdit *>("chapter-editor");
        auto *save = window.findChild<QPushButton *>("chapter-save");
        QVERIFY(editor);
        QVERIFY(save);
        editor->moveCursor(QTextCursor::End);
        QTest::keyClick(editor, Qt::Key_Return);
        QTest::keyClick(editor, Qt::Key_Return);
        QApplication::processEvents();
        QCOMPARE(editor->toPlainText(), QStringLiteral("Before.\n***\n"));

        save->click();
        QApplication::processEvents();
        const QByteArray saved = readFile(QDir(library.path()).filePath(
            QStringLiteral("book-1/chapters/chapter-a.html")));
        QVERIFY(saved.contains(QByteArrayLiteral("<p class=\"scene-break\">***</p>")));
        const LegacyChapterDocument decoded = LegacyChapterCodec::decode(saved);
        QVERIFY(decoded.editable());
        QVERIFY(decoded.text.contains(QStringLiteral("***")));
    }

    void chapterNavigationSavesPendingTextAndFollowsBookOrder()
    {
        QTemporaryDir privateData;
        QTemporaryDir privateState;
        QVERIFY(privateData.isValid());
        QVERIFY(privateState.isValid());
        ScopedEnvironmentVariable dataHome("XDG_DATA_HOME", privateData.path().toLocal8Bit());
        ScopedEnvironmentVariable stateHome("XDG_STATE_HOME", privateState.path().toLocal8Bit());
        QTemporaryDir library = makeSingleChapterLibrary(QByteArrayLiteral("<p>First.</p>"));
        QVERIFY(library.isValid());
        const QString bookPath = QDir(library.path()).filePath(QStringLiteral("book-1"));
        QJsonObject book = QJsonDocument::fromJson(readFile(
            QDir(bookPath).filePath(QStringLiteral("book.json")))).object();
        book.insert(QStringLiteral("chapterOrder"),
                    QJsonArray{QStringLiteral("chapter-a"), QStringLiteral("chapter-b")});
        QJsonObject titles;
        titles.insert(QStringLiteral("chapter-a"), QStringLiteral("Arrival"));
        titles.insert(QStringLiteral("chapter-b"), QStringLiteral("Crossing"));
        book.insert(QStringLiteral("chapterTitles"), titles);
        writeFile(QDir(bookPath).filePath(QStringLiteral("book.json")),
                  QJsonDocument(book).toJson());
        writeFile(QDir(bookPath).filePath(QStringLiteral("chapters/chapter-b.html")),
                  QByteArrayLiteral("<p>Second.</p>"));

        LibraryWindow window;
        QVERIFY(window.openLibrary(library.path()));
        openSingleChapter(&window);
        auto *editor = window.findChild<QPlainTextEdit *>("chapter-editor");
        auto *previous = window.findChild<QPushButton *>("chapter-previous");
        auto *next = window.findChild<QPushButton *>("chapter-next");
        QVERIFY(editor);
        QVERIFY(previous);
        QVERIFY(next);
        QVERIFY(!previous->isEnabled());
        QVERIFY(next->isEnabled());
        editor->setPlainText(QStringLiteral("Edited first."));
        next->click();
        QCOMPARE(editor->toPlainText(), QStringLiteral("Second."));
        QVERIFY(previous->isEnabled());
        QVERIFY(!next->isEnabled());
        QCOMPARE(readFile(QDir(bookPath).filePath(QStringLiteral("chapters/chapter-a.html"))),
                 QByteArrayLiteral("<p>Edited first.</p>"));
        previous->click();
        QCOMPARE(editor->toPlainText(), QStringLiteral("Edited first."));
    }

    void tripleEnterSplitsAndStructuralUndoRestoresTheChapter()
    {
        QTemporaryDir privateData;
        QTemporaryDir privateState;
        QVERIFY(privateData.isValid());
        QVERIFY(privateState.isValid());
        ScopedEnvironmentVariable dataHome("XDG_DATA_HOME", privateData.path().toLocal8Bit());
        ScopedEnvironmentVariable stateHome("XDG_STATE_HOME", privateState.path().toLocal8Bit());
        const QByteArray original("<p>Before.</p>");
        QTemporaryDir library = makeSingleChapterLibrary(original);
        QVERIFY(library.isValid());

        LibraryWindow window;
        QVERIFY(window.openLibrary(library.path()));
        openSingleChapter(&window);
        auto *editor = window.findChild<QPlainTextEdit *>("chapter-editor");
        QVERIFY(editor);
        editor->moveCursor(QTextCursor::End);
        QTest::keyClick(editor, Qt::Key_Return);
        QTest::keyClick(editor, Qt::Key_Enter);
        QTest::keyClick(editor, Qt::Key_Enter);
        QApplication::processEvents();

        QFile bookFile(QDir(library.path()).filePath(QStringLiteral("book-1/book.json")));
        QVERIFY(bookFile.open(QIODevice::ReadOnly));
        QJsonObject book = QJsonDocument::fromJson(bookFile.readAll()).object();
        QJsonArray order = book.value(QStringLiteral("chapterOrder")).toArray();
        QCOMPARE(order.size(), 2);
        QCOMPARE(order.first().toString(), QStringLiteral("chapter-a"));
        const QString addedId = order.last().toString();
        QCOMPARE(readFile(QDir(library.path()).filePath(
                     QStringLiteral("book-1/chapters/chapter-a.html"))), original);
        QVERIFY(QFileInfo::exists(QDir(library.path()).filePath(
            QStringLiteral("book-1/chapters/") + addedId + QStringLiteral(".html"))));

        QAction *undo = window.findChild<QAction *>("chapter-structure-undo");
        QVERIFY(undo);
        QVERIFY(undo->isEnabled());
        undo->trigger();
        QApplication::processEvents();
        book = QJsonDocument::fromJson(readFile(QDir(library.path()).filePath(
                                    QStringLiteral("book-1/book.json"))))
                   .object();
        order = book.value(QStringLiteral("chapterOrder")).toArray();
        QCOMPARE(order, QJsonArray{QStringLiteral("chapter-a")});
        QCOMPARE(readFile(QDir(library.path()).filePath(
                     QStringLiteral("book-1/chapters/chapter-a.html"))), original);
        QAction *redo = window.findChild<QAction *>("chapter-structure-redo");
        QVERIFY(redo);
        QVERIFY(redo->isEnabled());
    }
};

QTEST_MAIN(LibraryBrowserTest)
#include "library_browser_test.moc"
