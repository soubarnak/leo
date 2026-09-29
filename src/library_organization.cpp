#include "library_organization.h"

#include "library_persistence.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>
#include <QUuid>
#include <QVector>

namespace {

QString newId(const QString &prefix)
{
    return prefix + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool validPathComponent(const QString &component)
{
    return !component.isEmpty() && component != QStringLiteral(".") &&
           component != QStringLiteral("..") && !component.contains(QLatin1Char('/')) &&
           !component.contains(QLatin1Char('\\')) && !component.contains(QChar::Null);
}

LibraryOrganizationResult failed(const QString &error, bool conflict = false)
{
    LibraryOrganizationResult result;
    result.conflict = conflict;
    result.error = error;
    return result;
}

QJsonArray authorsOf(const QJsonObject &metadata)
{
    const QJsonValue value = metadata.value(QStringLiteral("authors"));
    return value.isArray() ? value.toArray() : QJsonArray{};
}

QJsonArray shelvesOf(const QJsonObject &metadata)
{
    const QJsonValue value = metadata.value(QStringLiteral("shelves"));
    return value.isArray() ? value.toArray() : QJsonArray{};
}

int objectIndexWithId(const QJsonArray &objects, const QString &id)
{
    for (int index = 0; index < objects.size(); ++index) {
        if (objects.at(index).toObject().value(QStringLiteral("id")).toString() == id) {
            return index;
        }
    }
    return -1;
}

QJsonArray withoutBookId(const QJsonArray &bookIds, const QString &bookId)
{
    QJsonArray kept;
    for (const QJsonValue &value : bookIds) {
        if (value.toString() != bookId) {
            kept.append(value);
        }
    }
    return kept;
}

bool placeBookOnShelf(QJsonArray *shelves, const QString &bookId,
                      const QString &targetShelfId, int index)
{
    const int targetIndex = objectIndexWithId(*shelves, targetShelfId);
    if (targetIndex < 0) {
        return false;
    }

    QJsonArray targetBooks;
    for (int shelfPosition = 0; shelfPosition < shelves->size(); ++shelfPosition) {
        QJsonObject shelf = shelves->at(shelfPosition).toObject();
        QJsonArray kept = withoutBookId(
            shelf.value(QStringLiteral("bookIds")).toArray(), bookId);
        if (shelfPosition == targetIndex) {
            targetBooks = kept;
        }
        shelf.insert(QStringLiteral("bookIds"), kept);
        shelves->replace(shelfPosition, shelf);
    }

    const int insertionIndex = index < 0 ? targetBooks.size()
                                         : qBound(0, index, targetBooks.size());
    targetBooks.insert(insertionIndex, bookId);
    QJsonObject targetShelf = shelves->at(targetIndex).toObject();
    targetShelf.insert(QStringLiteral("bookIds"), targetBooks);
    shelves->replace(targetIndex, targetShelf);
    return true;
}

QString homeAuthorId(const QJsonArray &authors)
{
    return authors.isEmpty()
               ? QString()
               : authors.first().toObject().value(QStringLiteral("id")).toString();
}

QString ownerOfShelf(const QJsonObject &shelf, const QString &homeId)
{
    const QJsonValue authorId = shelf.value(QStringLiteral("authorId"));
    return authorId.isString() && !authorId.toString().isEmpty()
               ? authorId.toString()
               : homeId;
}

QString authorName(const QJsonArray &authors, const QString &authorId)
{
    const int index = objectIndexWithId(authors, authorId);
    if (index < 0) {
        return QString();
    }
    const QString name = authors.at(index).toObject()
                             .value(QStringLiteral("name")).toString().trimmed();
    return name.isEmpty() ? QStringLiteral("Anonymous") : name;
}

bool validateStagingRoot(const QString &libraryPath, bool createIfMissing,
                         QString *error)
{
    const QString stagingRoot =
        QDir(libraryPath).filePath(QStringLiteral(".leo-staging"));
    QFileInfo stagingInfo(stagingRoot);
    if (stagingInfo.isSymLink() || (stagingInfo.exists() && !stagingInfo.isDir())) {
        *error = QStringLiteral("The book staging folder is unsafe; the Library was left unchanged.");
        return false;
    }
    if (!stagingInfo.exists()) {
        if (!createIfMissing) {
            return true;
        }
        if (!QDir().mkpath(stagingRoot)) {
            *error = QStringLiteral("Could not prepare the new book staging folder.");
            return false;
        }
        stagingInfo.refresh();
    }
    if (stagingInfo.isSymLink() || !stagingInfo.isDir()) {
        *error = QStringLiteral("The book staging folder is unsafe; the Library was left unchanged.");
        return false;
    }

    const QString canonicalLibrary = QFileInfo(libraryPath).canonicalFilePath();
    const QString canonicalStaging = stagingInfo.canonicalFilePath();
    if (canonicalLibrary.isEmpty() || canonicalStaging.isEmpty() ||
        QFileInfo(canonicalStaging).path() != canonicalLibrary) {
        *error = QStringLiteral("The book staging folder is outside the Library; the Library was left unchanged.");
        return false;
    }
    return true;
}

void synchronizeLegacyAuthorFields(QJsonObject *metadata)
{
    const QJsonArray authors = authorsOf(*metadata);
    if (authors.isEmpty()) {
        return;
    }

    metadata->insert(QStringLiteral("authorName"),
                     authors.first().toObject().value(QStringLiteral("name")));
    // NEO keeps penNames as first-run legacy data after structured authors exist.

    const QString currentId = metadata->value(QStringLiteral("currentAuthorId")).toString();
    if (objectIndexWithId(authors, currentId) < 0) {
        metadata->insert(QStringLiteral("currentAuthorId"),
                         authors.first().toObject().value(QStringLiteral("id")));
    }
}

bool prepareOrganizationMetadata(QJsonObject *metadata, QString *error)
{
    const QJsonValue authorsValue = metadata->value(QStringLiteral("authors"));
    const QJsonValue shelvesValue = metadata->value(QStringLiteral("shelves"));
    const QJsonValue authorNameValue = metadata->value(QStringLiteral("authorName"));
    const QJsonValue penNamesValue = metadata->value(QStringLiteral("penNames"));
    const QJsonValue currentAuthorValue = metadata->value(QStringLiteral("currentAuthorId"));
    if ((!authorsValue.isUndefined() && !authorsValue.isArray()) ||
        !shelvesValue.isArray() ||
        (!authorNameValue.isUndefined() && !authorNameValue.isString()) ||
        (!penNamesValue.isUndefined() && !penNamesValue.isArray()) ||
        (!currentAuthorValue.isUndefined() && !currentAuthorValue.isString())) {
        *error = QStringLiteral("Library author or shelf metadata has an invalid type.");
        return false;
    }
    for (const QJsonValue &penName : penNamesValue.toArray()) {
        if (!penName.isString()) {
            *error = QStringLiteral("Library pen name metadata is malformed.");
            return false;
        }
    }

    QJsonArray authors = authorsOf(*metadata);
    if (authors.isEmpty()) {
        QString legacyName = metadata->value(QStringLiteral("authorName")).toString().trimmed();
        const QJsonValue penNamesValue = metadata->value(QStringLiteral("penNames"));
        if (legacyName.isEmpty() && penNamesValue.isArray() && !penNamesValue.toArray().isEmpty()) {
            legacyName = penNamesValue.toArray().first().toString().trimmed();
        }
        if (legacyName.isEmpty()) {
            legacyName = QStringLiteral("Anonymous");
        }
        authors.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("a1")},
                                   {QStringLiteral("name"), legacyName}});
        metadata->insert(QStringLiteral("authors"), authors);
    }

    QSet<QString> authorIds;
    for (const QJsonValue &value : authors) {
        if (!value.isObject()) {
            *error = QStringLiteral("Library author metadata is malformed.");
            return false;
        }
        const QJsonObject author = value.toObject();
        const QString id = author.value(QStringLiteral("id")).toString();
        if (id.isEmpty() || authorIds.contains(id) ||
            !author.value(QStringLiteral("name")).isString()) {
            *error = QStringLiteral("Library author metadata contains a missing or duplicate ID.");
            return false;
        }
        authorIds.insert(id);
    }

    const QString homeId = homeAuthorId(authors);
    QJsonArray shelves = shelvesOf(*metadata);
    QSet<QString> shelfIds;
    for (int index = 0; index < shelves.size(); ++index) {
        if (!shelves.at(index).isObject()) {
            *error = QStringLiteral("Library shelf metadata is malformed.");
            return false;
        }
        QJsonObject shelf = shelves.at(index).toObject();
        const QString shelfId = shelf.value(QStringLiteral("id")).toString();
        if (!shelf.value(QStringLiteral("id")).isString() || shelfId.isEmpty() ||
            shelfIds.contains(shelfId) ||
            !shelf.value(QStringLiteral("name")).isString() ||
            !shelf.value(QStringLiteral("bookIds")).isArray()) {
            *error = QStringLiteral("Library shelf metadata is incomplete or ambiguous.");
            return false;
        }
        shelfIds.insert(shelfId);
        for (const QJsonValue &bookId : shelf.value(QStringLiteral("bookIds")).toArray()) {
            if (!bookId.isString() || !validPathComponent(bookId.toString())) {
                *error = QStringLiteral("Library shelf book membership is malformed.");
                return false;
            }
        }
        if (shelf.value(QStringLiteral("authorId")).isUndefined()) {
            shelf.insert(QStringLiteral("authorId"), homeId);
            shelves.replace(index, shelf);
        } else if (!shelf.value(QStringLiteral("authorId")).isString() ||
                   shelf.value(QStringLiteral("authorId")).toString().isEmpty()) {
            *error = QStringLiteral("Library shelf author metadata is malformed.");
            return false;
        }
        const QString owner = ownerOfShelf(shelf, homeId);
        if (!authorIds.contains(owner)) {
            *error = QStringLiteral("A shelf refers to an unknown pen name.");
            return false;
        }
    }
    metadata->insert(QStringLiteral("shelves"), shelves);
    synchronizeLegacyAuthorFields(metadata);
    return true;
}

