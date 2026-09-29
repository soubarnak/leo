#include "library_organization.h"
#include "library_reader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

namespace {

void writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        qFatal("Could not create Library organization fixture");
    }
}

void writeBook(const QString &root, const QString &id, const QString &title,
               const QString &author)
{
    const QString directory = QDir(root).filePath(id);
    if (!QDir().mkpath(QDir(directory).filePath(QStringLiteral("chapters")))) {
        qFatal("Could not create book folder in Library organization fixture");
    }
    const QJsonObject metadata{
        {QStringLiteral("id"), id},
        {QStringLiteral("title"), title},
        {QStringLiteral("author"), author},
        {QStringLiteral("chapterOrder"), QJsonArray{}}};
    writeFile(QDir(directory).filePath(QStringLiteral("book.json")),
              QJsonDocument(metadata).toJson(QJsonDocument::Indented));
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

void writeLibrary(const QString &root)
{
    const QJsonObject metadata{
        {QStringLiteral("authors"), QJsonArray{
             QJsonObject{{QStringLiteral("id"), QStringLiteral("a1")},
                         {QStringLiteral("name"), QStringLiteral("Ada")}},
             QJsonObject{{QStringLiteral("id"), QStringLiteral("a2")},
                         {QStringLiteral("name"), QStringLiteral("Babbage")}}}},
        {QStringLiteral("authorName"), QStringLiteral("Ada")},
        {QStringLiteral("penNames"), QJsonArray{QStringLiteral("A. L.")}},
        {QStringLiteral("currentAuthorId"), QStringLiteral("a1")},
        {QStringLiteral("shelves"), QJsonArray{
             QJsonObject{{QStringLiteral("id"), QStringLiteral("s1")},
                         {QStringLiteral("name"), QStringLiteral("Drafts")},
                         {QStringLiteral("authorId"), QStringLiteral("a1")},
                         {QStringLiteral("bookIds"), QJsonArray{
                              QStringLiteral("book-2"), QStringLiteral("book-1")}}},
             QJsonObject{{QStringLiteral("id"), QStringLiteral("s2")},
                         {QStringLiteral("name"), QStringLiteral("Archive")},
                         {QStringLiteral("authorId"), QStringLiteral("a2")},
                         {QStringLiteral("bookIds"), QJsonArray{QStringLiteral("book-3")}}}}},
        {QStringLiteral("futureLibraryField"), QJsonObject{
             {QStringLiteral("keep"), QJsonArray{1, true, QStringLiteral("future")}}}}};
    writeFile(QDir(root).filePath(QStringLiteral("library.json")),
              QJsonDocument(metadata).toJson(QJsonDocument::Indented));
    writeBook(root, QStringLiteral("book-1"), QStringLiteral("First"), QStringLiteral("Ada"));
    writeBook(root, QStringLiteral("book-2"), QStringLiteral("Second"), QStringLiteral("Ada"));
    writeBook(root, QStringLiteral("book-3"), QStringLiteral("Third"), QStringLiteral("Babbage"));
}

void writePendingTrashMetadata(const QString &root, bool keepBookFolder)
{
    const QString metadataPath = QDir(root).filePath(QStringLiteral("library.json"));
    QFile file(metadataPath);
    if (!file.open(QIODevice::ReadOnly)) {
        qFatal("Could not read test Library metadata for interrupted Trash fixture");
    }
    QJsonObject metadata = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    QJsonArray shelves = metadata.value(QStringLiteral("shelves")).toArray();
    QJsonObject firstShelf = shelves.first().toObject();
    QJsonArray bookIds = firstShelf.value(QStringLiteral("bookIds")).toArray();
    bookIds.removeAt(0);
    firstShelf.insert(QStringLiteral("bookIds"), bookIds);
    shelves.replace(0, firstShelf);
    metadata.insert(QStringLiteral("shelves"), shelves);
    const QJsonObject membership{
        {QStringLiteral("shelfId"), QStringLiteral("s1")},
        {QStringLiteral("index"), 0}};
    const QJsonObject pendingTrash{
        {QStringLiteral("bookId"), QStringLiteral("book-2")},
        {QStringLiteral("memberships"), QJsonArray{membership}}};
    metadata.insert(QStringLiteral("leoPendingTrash"), pendingTrash);
    writeFile(metadataPath, QJsonDocument(metadata).toJson(QJsonDocument::Indented));
    if (!keepBookFolder &&
        !QDir(QDir(root).filePath(QStringLiteral("book-2"))).removeRecursively()) {
        qFatal("Could not simulate a book already moved to Trash");
    }
}

void writePendingCreateMetadata(const QString &root, const QString &stagingPath,
                                const QString &shelfId = QStringLiteral("s1"), int index = 1)
{
    const QString metadataPath = QDir(root).filePath(QStringLiteral("library.json"));
    QFile file(metadataPath);
    if (!file.open(QIODevice::ReadOnly)) {
        qFatal("Could not read test Library metadata for interrupted book fixture");
    }
    QJsonObject metadata = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    metadata.insert(QStringLiteral("leoPendingCreate"), QJsonObject{
        {QStringLiteral("bookId"), QStringLiteral("book-recovered")},
        {QStringLiteral("shelfId"), shelfId},
        {QStringLiteral("index"), index},
        {QStringLiteral("stagingPath"), stagingPath}});
    writeFile(metadataPath, QJsonDocument(metadata).toJson(QJsonDocument::Indented));
}

void writePendingMoveMetadata(const QString &root, const QString &bookId,
                              const QString &shelfId, int index,
                              const QString &previousAuthor, const QString &authorName)
{
    const QString metadataPath = QDir(root).filePath(QStringLiteral("library.json"));
    QFile file(metadataPath);
    if (!file.open(QIODevice::ReadOnly)) {
        qFatal("Could not read test Library metadata for interrupted move fixture");
    }
    QJsonObject metadata = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    metadata.insert(QStringLiteral("leoPendingMove"), QJsonObject{
        {QStringLiteral("bookId"), bookId},
        {QStringLiteral("shelfId"), shelfId},
        {QStringLiteral("index"), index},
        {QStringLiteral("previousAuthor"), previousAuthor},
        {QStringLiteral("authorName"), authorName}});
    writeFile(metadataPath, QJsonDocument(metadata).toJson(QJsonDocument::Indented));
}

}

class LibraryOrganizationTest final : public QObject {
    Q_OBJECT

private slots:
    void penNamesAndShelfMembershipSurviveReopen()
    {
        QTemporaryDir privateData;
        QTemporaryDir privateState;
        QTemporaryDir library;
        QVERIFY(privateData.isValid());
        QVERIFY(privateState.isValid());
        QVERIFY(library.isValid());
        ScopedEnvironmentVariable dataHome("XDG_DATA_HOME", privateData.path().toLocal8Bit());
        ScopedEnvironmentVariable stateHome("XDG_STATE_HOME", privateState.path().toLocal8Bit());
        writeLibrary(library.path());

        LibraryOrganization organization(library.path());
        QString error;
        QVERIFY2(organization.load(&error), qPrintable(error));

        const LibraryOrganizationResult penName = organization.addAuthor(QStringLiteral("Grace"));
        QVERIFY2(penName.ok, qPrintable(penName.error));
        QVERIFY(!penName.id.isEmpty());
        QVERIFY2(organization.renameAuthor(QStringLiteral("a1"),
                                           QStringLiteral("Ada Lovelace")).ok,
                 "The first pen name should be renameable.");
        QVERIFY2(organization.removeAuthor(QStringLiteral("a2"), QStringLiteral("a1")).ok,
                 "Removing a pen name should keep its books under the chosen author.");
        QVERIFY2(organization.moveBook(QStringLiteral("book-1"), QStringLiteral("s1"), 0).ok,
                 "A book should be reorderable within its shelf.");

        const LibraryReadResult reordered = LibraryReader::read(library.path());
        QVERIFY2(reordered.ok(), qPrintable(reordered.error));
        QCOMPARE(reordered.library.authors.first().shelves.at(0).books.at(0).id,
                 QStringLiteral("book-1"));
        QCOMPARE(reordered.library.authors.first().shelves.at(0).books.at(1).id,
                 QStringLiteral("book-2"));

        const LibraryOrganizationResult ideas =
            organization.addShelf(QStringLiteral("a1"), QStringLiteral("Ideas"));
        QVERIFY2(ideas.ok, qPrintable(ideas.error));
        QVERIFY2(organization.moveBook(QStringLiteral("book-2"), ideas.id, 0).ok,
                 "A book should move to the selected shelf.");

        LibraryOrganization reopened(library.path());
        QVERIFY2(reopened.load(&error), qPrintable(error));
        const QJsonObject persisted = reopened.metadata();
        QCOMPARE(persisted.value(QStringLiteral("authorName")).toString(),
                 QStringLiteral("Ada Lovelace"));
        QCOMPARE(persisted.value(QStringLiteral("penNames")).toArray(),
                 QJsonArray{QStringLiteral("A. L.")});
        QCOMPARE(persisted.value(QStringLiteral("currentAuthorId")).toString(), penName.id);
        const QJsonArray expectedFutureValues{1, true, QStringLiteral("future")};
        QCOMPARE(persisted.value(QStringLiteral("futureLibraryField")).toObject()
                     .value(QStringLiteral("keep")).toArray(), expectedFutureValues);

        const LibraryReadResult read = LibraryReader::read(library.path());
        QVERIFY2(read.ok(), qPrintable(read.error));
        QCOMPARE(read.library.authors.size(), 2);
        QCOMPARE(read.library.authors.first().name, QStringLiteral("Ada Lovelace"));
        QCOMPARE(read.library.authors.first().shelves.size(), 3);
        QCOMPARE(read.library.authors.first().shelves.at(0).id, QStringLiteral("s1"));
        QCOMPARE(read.library.authors.first().shelves.at(0).books.size(), 1);
        QCOMPARE(read.library.authors.first().shelves.at(0).books.first().id,
                 QStringLiteral("book-1"));
        QCOMPARE(read.library.authors.first().shelves.at(1).id, QStringLiteral("s2"));
        QCOMPARE(read.library.authors.first().shelves.at(1).books.size(), 1);
        QCOMPARE(read.library.authors.first().shelves.at(1).books.first().id,
                 QStringLiteral("book-3"));
        QCOMPARE(read.library.authors.first().shelves.at(2).id, ideas.id);
        QCOMPARE(read.library.authors.first().shelves.at(2).books.size(), 1);
        QCOMPARE(read.library.authors.first().shelves.at(2).books.first().id,
                 QStringLiteral("book-2"));
        QVERIFY(QFileInfo::exists(QDir(library.path()).filePath(
            QStringLiteral("book-3/book.json"))));
    }