bool writeNewBookFile(const QString &directory, const QString &relativePath,
                      const QByteArray &bytes, QString *error)
{
    const QString path = QDir(directory).filePath(relativePath);
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        *error = QStringLiteral("Could not create %1.").arg(QFileInfo(path).absolutePath());
        return false;
    }

    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        *error = QStringLiteral("Could not create %1: %2").arg(path, file.errorString());
        return false;
    }
    if (file.write(bytes) != bytes.size()) {
        *error = QStringLiteral("Could not write %1: %2").arg(path, file.errorString());
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        *error = QStringLiteral("Could not finish %1: %2").arg(path, file.errorString());
        return false;
    }
    return true;
}

}

LibraryOrganization::LibraryOrganization(const QString &libraryPath,
                                         const PersistenceCheckpointHook &checkpoint,
                                         const PersistenceIoFailureHook &ioFailure)
    : libraryPath_(QDir::cleanPath(QFileInfo(libraryPath).absoluteFilePath())),
      checkpoint_(checkpoint), ioFailure_(ioFailure)
{
}

bool LibraryOrganization::load(QString *error)
{
    if (error) {
        error->clear();
    }
    loaded_ = false;
    metadata_ = QJsonObject{};
    metadataHash_.clear();
    QString readError;
    QByteArray bytes;
    if (!LibraryPersistence::readLibraryFile(libraryPath_, QStringLiteral("library.json"),
                                             &bytes, &readError)) {
        if (error) {
            *error = readError;
        }
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) {
            *error = QStringLiteral("Library metadata is not a valid JSON object.");
        }
        return false;
    }
    metadata_ = document.object();
    metadataHash_ = LibraryPersistence::hash(bytes);
    loaded_ = true;
    QJsonObject validatedMetadata = metadata_;
    QString validationError;
    if (!prepareOrganizationMetadata(&validatedMetadata, &validationError)) {
        loaded_ = false;
        if (error) {
            *error = validationError;
        }
        return false;
    }
    metadata_ = validatedMetadata;

    int pendingOperationCount = 0;
    for (const QString &key : {QStringLiteral("leoPendingTrash"),
                               QStringLiteral("leoPendingCreate"),
                               QStringLiteral("leoPendingMove")}) {
        pendingOperationCount += metadata_.contains(key) ? 1 : 0;
    }
    if (pendingOperationCount > 1) {
        loaded_ = false;
        if (error) {
            *error = QStringLiteral("The Library contains conflicting interrupted organization records.");
        }
        return false;
    }

    const QJsonValue pendingTrashValue = metadata_.value(QStringLiteral("leoPendingTrash"));
    if (!pendingTrashValue.isUndefined()) {
        if (!pendingTrashValue.isObject()) {
            loaded_ = false;
            if (error) {
                *error = QStringLiteral("The interrupted Trash operation record is malformed.");
            }
            return false;
        }

        const QJsonObject pendingTrash = pendingTrashValue.toObject();
        const QString bookId = pendingTrash.value(QStringLiteral("bookId")).toString();
        const QJsonValue membershipsValue = pendingTrash.value(QStringLiteral("memberships"));
        if (!validPathComponent(bookId) || !bookId.startsWith(QStringLiteral("book-")) ||
            !membershipsValue.isArray()) {
            loaded_ = false;
            if (error) {
                *error = QStringLiteral("The interrupted Trash operation record is incomplete.");
            }
            return false;
        }

        const QJsonArray memberships = membershipsValue.toArray();
        for (const QJsonValue &membershipValue : memberships) {
            if (!membershipValue.isObject()) {
                loaded_ = false;
                if (error) {
                    *error = QStringLiteral("The interrupted Trash operation has invalid shelf membership data.");
                }
                return false;
            }
            const QJsonObject membership = membershipValue.toObject();
            if (membership.value(QStringLiteral("shelfId")).toString().isEmpty() ||
                !membership.value(QStringLiteral("index")).isDouble() ||
                membership.value(QStringLiteral("index")).toInt(-1) < 0) {
                loaded_ = false;
                if (error) {
                    *error = QStringLiteral("The interrupted Trash operation has invalid shelf membership data.");
                }
                return false;
            }
        }

        const QFileInfo bookInfo(QDir(libraryPath_).filePath(bookId));
        if (bookInfo.isSymLink() || (bookInfo.exists() && !bookInfo.isDir())) {
            loaded_ = false;
            if (error) {
                *error = QStringLiteral("The interrupted Trash operation's book folder is unsafe; the Library was left unchanged.");
            }
            return false;
        }

        QJsonObject recovered = metadata_;
        if (bookInfo.isDir()) {
            QJsonArray shelves = shelvesOf(recovered);
            for (const QJsonValue &membershipValue : memberships) {
                const QJsonObject membership = membershipValue.toObject();
                const QString shelfId = membership.value(QStringLiteral("shelfId")).toString();
                const int shelfPosition = objectIndexWithId(shelves, shelfId);
                if (shelfPosition < 0) {
                    loaded_ = false;
                    if (error) {
                        *error = QStringLiteral("The interrupted Trash operation refers to a missing shelf; the Library was left unchanged.");
                    }
                    return false;
                }
                QJsonObject shelf = shelves.at(shelfPosition).toObject();
                QJsonArray bookIds = shelf.value(QStringLiteral("bookIds")).toArray();
                if (!bookIds.contains(bookId)) {
                    bookIds.insert(qBound(0, membership.value(QStringLiteral("index")).toInt(),
                                          bookIds.size()), bookId);
                    shelf.insert(QStringLiteral("bookIds"), bookIds);
                    shelves.replace(shelfPosition, shelf);
                }
            }
            recovered.insert(QStringLiteral("shelves"), shelves);
        } else {
            QJsonArray shelves = shelvesOf(recovered);
            for (int shelfPosition = 0; shelfPosition < shelves.size(); ++shelfPosition) {
                QJsonObject shelf = shelves.at(shelfPosition).toObject();
                shelf.insert(QStringLiteral("bookIds"), withoutBookId(
                    shelf.value(QStringLiteral("bookIds")).toArray(), bookId));
                shelves.replace(shelfPosition, shelf);
            }
            recovered.insert(QStringLiteral("shelves"), shelves);
        }
        recovered.remove(QStringLiteral("leoPendingTrash"));
        const LibraryOrganizationResult recovery = commitMetadata(recovered);
        if (!recovery.ok) {
            loaded_ = false;
            if (error) {
                *error = QStringLiteral("LEO could not finish recovering an interrupted Trash operation: %1")
                             .arg(recovery.error);
            }
            return false;
        }
    }

    const QJsonValue pendingMoveValue = metadata_.value(QStringLiteral("leoPendingMove"));
    if (!pendingMoveValue.isUndefined()) {
        if (!pendingMoveValue.isObject()) {
            loaded_ = false;
            if (error) {
                *error = QStringLiteral("The interrupted book move record is malformed.");
            }
            return false;
        }

        const QJsonObject pendingMove = pendingMoveValue.toObject();
        const QString bookId = pendingMove.value(QStringLiteral("bookId")).toString();
        const QString shelfId = pendingMove.value(QStringLiteral("shelfId")).toString();
        const QJsonValue previousAuthorValue =
            pendingMove.value(QStringLiteral("previousAuthor"));
        const QJsonValue targetAuthorValue = pendingMove.value(QStringLiteral("authorName"));
        const QString previousAuthor = previousAuthorValue.toString();
        const QString targetAuthor = targetAuthorValue.toString();
        const QJsonValue indexValue = pendingMove.value(QStringLiteral("index"));
        if (!validPathComponent(bookId) || !bookId.startsWith(QStringLiteral("book-")) ||
            !validPathComponent(shelfId) || !previousAuthorValue.isString() ||
            !targetAuthorValue.isString() || targetAuthor.trimmed().isEmpty() ||
            !indexValue.isDouble() || indexValue.toInt(-1) < 0 ||
            indexValue.toDouble() != indexValue.toInt(-1)) {
            loaded_ = false;
            if (error) {
                *error = QStringLiteral("The interrupted book move record is incomplete.");
            }
            return false;
        }

        const QJsonArray shelves = shelvesOf(metadata_);
        const int targetShelfIndex = objectIndexWithId(shelves, shelfId);
        if (targetShelfIndex < 0) {
            loaded_ = false;
            if (error) {
                *error = QStringLiteral("The interrupted book move refers to a missing shelf; the Library was left unchanged.");
            }
            return false;
        }
        const QJsonArray authors = authorsOf(metadata_);
        const QString shelfOwner = ownerOfShelf(shelves.at(targetShelfIndex).toObject(),
                                                homeAuthorId(authors));
        if (authorName(authors, shelfOwner) != targetAuthor) {
            loaded_ = false;
            if (error) {
                *error = QStringLiteral("The interrupted book move destination changed; the Library was left unchanged.");
            }
            return false;
        }

        const QString bookRelativePath = bookId + QStringLiteral("/book.json");
        QByteArray bookBytes;
        if (!LibraryPersistence::readLibraryFile(libraryPath_, bookRelativePath,
                                                 &bookBytes, &readError)) {
            loaded_ = false;
            if (error) {
                *error = QStringLiteral("LEO could not recover the interrupted book move: %1")
                             .arg(readError);
            }
            return false;
        }
        QJsonParseError bookParseError;
        const QJsonDocument bookDocument = QJsonDocument::fromJson(bookBytes, &bookParseError);
        if (bookParseError.error != QJsonParseError::NoError || !bookDocument.isObject()) {
            loaded_ = false;
            if (error) {
                *error = QStringLiteral("The interrupted book move has invalid book metadata; the Library was left unchanged.");
            }
            return false;
        }
        const QJsonObject bookMetadata = bookDocument.object();
        const QString currentAuthor = bookMetadata.value(QStringLiteral("author")).toString();
        const QString storedBookId = bookMetadata.value(QStringLiteral("id")).toString();
        if ((!storedBookId.isEmpty() && storedBookId != bookId) ||
            (currentAuthor != targetAuthor && currentAuthor != previousAuthor)) {
            loaded_ = false;
            if (error) {
                *error = QStringLiteral("The interrupted book move conflicts with saved book metadata; the Library was left unchanged.");
            }
            return false;
        }

        QJsonObject recovered = metadata_;
        if (currentAuthor == targetAuthor) {
            QJsonArray recoveredShelves = shelvesOf(recovered);
            if (!placeBookOnShelf(&recoveredShelves, bookId, shelfId,
                                  indexValue.toInt())) {
                loaded_ = false;
                if (error) {
                    *error = QStringLiteral("LEO could not restore the interrupted book's shelf membership.");
                }
                return false;
            }
            recovered.insert(QStringLiteral("shelves"), recoveredShelves);
        }
        recovered.remove(QStringLiteral("leoPendingMove"));
        const LibraryOrganizationResult recovery = commitMetadata(recovered);
        if (!recovery.ok) {
            loaded_ = false;
            if (error) {
                *error = QStringLiteral("LEO could not finish recovering an interrupted book move: %1")
                             .arg(recovery.error);
            }
            return false;
        }
    }

    const QJsonValue pendingCreateValue = metadata_.value(QStringLiteral("leoPendingCreate"));
    if (pendingCreateValue.isUndefined()) {
        return true;
    }
    if (!pendingCreateValue.isObject()) {
        loaded_ = false;
        if (error) {
            *error = QStringLiteral("The interrupted book creation record is malformed.");
        }
        return false;
    }

    const QJsonObject pendingCreate = pendingCreateValue.toObject();
    const QString bookId = pendingCreate.value(QStringLiteral("bookId")).toString();
    const QString shelfId = pendingCreate.value(QStringLiteral("shelfId")).toString();
    const QString stagingRelativePath =
        pendingCreate.value(QStringLiteral("stagingPath")).toString();
    const QJsonValue indexValue = pendingCreate.value(QStringLiteral("index"));
    const QString stagingPrefix = QStringLiteral(".leo-staging/");
    const QString stagingName = stagingRelativePath.mid(stagingPrefix.size());
    if (!validPathComponent(bookId) || !bookId.startsWith(QStringLiteral("book-")) ||
        !validPathComponent(shelfId) ||
        !stagingRelativePath.startsWith(stagingPrefix) || !validPathComponent(stagingName) ||
        !indexValue.isDouble() || indexValue.toInt(-1) < 0) {
        loaded_ = false;
        if (error) {
            *error = QStringLiteral("The interrupted book creation record is incomplete.");
        }
        return false;
    }

    QString stagingError;
    if (!validateStagingRoot(libraryPath_, false, &stagingError)) {
        loaded_ = false;
        if (error) {
            *error = stagingError;
        }
        return false;
    }

    const QString bookPath = QDir(libraryPath_).filePath(bookId);
    const QString stagingPath = QDir(libraryPath_).filePath(stagingRelativePath);
    const QFileInfo bookInfo(bookPath);
    const QFileInfo stagingInfo(stagingPath);
    if (bookInfo.isSymLink() || (bookInfo.exists() && !bookInfo.isDir()) ||
        stagingInfo.isSymLink() || (stagingInfo.exists() && !stagingInfo.isDir()) ||
        (bookInfo.isDir() && stagingInfo.isDir())) {
        loaded_ = false;
        if (error) {
            *error = QStringLiteral("The interrupted book creation folders are unsafe or conflicting; the Library was left unchanged.");
        }
        return false;
    }
    if ((bookInfo.isDir() || stagingInfo.isDir()) &&
        objectIndexWithId(shelvesOf(metadata_), shelfId) < 0) {
        loaded_ = false;
        if (error) {
            *error = QStringLiteral("The interrupted new book refers to a missing shelf; its folder was retained for inspection.");
        }
        return false;
    }

    bool bookFolderReady = bookInfo.isDir();
    if (!bookFolderReady && stagingInfo.isDir()) {
        QFile stagedMetadataFile(QDir(stagingPath).filePath(QStringLiteral("book.json")));
        if (!stagedMetadataFile.open(QIODevice::ReadOnly)) {
            loaded_ = false;
            if (error) {
                *error = QStringLiteral("The interrupted new book has no readable metadata; the Library was left unchanged.");
            }
            return false;
        }
        QJsonParseError stagedParseError;
        const QJsonDocument stagedDocument =
            QJsonDocument::fromJson(stagedMetadataFile.readAll(), &stagedParseError);
        stagedMetadataFile.close();
        if (stagedParseError.error != QJsonParseError::NoError ||
            !stagedDocument.isObject() ||
            stagedDocument.object().value(QStringLiteral("id")).toString() != bookId) {
            loaded_ = false;
            if (error) {
                *error = QStringLiteral("The interrupted new book metadata is invalid; the Library was left unchanged.");
            }
            return false;
        }
        if (!QDir(libraryPath_).rename(stagingRelativePath, bookId)) {
            loaded_ = false;
            if (error) {
                *error = QStringLiteral("LEO could not finish moving the interrupted new book into the Library.");
            }
            return false;
        }
        bookFolderReady = true;
    }

    QJsonObject recovered = metadata_;
    if (bookFolderReady) {
        QJsonArray shelves = shelvesOf(recovered);
        const int targetShelfPosition = objectIndexWithId(shelves, shelfId);
        if (targetShelfPosition < 0) {
            loaded_ = false;
            if (error) {
                *error = QStringLiteral("The interrupted new book refers to a missing shelf; the Library was left unchanged.");
            }
            return false;
        }
        for (int shelfPosition = 0; shelfPosition < shelves.size(); ++shelfPosition) {
            QJsonObject shelf = shelves.at(shelfPosition).toObject();
            QJsonArray bookIds = withoutBookId(
                shelf.value(QStringLiteral("bookIds")).toArray(), bookId);
            if (shelfPosition == targetShelfPosition) {
                bookIds.insert(qBound(0, indexValue.toInt(), bookIds.size()), bookId);
            }
            shelf.insert(QStringLiteral("bookIds"), bookIds);
            shelves.replace(shelfPosition, shelf);
        }
        recovered.insert(QStringLiteral("shelves"), shelves);
        if (recovered.value(QStringLiteral("initialBookId")).toString().isEmpty()) {
            recovered.insert(QStringLiteral("initialBookId"), bookId);
        }
    }
    recovered.remove(QStringLiteral("leoPendingCreate"));
    const LibraryOrganizationResult creationRecovery = commitMetadata(recovered);
    if (!creationRecovery.ok) {
        loaded_ = false;
        if (error) {
            *error = QStringLiteral("LEO could not finish recovering an interrupted book creation: %1")
                         .arg(creationRecovery.error);
        }
        return false;
    }
    return true;
}