    void movingBookBetweenPenNamesUpdatesPersistentAuthorAndMembership()
    {
        QTemporaryDir privateData;
        QTemporaryDir privateState;
        QTemporaryDir library;
        QVERIFY(privateData.isValid());
        QVERIFY(privateState.isValid());
        QVERIFY(library.isValid());
        ScopedEnvironmentVariable dataHome("XDG_DATA_HOME", privateData.path().toLocal8Bit());
        ScopedEnvironmentVariable stateHome("XDG_STATE_HOME", privateState.path().toLocal8Bit());
        writeLibrary(library.path());

        LibraryOrganization organization(library.path());
        QString error;
        QVERIFY2(organization.load(&error), qPrintable(error));
        const LibraryOrganizationResult moved =
            organization.moveBook(QStringLiteral("book-3"), QStringLiteral("s1"), 0);
        QVERIFY2(moved.ok, qPrintable(moved.error));

        LibraryOrganization reopened(library.path());
        QVERIFY2(reopened.load(&error), qPrintable(error));
        const LibraryReadResult read = LibraryReader::read(library.path());
        QVERIFY2(read.ok(), qPrintable(read.error));
        QCOMPARE(read.library.authors.first().shelves.first().books.first().id,
                 QStringLiteral("book-3"));
        QCOMPARE(read.library.authors.first().shelves.first().books.first().author,
                 QStringLiteral("Ada"));
        QCOMPARE(read.library.authors.at(1).shelves.first().books.size(), 0);
        QVERIFY(QFileInfo::exists(QDir(library.path()).filePath(
            QStringLiteral("book-3/book.json"))));
    }