const QJsonObject &LibraryOrganization::metadata() const
{
    return metadata_;
}

LibraryOrganizationResult LibraryOrganization::commitMetadata(const QJsonObject &updated)
{
    if (!loaded_) {
        return failed(QStringLiteral("Open a Library before changing its organization."));
    }

    const QByteArray bytes = QJsonDocument(updated).toJson(QJsonDocument::Indented);
    const PersistenceResult saved = LibraryPersistence::saveFile(
        libraryPath_, QStringLiteral("library.json"), metadataHash_, bytes,
        checkpoint_, ioFailure_);
    if (!saved.ok) {
        return failed(saved.error, saved.conflict);
    }

    metadata_ = updated;
    metadataHash_ = saved.savedHash;
    LibraryOrganizationResult result;
    result.ok = true;
    return result;
}

LibraryOrganizationResult LibraryOrganization::commitBookMetadata(
    const QString &relativePath, const QByteArray &oldBytes, const QByteArray &newBytes)
{
    if (!loaded_) {
        return failed(QStringLiteral("Open a Library before changing book metadata."));
    }
    const PersistenceResult saved = LibraryPersistence::saveFile(
        libraryPath_, relativePath, LibraryPersistence::hash(oldBytes), newBytes,
        checkpoint_, ioFailure_);
    if (!saved.ok) {
        return failed(saved.error, saved.conflict);
    }
    LibraryOrganizationResult result;
    result.ok = true;
    return result;
}