    void interruptedBookAuthorMoveFinishesOnReopen()
    {
        QTemporaryDir privateData;
        QTemporaryDir privateState;
        QTemporaryDir library;
        QVERIFY(privateData.isValid());
        QVERIFY(privateState.isValid());
        QVERIFY(library.isValid());
        ScopedEnvironmentVariable dataHome("XDG_DATA_HOME", privateData.path().toLocal8Bit());
        ScopedEnvironmentVariable stateHome("XDG_STATE_HOME", privateState.path().toLocal8Bit());
        writeLibrary(library.path());

        const QString bookPath = QDir(library.path()).filePath(
            QStringLiteral("book-3/book.json"));
        QFile bookFile(bookPath);
        QVERIFY(bookFile.open(QIODevice::ReadOnly));
        QJsonObject book = QJsonDocument::fromJson(bookFile.readAll()).object();
        bookFile.close();
        book.insert(QStringLiteral("author"), QStringLiteral("Ada"));
        writeFile(bookPath, QJsonDocument(book).toJson(QJsonDocument::Indented));
        writePendingMoveMetadata(library.path(), QStringLiteral("book-3"),
                                QStringLiteral("s1"), 0,
                                QStringLiteral("Babbage"), QStringLiteral("Ada"));

        LibraryOrganization organization(library.path());
        QString error;
        QVERIFY2(organization.load(&error), qPrintable(error));
        QVERIFY(!organization.metadata().contains(QStringLiteral("leoPendingMove")));

        const LibraryReadResult read = LibraryReader::read(library.path());
        QVERIFY2(read.ok(), qPrintable(read.error));
        QCOMPARE(read.library.authors.first().shelves.first().books.first().id,
                 QStringLiteral("book-3"));
        QCOMPARE(read.library.authors.at(1).shelves.first().books.size(), 0);
    }

    void malformedOrganizationMetadataIsLeftUnchanged()
    {
        QTemporaryDir library;
        QVERIFY(library.isValid());
        writeLibrary(library.path());

        const QString metadataPath = QDir(library.path()).filePath(
            QStringLiteral("library.json"));
        QFile metadataFile(metadataPath);
        QVERIFY(metadataFile.open(QIODevice::ReadOnly));
        QJsonObject metadata = QJsonDocument::fromJson(metadataFile.readAll()).object();
        metadataFile.close();
        metadata.insert(QStringLiteral("shelves"), QStringLiteral("not an array"));
        const QByteArray malformedBytes =
            QJsonDocument(metadata).toJson(QJsonDocument::Indented);
        writeFile(metadataPath, malformedBytes);

        LibraryOrganization organization(library.path());
        QString error;
        QVERIFY(!organization.load(&error));
        QVERIFY(error.contains(QStringLiteral("invalid type")));

        QFile unchangedFile(metadataPath);
        QVERIFY(unchangedFile.open(QIODevice::ReadOnly));
        QCOMPARE(unchangedFile.readAll(), malformedBytes);
    }

    void newBookCanBeRenamedAndReopened()
    {
        QTemporaryDir privateData;
        QTemporaryDir privateState;
        QTemporaryDir library;
        QVERIFY(privateData.isValid());
        QVERIFY(privateState.isValid());
        QVERIFY(library.isValid());
        ScopedEnvironmentVariable dataHome("XDG_DATA_HOME", privateData.path().toLocal8Bit());
        ScopedEnvironmentVariable stateHome("XDG_STATE_HOME", privateState.path().toLocal8Bit());
        writeLibrary(library.path());

        LibraryOrganization organization(library.path());
        QString error;
        QVERIFY2(organization.load(&error), qPrintable(error));
        const LibraryOrganizationResult created =
            organization.createBook(QStringLiteral("s1"), QStringLiteral("New story"));
        QVERIFY2(created.ok, qPrintable(created.error));
        QVERIFY(QFileInfo::exists(QDir(library.path()).filePath(
            created.id + QStringLiteral("/chapters"))));
        QVERIFY2(organization.renameBook(created.id, QStringLiteral("Renamed story")).ok,
                 "Book titles should be saved through the Library persistence layer.");

        const LibraryReadResult reopened = LibraryReader::read(library.path());
        QVERIFY2(reopened.ok(), qPrintable(reopened.error));
        bool found = false;
        for (const Shelf &shelf : reopened.library.authors.first().shelves) {
            for (const Book &book : shelf.books) {
                if (book.id == created.id) {
                    found = true;
                    QCOMPARE(book.title, QStringLiteral("Renamed story"));
                    QCOMPARE(book.chapters.size(), 1);
                }
            }
        }
        QVERIFY(found);
    }