LibraryOrganizationResult LibraryOrganization::addAuthor(const QString &name)
{
    const QString normalizedName = name.trimmed();
    if (normalizedName.isEmpty()) {
        return failed(QStringLiteral("Enter a name for the pen name."));
    }
    QJsonObject updated = metadata_;
    QString error;
    if (!prepareOrganizationMetadata(&updated, &error)) {
        return failed(error);
    }

    QJsonArray authors = authorsOf(updated);
    QJsonArray shelves = shelvesOf(updated);
    const QString authorId = newId(QStringLiteral("author-"));
    const QString shelfId = newId(QStringLiteral("shelf-"));
    authors.append(QJsonObject{{QStringLiteral("id"), authorId},
                               {QStringLiteral("name"), normalizedName}});
    shelves.append(QJsonObject{{QStringLiteral("id"), shelfId},
                               {QStringLiteral("name"), QStringLiteral("Works in Progress")},
                               {QStringLiteral("authorId"), authorId},
                               {QStringLiteral("bookIds"), QJsonArray{}}});
    updated.insert(QStringLiteral("authors"), authors);
    updated.insert(QStringLiteral("shelves"), shelves);
    updated.insert(QStringLiteral("currentAuthorId"), authorId);
    synchronizeLegacyAuthorFields(&updated);

    LibraryOrganizationResult result = commitMetadata(updated);
    if (result.ok) {
        result.id = authorId;
    }
    return result;
}