    void deletingShelfMovesBooksToAnotherShelfUnderThatPenName()
    {
        QTemporaryDir privateData;
        QTemporaryDir privateState;
        QTemporaryDir library;
        QVERIFY(privateData.isValid());
        QVERIFY(privateState.isValid());
        QVERIFY(library.isValid());
        ScopedEnvironmentVariable dataHome("XDG_DATA_HOME", privateData.path().toLocal8Bit());
        ScopedEnvironmentVariable stateHome("XDG_STATE_HOME", privateState.path().toLocal8Bit());
        writeLibrary(library.path());

        LibraryOrganization organization(library.path());
        QString error;
        QVERIFY2(organization.load(&error), qPrintable(error));
        const LibraryOrganizationResult replacement =
            organization.addShelf(QStringLiteral("a2"), QStringLiteral("Reading"));
        QVERIFY2(replacement.ok, qPrintable(replacement.error));
        QVERIFY2(organization.removeShelf(QStringLiteral("s2")).ok,
                 "Deleting a shelf should preserve its book membership.");

        const LibraryReadResult reopened = LibraryReader::read(library.path());
        QVERIFY2(reopened.ok(), qPrintable(reopened.error));
        QCOMPARE(reopened.library.authors.at(1).shelves.size(), 1);
        QCOMPARE(reopened.library.authors.at(1).shelves.first().id, replacement.id);
        QCOMPARE(reopened.library.authors.at(1).shelves.first().books.size(), 1);
        QCOMPARE(reopened.library.authors.at(1).shelves.first().books.first().id,
                 QStringLiteral("book-3"));
    }

    void failedTrashKeepsBookAndShelfMembership()
    {
        QTemporaryDir privateData;
        QTemporaryDir privateState;
        QTemporaryDir library;
        QVERIFY(privateData.isValid());
        QVERIFY(privateState.isValid());
        QVERIFY(library.isValid());
        ScopedEnvironmentVariable dataHome("XDG_DATA_HOME", privateData.path().toLocal8Bit());
        ScopedEnvironmentVariable stateHome("XDG_STATE_HOME", privateState.path().toLocal8Bit());
        writeLibrary(library.path());

        LibraryOrganization organization(library.path());
        QString error;
        QVERIFY2(organization.load(&error), qPrintable(error));
        const LibraryOrganizationResult removed = organization.moveBookToTrash(
            QStringLiteral("book-2"), [](const QString &, QString *trashError) {
                *trashError = QStringLiteral("The test trash service is unavailable.");
                return false;
            });
        QVERIFY(!removed.ok);
        QVERIFY(QFileInfo::exists(QDir(library.path()).filePath(
            QStringLiteral("book-2/book.json"))));

        const LibraryReadResult reopened = LibraryReader::read(library.path());
        QVERIFY2(reopened.ok(), qPrintable(reopened.error));
        QCOMPARE(reopened.library.authors.first().shelves.first().books.size(), 2);
        QCOMPARE(reopened.library.authors.first().shelves.first().books.at(0).id,
                 QStringLiteral("book-2"));
    }

    void interruptedTrashRestoresMembershipWhenBookFolderRemains()
    {
        QTemporaryDir privateData;
        QTemporaryDir privateState;
        QTemporaryDir library;
        QVERIFY(privateData.isValid());
        QVERIFY(privateState.isValid());
        QVERIFY(library.isValid());
        ScopedEnvironmentVariable dataHome("XDG_DATA_HOME", privateData.path().toLocal8Bit());
        ScopedEnvironmentVariable stateHome("XDG_STATE_HOME", privateState.path().toLocal8Bit());
        writeLibrary(library.path());
        writePendingTrashMetadata(library.path(), true);

        LibraryOrganization organization(library.path());
        QString error;
        QVERIFY2(organization.load(&error), qPrintable(error));
        QVERIFY(!organization.metadata().contains(QStringLiteral("leoPendingTrash")));

        const LibraryReadResult reopened = LibraryReader::read(library.path());
        QVERIFY2(reopened.ok(), qPrintable(reopened.error));
        QCOMPARE(reopened.library.authors.first().shelves.first().books.size(), 2);
        QCOMPARE(reopened.library.authors.first().shelves.first().books.at(0).id,
                 QStringLiteral("book-2"));
        QCOMPARE(reopened.library.authors.first().shelves.first().books.at(1).id,
                 QStringLiteral("book-1"));
    }