LibraryOrganizationResult LibraryOrganization::renameAuthor(const QString &authorId,
                                                              const QString &name)
{
    const QString normalizedName = name.trimmed();
    if (normalizedName.isEmpty()) {
        return failed(QStringLiteral("Enter a name for the pen name."));
    }
    QJsonObject updated = metadata_;
    QString error;
    if (!prepareOrganizationMetadata(&updated, &error)) {
        return failed(error);
    }
    QJsonArray authors = authorsOf(updated);
    const int index = objectIndexWithId(authors, authorId);
    if (index < 0) {
        return failed(QStringLiteral("The selected pen name no longer exists."));
    }
    QJsonObject author = authors.at(index).toObject();
    author.insert(QStringLiteral("name"), normalizedName);
    authors.replace(index, author);
    updated.insert(QStringLiteral("authors"), authors);
    synchronizeLegacyAuthorFields(&updated);
    return commitMetadata(updated);
}

LibraryOrganizationResult LibraryOrganization::removeAuthor(const QString &authorId,
                                                              const QString &targetAuthorId)
{
    QJsonObject updated = metadata_;
    QString error;
    if (!prepareOrganizationMetadata(&updated, &error)) {
        return failed(error);
    }
    QJsonArray authors = authorsOf(updated);
    const int index = objectIndexWithId(authors, authorId);
    if (index < 0) {
        return failed(QStringLiteral("The selected pen name no longer exists."));
    }
    if (authors.size() < 2) {
        return failed(QStringLiteral("A Library must keep at least one pen name."));
    }
    if (targetAuthorId == authorId || objectIndexWithId(authors, targetAuthorId) < 0) {
        return failed(QStringLiteral("Choose another pen name to keep these books."));
    }

    const QString currentId = updated.value(QStringLiteral("currentAuthorId")).toString();
    QJsonArray shelves = shelvesOf(updated);
    for (int shelfPosition = 0; shelfPosition < shelves.size(); ++shelfPosition) {
        QJsonObject shelf = shelves.at(shelfPosition).toObject();
        if (ownerOfShelf(shelf, homeAuthorId(authors)) == authorId) {
            shelf.insert(QStringLiteral("authorId"), targetAuthorId);
            shelves.replace(shelfPosition, shelf);
        }
    }
    authors.removeAt(index);
    updated.insert(QStringLiteral("authors"), authors);
    updated.insert(QStringLiteral("shelves"), shelves);
    if (currentId == authorId) {
        updated.insert(QStringLiteral("currentAuthorId"), targetAuthorId);
    }
    synchronizeLegacyAuthorFields(&updated);
    return commitMetadata(updated);
}

LibraryOrganizationResult LibraryOrganization::setCurrentAuthor(const QString &authorId)
{
    QJsonObject updated = metadata_;
    QString error;
    if (!prepareOrganizationMetadata(&updated, &error)) {
        return failed(error);
    }
    if (objectIndexWithId(authorsOf(updated), authorId) < 0) {
        return failed(QStringLiteral("The selected pen name no longer exists."));
    }
    updated.insert(QStringLiteral("currentAuthorId"), authorId);
    synchronizeLegacyAuthorFields(&updated);
    return commitMetadata(updated);
}

LibraryOrganizationResult LibraryOrganization::addShelf(const QString &authorId,
                                                          const QString &name)
{
    const QString normalizedName = name.trimmed();
    if (normalizedName.isEmpty()) {
        return failed(QStringLiteral("Enter a name for the shelf."));
    }
    QJsonObject updated = metadata_;
    QString error;
    if (!prepareOrganizationMetadata(&updated, &error)) {
        return failed(error);
    }
    const QJsonArray authors = authorsOf(updated);
    if (objectIndexWithId(authors, authorId) < 0) {
        return failed(QStringLiteral("Choose a pen name before adding a shelf."));
    }

    const QString id = newId(QStringLiteral("shelf-"));
    QJsonArray shelves = shelvesOf(updated);
    shelves.append(QJsonObject{{QStringLiteral("id"), id},
                               {QStringLiteral("name"), normalizedName},
                               {QStringLiteral("authorId"), authorId},
                               {QStringLiteral("bookIds"), QJsonArray{}}});
    updated.insert(QStringLiteral("shelves"), shelves);
    LibraryOrganizationResult result = commitMetadata(updated);
    if (result.ok) {
        result.id = id;
    }
    return result;
}

LibraryOrganizationResult LibraryOrganization::renameShelf(const QString &shelfId,
                                                             const QString &name)
{
    const QString normalizedName = name.trimmed();
    if (normalizedName.isEmpty()) {
        return failed(QStringLiteral("Enter a name for the shelf."));
    }
    QJsonObject updated = metadata_;
    QString error;
    if (!prepareOrganizationMetadata(&updated, &error)) {
        return failed(error);
    }
    QJsonArray shelves = shelvesOf(updated);
    const int index = objectIndexWithId(shelves, shelfId);
    if (index < 0) {
        return failed(QStringLiteral("The selected shelf no longer exists."));
    }
    QJsonObject shelf = shelves.at(index).toObject();
    shelf.insert(QStringLiteral("name"), normalizedName);
    shelves.replace(index, shelf);
    updated.insert(QStringLiteral("shelves"), shelves);
    return commitMetadata(updated);
}

LibraryOrganizationResult LibraryOrganization::removeShelf(const QString &shelfId)
{
    QJsonObject updated = metadata_;
    QString error;
    if (!prepareOrganizationMetadata(&updated, &error)) {
        return failed(error);
    }
    const QJsonArray authors = authorsOf(updated);
    QJsonArray shelves = shelvesOf(updated);
    const int index = objectIndexWithId(shelves, shelfId);
    if (index < 0) {
        return failed(QStringLiteral("The selected shelf no longer exists."));
    }

    const QString homeId = homeAuthorId(authors);
    const QJsonObject removed = shelves.at(index).toObject();
    const QString owner = ownerOfShelf(removed, homeId);
    int replacementIndex = -1;
    for (int shelfPosition = 0; shelfPosition < shelves.size(); ++shelfPosition) {
        if (shelfPosition != index &&
            ownerOfShelf(shelves.at(shelfPosition).toObject(), homeId) == owner) {
            replacementIndex = shelfPosition;
            break;
        }
    }
    if (replacementIndex < 0) {
        return failed(QStringLiteral("Add another shelf for this pen name before deleting this one."));
    }

    QJsonObject replacement = shelves.at(replacementIndex).toObject();
    QJsonArray replacementBooks = replacement.value(QStringLiteral("bookIds")).toArray();
    const QJsonArray removedBooks = removed.value(QStringLiteral("bookIds")).toArray();
    for (const QJsonValue &bookId : removedBooks) {
        if (!replacementBooks.contains(bookId)) {
            replacementBooks.append(bookId);
        }
    }
    replacement.insert(QStringLiteral("bookIds"), replacementBooks);
    shelves.replace(replacementIndex, replacement);
    shelves.removeAt(index);
    updated.insert(QStringLiteral("shelves"), shelves);
    return commitMetadata(updated);
}

LibraryOrganizationResult LibraryOrganization::moveShelf(const QString &shelfId,
                                                           const QString &targetAuthorId,
                                                           int index)
{
    QJsonObject updated = metadata_;
    QString error;
    if (!prepareOrganizationMetadata(&updated, &error)) {
        return failed(error);
    }
    const QJsonArray authors = authorsOf(updated);
    if (objectIndexWithId(authors, targetAuthorId) < 0) {
        return failed(QStringLiteral("Choose a valid pen name for this shelf."));
    }
    QJsonArray shelves = shelvesOf(updated);
    const int movingIndex = objectIndexWithId(shelves, shelfId);
    if (movingIndex < 0) {
        return failed(QStringLiteral("The selected shelf no longer exists."));
    }
    QJsonObject moving = shelves.at(movingIndex).toObject();
    const QString homeId = homeAuthorId(authors);
    if (ownerOfShelf(moving, homeId) != targetAuthorId) {
        return failed(QStringLiteral("Move books between pen names to reassign their author."));
    }
    shelves.removeAt(movingIndex);

    QVector<int> targetPositions;
    for (int position = 0; position < shelves.size(); ++position) {
        if (ownerOfShelf(shelves.at(position).toObject(), homeId) == targetAuthorId) {
            targetPositions.append(position);
        }
    }
    const int insertionIndex = index < 0
                                   ? shelves.size()
                                   : index < targetPositions.size()
                                         ? targetPositions.at(index)
                                         : targetPositions.isEmpty()
                                               ? shelves.size()
                                               : targetPositions.last() + 1;
    shelves.insert(insertionIndex, moving);
    updated.insert(QStringLiteral("shelves"), shelves);
    return commitMetadata(updated);
}