    void interruptedTrashCompletesWhenBookFolderIsGone()
    {
        QTemporaryDir privateData;
        QTemporaryDir privateState;
        QTemporaryDir library;
        QVERIFY(privateData.isValid());
        QVERIFY(privateState.isValid());
        QVERIFY(library.isValid());
        ScopedEnvironmentVariable dataHome("XDG_DATA_HOME", privateData.path().toLocal8Bit());
        ScopedEnvironmentVariable stateHome("XDG_STATE_HOME", privateState.path().toLocal8Bit());
        writeLibrary(library.path());
        writePendingTrashMetadata(library.path(), false);

        LibraryOrganization organization(library.path());
        QString error;
        QVERIFY2(organization.load(&error), qPrintable(error));
        QVERIFY(!organization.metadata().contains(QStringLiteral("leoPendingTrash")));

        const LibraryReadResult reopened = LibraryReader::read(library.path());
        QVERIFY2(reopened.ok(), qPrintable(reopened.error));
        QCOMPARE(reopened.library.authors.first().shelves.first().books.size(), 1);
        QCOMPARE(reopened.library.authors.first().shelves.first().books.first().id,
                 QStringLiteral("book-1"));
        QVERIFY(reopened.library.unfiledBooks.isEmpty());
    }

    void incompleteBookStagingDoesNotBreakLibraryOpen()
    {
        QTemporaryDir library;
        QVERIFY(library.isValid());
        writeLibrary(library.path());
        const QString incomplete = QDir(library.path()).filePath(
            QStringLiteral(".leo-staging/new-book-interrupted"));
        QVERIFY(QDir().mkpath(incomplete));
        writeFile(QDir(incomplete).filePath(QStringLiteral("book.json")),
                  QByteArrayLiteral("{\"id\":\"book-not-yet-created\"}"));

        const LibraryReadResult reopened = LibraryReader::read(library.path());
        QVERIFY2(reopened.ok(), qPrintable(reopened.error));
        QCOMPARE(reopened.library.authors.first().shelves.first().books.size(), 2);
    }

    void interruptedBookCreationFinishesFromStagingFolder()
    {
        QTemporaryDir privateData;
        QTemporaryDir privateState;
        QTemporaryDir library;
        QVERIFY(privateData.isValid());
        QVERIFY(privateState.isValid());
        QVERIFY(library.isValid());
        ScopedEnvironmentVariable dataHome("XDG_DATA_HOME", privateData.path().toLocal8Bit());
        ScopedEnvironmentVariable stateHome("XDG_STATE_HOME", privateState.path().toLocal8Bit());
        writeLibrary(library.path());

        const QString stagingRoot = QDir(library.path()).filePath(QStringLiteral(".leo-staging"));
        QVERIFY(QDir().mkpath(stagingRoot));
        writeBook(stagingRoot, QStringLiteral("book-recovered"),
                  QStringLiteral("Recovered"), QStringLiteral("Ada"));
        writePendingCreateMetadata(library.path(),
                                   QStringLiteral(".leo-staging/book-recovered"));

        LibraryOrganization organization(library.path());
        QString error;
        QVERIFY2(organization.load(&error), qPrintable(error));
        QVERIFY(!organization.metadata().contains(QStringLiteral("leoPendingCreate")));
        QVERIFY(QFileInfo::exists(QDir(library.path()).filePath(
            QStringLiteral("book-recovered/book.json"))));

        const LibraryReadResult reopened = LibraryReader::read(library.path());
        QVERIFY2(reopened.ok(), qPrintable(reopened.error));
        QCOMPARE(reopened.library.authors.first().shelves.first().books.size(), 3);
        QCOMPARE(reopened.library.authors.first().shelves.first().books.at(0).id,
                 QStringLiteral("book-2"));
        QCOMPARE(reopened.library.authors.first().shelves.first().books.at(1).id,
                 QStringLiteral("book-recovered"));
        QCOMPARE(reopened.library.authors.first().shelves.first().books.at(2).id,
                 QStringLiteral("book-1"));
    }