LibraryOrganizationResult LibraryOrganization::moveBook(const QString &bookId,
                                                          const QString &targetShelfId,
                                                          int index)
{
    if (!loaded_) {
        return failed(QStringLiteral("Open a Library before moving a book."));
    }
    if (!validPathComponent(bookId) || !bookId.startsWith(QStringLiteral("book-"))) {
        return failed(QStringLiteral("Choose a book to move."));
    }
    if (metadata_.contains(QStringLiteral("leoPendingTrash")) ||
        metadata_.contains(QStringLiteral("leoPendingCreate")) ||
        metadata_.contains(QStringLiteral("leoPendingMove"))) {
        return failed(QStringLiteral("An earlier Library organization change needs recovery before moving a book."));
    }
    QJsonObject updated = metadata_;
    QString error;
    if (!prepareOrganizationMetadata(&updated, &error)) {
        return failed(error);
    }
    QJsonArray shelves = shelvesOf(updated);
    const int targetIndex = objectIndexWithId(shelves, targetShelfId);
    if (targetIndex < 0) {
        return failed(QStringLiteral("Choose a valid shelf for this book."));
    }

    const QJsonArray authors = authorsOf(updated);
    const QString targetAuthorId = ownerOfShelf(shelves.at(targetIndex).toObject(),
                                                homeAuthorId(authors));
    const QString targetAuthorName = authorName(authors, targetAuthorId);
    if (targetAuthorName.isEmpty()) {
        return failed(QStringLiteral("The selected shelf has no valid pen name."));
    }

    QJsonArray targetBooks = shelves.at(targetIndex).toObject()
                                 .value(QStringLiteral("bookIds")).toArray();
    targetBooks = withoutBookId(targetBooks, bookId);
    const int insertionIndex = index < 0 ? targetBooks.size()
                                         : qBound(0, index, targetBooks.size());

    const QString bookPath = bookId + QStringLiteral("/book.json");
    QByteArray oldBookBytes;
    if (!LibraryPersistence::readLibraryFile(libraryPath_, bookPath,
                                             &oldBookBytes, &error)) {
        return failed(error);
    }
    QJsonParseError parseError;
    const QJsonDocument bookDocument = QJsonDocument::fromJson(oldBookBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !bookDocument.isObject()) {
        return failed(QStringLiteral("Book metadata is not a valid JSON object."));
    }
    QJsonObject bookMetadata = bookDocument.object();
    const QString storedId = bookMetadata.value(QStringLiteral("id")).toString();
    if (!storedId.isEmpty() && storedId != bookId) {
        return failed(QStringLiteral("Book metadata does not match its Library folder."));
    }

    if (bookMetadata.value(QStringLiteral("author")).toString() != targetAuthorName) {
        bookMetadata.insert(QStringLiteral("author"), targetAuthorName);
        bookMetadata.insert(QStringLiteral("modified"),
                            QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        const QByteArray newBookBytes =
            QJsonDocument(bookMetadata).toJson(QJsonDocument::Indented);
        QJsonObject pending = updated;
        pending.insert(QStringLiteral("leoPendingMove"), QJsonObject{
            {QStringLiteral("bookId"), bookId},
            {QStringLiteral("shelfId"), targetShelfId},
            {QStringLiteral("index"), insertionIndex},
            {QStringLiteral("previousAuthor"),
             bookDocument.object().value(QStringLiteral("author")).toString()},
            {QStringLiteral("authorName"), targetAuthorName}});
        const LibraryOrganizationResult prepared = commitMetadata(pending);
        if (!prepared.ok) {
            return prepared;
        }

        const LibraryOrganizationResult bookSaved =
            commitBookMetadata(bookPath, oldBookBytes, newBookBytes);
        if (!bookSaved.ok) {
            return failed(QStringLiteral(
                "The book's shelf was left unchanged. LEO will recover the author update when "
                "the Library is reopened. %1")
                              .arg(bookSaved.error),
                          bookSaved.conflict);
        }

        QJsonObject completed = metadata_;
        QJsonArray completedShelves = shelvesOf(completed);
        if (!placeBookOnShelf(&completedShelves, bookId, targetShelfId, insertionIndex)) {
            return failed(QStringLiteral(
                "The author was updated, but the destination shelf is missing. Reopen the "
                "Library to recover or inspect the change."));
        }
        completed.insert(QStringLiteral("shelves"), completedShelves);
        completed.remove(QStringLiteral("leoPendingMove"));
        const LibraryOrganizationResult finished = commitMetadata(completed);
        if (!finished.ok) {
            return failed(QStringLiteral(
                "The author was updated. LEO will finish moving the book when the Library is "
                "reopened. %1")
                              .arg(finished.error),
                          finished.conflict);
        }
        return finished;
    }

    if (!placeBookOnShelf(&shelves, bookId, targetShelfId, insertionIndex)) {
        return failed(QStringLiteral("Choose a valid shelf for this book."));
    }
    updated.insert(QStringLiteral("shelves"), shelves);
    return commitMetadata(updated);
}

LibraryOrganizationResult LibraryOrganization::removeBookFromShelves(const QString &bookId)
{
    QJsonObject updated = metadata_;
    QString error;
    if (!prepareOrganizationMetadata(&updated, &error)) {
        return failed(error);
    }
    QJsonArray shelves = shelvesOf(updated);
    for (int shelfPosition = 0; shelfPosition < shelves.size(); ++shelfPosition) {
        QJsonObject shelf = shelves.at(shelfPosition).toObject();
        const QJsonArray kept = withoutBookId(
            shelf.value(QStringLiteral("bookIds")).toArray(), bookId);
        shelf.insert(QStringLiteral("bookIds"), kept);
        shelves.replace(shelfPosition, shelf);
    }
    updated.insert(QStringLiteral("shelves"), shelves);
    return commitMetadata(updated);
}

LibraryOrganizationResult LibraryOrganization::createBook(const QString &shelfId,
                                                            const QString &title)
{
    if (!loaded_) {
        return failed(QStringLiteral("Open a Library before creating a book."));
    }
    if (metadata_.contains(QStringLiteral("leoPendingTrash")) ||
        metadata_.contains(QStringLiteral("leoPendingCreate")) ||
        metadata_.contains(QStringLiteral("leoPendingMove"))) {
        return failed(QStringLiteral("An earlier Library organization change needs recovery before creating a book."));
    }
    QJsonObject updated = metadata_;
    QString error;
    if (!prepareOrganizationMetadata(&updated, &error)) {
        return failed(error);
    }
    QJsonArray authors = authorsOf(updated);
    QJsonArray shelves = shelvesOf(updated);
    const int targetIndex = objectIndexWithId(shelves, shelfId);
    if (targetIndex < 0) {
        return failed(QStringLiteral("Choose a valid shelf for the new book."));
    }

    const QString ownerId = ownerOfShelf(shelves.at(targetIndex).toObject(),
                                         homeAuthorId(authors));
    const QString ownerName = authorName(authors, ownerId);
    if (ownerName.isEmpty()) {
        return failed(QStringLiteral("The selected shelf has no valid pen name."));
    }

    const QString bookId = newId(QStringLiteral("book-"));
    const QString chapterId = updated.value(QStringLiteral("writingStyle")).toString() ==
                                      QStringLiteral("plotter")
                                  ? QString()
                                  : newId(QStringLiteral("chapter-"));
    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    QJsonArray chapterOrder;
    QJsonObject chapterTitles;
    if (!chapterId.isEmpty()) {
        chapterOrder.append(chapterId);
        chapterTitles.insert(chapterId, QStringLiteral("Chapter 1"));
    }
    const QString normalizedTitle = title.trimmed().isEmpty()
                                        ? QStringLiteral("Untitled")
                                        : title.trimmed();
    const QJsonObject bookMetadata{
        {QStringLiteral("id"), bookId},
        {QStringLiteral("title"), normalizedTitle},
        {QStringLiteral("subtitle"), QString()},
        {QStringLiteral("series"), QString()},
        {QStringLiteral("author"), ownerName},
        {QStringLiteral("wordGoal"), 0},
        {QStringLiteral("created"), now},
        {QStringLiteral("modified"), now},
        {QStringLiteral("chapterOrder"), chapterOrder},
        {QStringLiteral("chapterTitles"), chapterTitles},
        {QStringLiteral("tabNames"), QJsonObject{
             {QStringLiteral("notes"), QStringLiteral("Notes")},
             {QStringLiteral("outline"), QStringLiteral("Outline")}}}};

    QString stagingError;
    if (!validateStagingRoot(libraryPath_, true, &stagingError)) {
        return failed(stagingError);
    }
    const QString stagingRoot = QDir(libraryPath_).filePath(QStringLiteral(".leo-staging"));
    QTemporaryDir staging(QDir(stagingRoot).filePath(QStringLiteral("new-book-XXXXXX")));
    if (!staging.isValid()) {
        return failed(QStringLiteral("Could not prepare the new book folder."));
    }
    const QByteArray bookBytes = QJsonDocument(bookMetadata).toJson(QJsonDocument::Indented);
    const bool filesWritten =
        writeNewBookFile(staging.path(), QStringLiteral("book.json"), bookBytes, &error) &&
        writeNewBookFile(staging.path(), QStringLiteral("notes.html"), QByteArray(), &error) &&
        writeNewBookFile(staging.path(), QStringLiteral("outline.html"), QByteArray(), &error) &&
        writeNewBookFile(staging.path(), QStringLiteral("darlings.json"),
                         QByteArrayLiteral("[]\n"), &error) &&
        writeNewBookFile(staging.path(), QStringLiteral("stickies.json"),
                         QByteArrayLiteral("[]\n"), &error);
    if (!filesWritten ||
        (!chapterId.isEmpty() &&
         !writeNewBookFile(staging.path(), QStringLiteral("chapters/") + chapterId +
                                                QStringLiteral(".html"),
                           QByteArrayLiteral("<p><br></p>\n"), &error))) {
        return failed(error);
    }

    const QString stagedDirectory = QStringLiteral(".leo-staging/") +
                                    QFileInfo(staging.path()).fileName();
    const QJsonArray targetBooks = shelves.at(targetIndex).toObject()
                                       .value(QStringLiteral("bookIds")).toArray();
    const int insertionIndex = targetBooks.size();
    const QJsonObject beforeCreation = metadata_;
    QJsonObject pendingCreate = updated;
    pendingCreate.insert(QStringLiteral("leoPendingCreate"), QJsonObject{
        {QStringLiteral("bookId"), bookId},
        {QStringLiteral("shelfId"), shelfId},
        {QStringLiteral("index"), insertionIndex},
        {QStringLiteral("stagingPath"), stagedDirectory}});
    const LibraryOrganizationResult prepared = commitMetadata(pendingCreate);
    if (!prepared.ok) {
        return prepared;
    }

    if (!validateStagingRoot(libraryPath_, false, &stagingError) ||
        QFileInfo::exists(QDir(libraryPath_).filePath(bookId)) ||
        !QDir(libraryPath_).rename(stagedDirectory, bookId)) {
        const LibraryOrganizationResult restored = commitMetadata(beforeCreation);
        LibraryOrganizationResult result = failed(
            stagingError.isEmpty()
                ? QStringLiteral("Could not finish creating the new book folder.")
                : stagingError);
        if (!restored.ok) {
            result.conflict = restored.conflict;
            result.error += QStringLiteral(" The creation record remains for recovery; reopen the Library. %1")
                                .arg(restored.error);
        }
        return result;
    }
    QJsonObject completed = metadata_;
    QJsonArray completedShelves = shelvesOf(completed);
    const int completedTargetIndex = objectIndexWithId(completedShelves, shelfId);
    if (completedTargetIndex < 0) {
        return failed(QStringLiteral("The new book folder is ready, but its shelf is missing. Reopen the Library to recover or inspect it."));
    }
    QJsonObject targetShelf = completedShelves.at(completedTargetIndex).toObject();
    QJsonArray bookIds = targetShelf.value(QStringLiteral("bookIds")).toArray();
    bookIds.insert(qBound(0, insertionIndex, bookIds.size()), bookId);
    targetShelf.insert(QStringLiteral("bookIds"), bookIds);
    completedShelves.replace(completedTargetIndex, targetShelf);
    completed.insert(QStringLiteral("shelves"), completedShelves);
    completed.remove(QStringLiteral("leoPendingCreate"));
    if (completed.value(QStringLiteral("initialBookId")).toString().isEmpty()) {
        completed.insert(QStringLiteral("initialBookId"), bookId);
    }

    LibraryOrganizationResult result = commitMetadata(completed);
    if (result.ok) {
        result.id = bookId;
    } else {
        result.error += QStringLiteral(" The creation record remains for recovery when the Library is reopened.");
    }
    return result;
}

LibraryOrganizationResult LibraryOrganization::renameBook(const QString &bookId,
                                                            const QString &title)
{
    if (!loaded_) {
        return failed(QStringLiteral("Open a Library before renaming a book."));
    }
    const QString normalizedTitle = title.trimmed();
    if (!validPathComponent(bookId) || !bookId.startsWith(QStringLiteral("book-"))) {
        return failed(QStringLiteral("The selected book ID is invalid."));
    }
    if (normalizedTitle.isEmpty()) {
        return failed(QStringLiteral("Enter a title for the book."));
    }

    const QString relativePath = bookId + QStringLiteral("/book.json");
    QByteArray bytes;
    QString error;
    if (!LibraryPersistence::readLibraryFile(libraryPath_, relativePath, &bytes, &error)) {
        return failed(error);
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return failed(QStringLiteral("Book metadata is not a valid JSON object."));
    }
    QJsonObject metadata = document.object();
    metadata.insert(QStringLiteral("title"), normalizedTitle);
    metadata.insert(QStringLiteral("modified"),
                    QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    const QByteArray updated = QJsonDocument(metadata).toJson(QJsonDocument::Indented);
    return commitBookMetadata(relativePath, bytes, updated);
}

LibraryOrganizationResult LibraryOrganization::moveBookToTrash(
    const QString &bookId, const LibraryTrashOperation &trashOperation)
{
    if (!loaded_) {
        return failed(QStringLiteral("Open a Library before moving a book to Trash."));
    }
    if (!validPathComponent(bookId) || !bookId.startsWith(QStringLiteral("book-"))) {
        return failed(QStringLiteral("The selected book ID is invalid."));
    }
    if (metadata_.contains(QStringLiteral("leoPendingTrash")) ||
        metadata_.contains(QStringLiteral("leoPendingCreate")) ||
        metadata_.contains(QStringLiteral("leoPendingMove"))) {
        return failed(QStringLiteral("An earlier Trash operation needs recovery before another can start."));
    }
    const QString bookPath = QDir(libraryPath_).filePath(bookId);
    const QFileInfo bookInfo(bookPath);
    if (!bookInfo.isDir() || bookInfo.isSymLink()) {
        return failed(QStringLiteral("The book folder is missing or unsafe: %1").arg(bookPath));
    }

    const QJsonObject beforeRemoval = metadata_;
    QJsonObject pending = metadata_;
    QString metadataError;
    if (!prepareOrganizationMetadata(&pending, &metadataError)) {
        return failed(metadataError);
    }
    QJsonArray shelves = shelvesOf(pending);
    QJsonArray memberships;
    for (int shelfPosition = 0; shelfPosition < shelves.size(); ++shelfPosition) {
        QJsonObject shelf = shelves.at(shelfPosition).toObject();
        const QJsonArray bookIds = shelf.value(QStringLiteral("bookIds")).toArray();
        for (int bookPosition = 0; bookPosition < bookIds.size(); ++bookPosition) {
            if (bookIds.at(bookPosition).toString() == bookId) {
                memberships.append(QJsonObject{
                    {QStringLiteral("shelfId"),
                     shelf.value(QStringLiteral("id")).toString()},
                    {QStringLiteral("index"), bookPosition}});
            }
        }
        shelf.insert(QStringLiteral("bookIds"), withoutBookId(bookIds, bookId));
        shelves.replace(shelfPosition, shelf);
    }
    pending.insert(QStringLiteral("shelves"), shelves);
    pending.insert(QStringLiteral("leoPendingTrash"), QJsonObject{
        {QStringLiteral("bookId"), bookId},
        {QStringLiteral("memberships"), memberships}});
    const LibraryOrganizationResult prepared = commitMetadata(pending);
    if (!prepared.ok) {
        return prepared;
    }

    QString trashError;
    bool moved = false;
    if (trashOperation) {
        moved = trashOperation(bookPath, &trashError);
    } else {
        QFile bookFolder(bookPath);
        moved = bookFolder.moveToTrash();
        if (!moved) {
            trashError = QStringLiteral("The system could not move the book folder to Trash.");
        }
    }
    if (!moved && QFileInfo(bookPath).isDir() && !QFileInfo(bookPath).isSymLink()) {
        const LibraryOrganizationResult restored = commitMetadata(beforeRemoval);
        LibraryOrganizationResult result = failed(
            trashError.isEmpty()
                ? QStringLiteral("The book was not moved to Trash. Its original folder is unchanged.")
                : QStringLiteral("%1 The original book folder is unchanged.").arg(trashError));
        if (!restored.ok) {
            result.conflict = restored.conflict;
            result.error += QStringLiteral(" Shelf membership is recorded for recovery; reopen the Library. %1")
                                .arg(restored.error);
        }
        return result;
    }

    QJsonObject finished = metadata_;
    finished.remove(QStringLiteral("leoPendingTrash"));
    const LibraryOrganizationResult finalized = commitMetadata(finished);
    if (!moved) {
        return failed(QStringLiteral(
            "The Trash operation did not confirm success and the original folder is missing. "
            "LEO recorded the membership change for recovery. %1")
                          .arg(finalized.ok ? QString() : finalized.error),
                      finalized.conflict);
    }
    if (!finalized.ok) {
        return failed(QStringLiteral(
            "The book was moved to Trash. LEO will finish recording the change when the Library "
            "is reopened. %1")
                          .arg(finalized.error),
                      finalized.conflict);
    }

    LibraryOrganizationResult result;
    result.ok = true;
    result.id = bookId;
    return result;
}