    void interruptedBookCreationRecoversAfterFolderMove()
    {
        QTemporaryDir privateData;
        QTemporaryDir privateState;
        QTemporaryDir library;
        QVERIFY(privateData.isValid());
        QVERIFY(privateState.isValid());
        QVERIFY(library.isValid());
        ScopedEnvironmentVariable dataHome("XDG_DATA_HOME", privateData.path().toLocal8Bit());
        ScopedEnvironmentVariable stateHome("XDG_STATE_HOME", privateState.path().toLocal8Bit());
        writeLibrary(library.path());
        writeBook(library.path(), QStringLiteral("book-recovered"),
                  QStringLiteral("Recovered"), QStringLiteral("Ada"));
        writePendingCreateMetadata(library.path(),
                                   QStringLiteral(".leo-staging/new-book-already-moved"));

        LibraryOrganization organization(library.path());
        QString error;
        QVERIFY2(organization.load(&error), qPrintable(error));
        QVERIFY(!organization.metadata().contains(QStringLiteral("leoPendingCreate")));

        const LibraryReadResult reopened = LibraryReader::read(library.path());
        QVERIFY2(reopened.ok(), qPrintable(reopened.error));
        QCOMPARE(reopened.library.authors.first().shelves.first().books.size(), 3);
        QCOMPARE(reopened.library.authors.first().shelves.first().books.at(0).id,
                 QStringLiteral("book-2"));
        QCOMPARE(reopened.library.authors.first().shelves.first().books.at(1).id,
                 QStringLiteral("book-recovered"));
        QCOMPARE(reopened.library.authors.first().shelves.first().books.at(2).id,
                 QStringLiteral("book-1"));
        QVERIFY(reopened.library.unfiledBooks.isEmpty());
    }

    void interruptedOrganizationSaveRecoversAsACompleteLibrary()
    {
        QTemporaryDir privateData;
        QTemporaryDir privateState;
        QTemporaryDir library;
        QVERIFY(privateData.isValid());
        QVERIFY(privateState.isValid());
        QVERIFY(library.isValid());
        ScopedEnvironmentVariable dataHome("XDG_DATA_HOME", privateData.path().toLocal8Bit());
        ScopedEnvironmentVariable stateHome("XDG_STATE_HOME", privateState.path().toLocal8Bit());
        writeLibrary(library.path());

        const PersistenceCheckpointHook interruptBeforeRename = [](
            PersistenceCheckpoint checkpoint, QString *error) {
            if (checkpoint != PersistenceCheckpoint::BeforeTargetRename) {
                return true;
            }
            *error = QStringLiteral("Simulated interruption during shelf update.");
            return false;
        };
        LibraryOrganization organization(library.path(), interruptBeforeRename);
        QString error;
        QVERIFY2(organization.load(&error), qPrintable(error));
        const LibraryOrganizationResult interrupted =
            organization.addShelf(QStringLiteral("a1"), QStringLiteral("Interrupted"));
        QVERIFY(!interrupted.ok);

        const PersistenceResult recovery = LibraryPersistence::recoverPendingSaves(library.path());
        QVERIFY2(recovery.ok, qPrintable(recovery.error));
        const LibraryReadResult reopened = LibraryReader::read(library.path());
        QVERIFY2(reopened.ok(), qPrintable(reopened.error));
        QCOMPARE(reopened.library.authors.first().shelves.size(), 2);
        QCOMPARE(reopened.library.authors.first().shelves.last().name,
                 QStringLiteral("Interrupted"));
        QCOMPARE(reopened.library.authors.first().shelves.first().books.size(), 2);
    }
};

QTEST_GUILESS_MAIN(LibraryOrganizationTest)
#include "library_organization_test.moc"
