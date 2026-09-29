#include "chapter_structure.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>
#include <QUuid>

#include <algorithm>

namespace {

QString chapterPath(const QString &bookId, const QString &chapterId)
{
    return bookId + QStringLiteral("/chapters/") + chapterId + QStringLiteral(".html");
}

bool readOptionalRecords(const QString &libraryPath, const QString &relativePath,
                         LegacyChapterRecordLinks *links, QString *error)
{
    const QFileInfo fileInfo(QDir(libraryPath).filePath(relativePath));
    if (!fileInfo.exists() && !fileInfo.isSymLink()) {
        return true;
    }

    QByteArray bytes;
    if (!LibraryPersistence::readLibraryFile(libraryPath, relativePath, &bytes, error)) {
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
        *error = QStringLiteral("%1 is not a valid JSON array.").arg(relativePath);
        return false;
    }

    for (int index = 0; index < document.array().size(); ++index) {
        const QJsonValue value = document.array().at(index);
        if (!value.isObject()) {
            *error = QStringLiteral("%1 has an invalid record at position %2.")
                         .arg(relativePath)
                         .arg(index + 1);
            return false;
        }
        const QJsonObject record = value.toObject();
        const QJsonValue idValue = record.value(QStringLiteral("id"));
        const QJsonValue chapterValue = record.value(QStringLiteral("chapterId"));
        if (!idValue.isString() || idValue.toString().isEmpty() ||
            (!chapterValue.isUndefined() && !chapterValue.isNull() &&
             !chapterValue.isString())) {
            *error = QStringLiteral("%1 has an invalid record or chapter link.")
                         .arg(relativePath);
            return false;
        }
        const QString id = idValue.toString();
        if (links->ids.contains(id)) {
            *error = QStringLiteral("%1 contains duplicate record ID '%2'.")
                         .arg(relativePath, id);
            return false;
        }
        links->ids.insert(id);
        if (chapterValue.isString()) {
            links->chapterIds.insert(id, chapterValue.toString());
        }
    }
    return true;
}

bool containsExactString(const QJsonValue &value, const QString &needle)
{
    if (value.isString()) {
        return value.toString() == needle;
    }
    if (value.isArray()) {
        for (const QJsonValue &item : value.toArray()) {
            if (containsExactString(item, needle)) {
                return true;
            }
        }
    } else if (value.isObject()) {
        const QJsonObject object = value.toObject();
        for (auto item = object.begin(); item != object.end(); ++item) {
            if (item.key() == needle || containsExactString(item.value(), needle)) {
                return true;
            }
        }
    }
    return false;
}

bool metadataValueHasContent(const QJsonValue &value)
{
    if (value.isUndefined() || value.isNull()) {
        return false;
    }
    if (value.isArray()) {
        return !value.toArray().isEmpty();
    }
    if (value.isObject()) {
        return !value.toObject().isEmpty();
    }
    if (value.isString()) {
        return !value.toString().isEmpty();
    }
    return true;
}

void removeEmptyMetadataEntry(QJsonObject *book, const QString &metadataKey,
                              const QString &chapterId)
{
    const QJsonValue value = book->value(metadataKey);
    if (!value.isObject()) {
        return;
    }
    QJsonObject entries = value.toObject();
    entries.remove(chapterId);
    book->insert(metadataKey, entries);
}

QString mergedParagraphs(const QString &first, const QString &second)
{
    if (first.isEmpty()) {
        return second;
    }
    if (second.isEmpty()) {
        return first;
    }
    return first + QLatin1Char('\n') + second;
}

void updateLastPosition(QJsonObject *book, const QString &removedChapterId,
                        const QString &replacementChapterId)
{
    const QJsonValue value = book->value(QStringLiteral("lastPosition"));
    if (!value.isObject()) {
        return;
    }
    QJsonObject lastPosition = value.toObject();
    if (lastPosition.value(QStringLiteral("chapterId")).toString() != removedChapterId) {
        return;
    }
    if (replacementChapterId.isEmpty()) {
        lastPosition.remove(QStringLiteral("chapterId"));
    } else {
        lastPosition.insert(QStringLiteral("chapterId"), replacementChapterId);
    }
    book->insert(QStringLiteral("lastPosition"), lastPosition);
}

QString newChapterId()
{
    return QStringLiteral("ch-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

} // namespace

ChapterStructure::ChapterStructure(QString libraryPath, QString bookId)
    : libraryPath_(QFileInfo(libraryPath).absoluteFilePath()),
      bookId_(std::move(bookId)),
      bookRelativePath_(bookId_ + QStringLiteral("/book.json"))
{
}

bool ChapterStructure::load(QString *error)
{
    bookBytes_.clear();
    bookHash_.clear();
    history_.clear();
    historyIndex_ = 0;
    loaded_ = false;
    if (!LibraryPersistence::readLibraryFile(libraryPath_, bookRelativePath_,
                                             &bookBytes_, error)) {
        return false;
    }
    QJsonObject book;
    if (!currentBook(&book, error)) {
        return false;
    }
    bookHash_ = LibraryPersistence::hash(bookBytes_);
    loaded_ = true;
    return true;
}

ChapterStructureResult ChapterStructure::addChapter(int index, const QString &title)
{
    QJsonObject book;
    QJsonArray order;
    QString error;
    if (!currentBook(&book, &error) || !chapterOrder(book, &order, &error)) {
        return fail(error);
    }
    if (index < 0 || index > order.size()) {
        return fail(QStringLiteral("Choose a chapter position within this book."));
    }
    const QJsonValue titlesValue = book.value(QStringLiteral("chapterTitles"));
    if (!titlesValue.isUndefined() && !titlesValue.isObject()) {
        return fail(QStringLiteral("Chapter titles in book.json are invalid; the Library was not changed."));
    }

    const QString id = newChapterId();
    const QString normalizedTitle = title.trimmed().isEmpty()
        ? QStringLiteral("Chapter %1").arg(index + 1)
        : title.trimmed();
    QJsonObject titles = titlesValue.toObject();
    titles.insert(id, normalizedTitle);
    order.insert(index, id);
    book.insert(QStringLiteral("chapterOrder"), order);
    book.insert(QStringLiteral("chapterTitles"), titles);

    PlannedFile chapter;
    chapter.change = {chapterPath(bookId_, id), {}, true,
                      QByteArrayLiteral("<p><br></p>")};
    chapter.beforeExists = false;
    return commitBookChange(book, {chapter}, QString(), id);
}

ChapterStructureResult ChapterStructure::renameChapter(const QString &chapterId,
                                                        const QString &title)
{
    QJsonObject book;
    QJsonArray order;
    QString error;
    int index = -1;
    if (!currentBook(&book, &error) || !chapterOrder(book, &order, &error) ||
        !chapterIndex(order, chapterId, &index)) {
        return fail(error.isEmpty() ? QStringLiteral("The chapter is no longer in this book.")
                                    : error);
    }
    const QJsonValue titlesValue = book.value(QStringLiteral("chapterTitles"));
    if (!titlesValue.isUndefined() && !titlesValue.isObject()) {
        return fail(QStringLiteral("Chapter titles in book.json are invalid; the Library was not changed."));
    }
    QJsonObject titles = titlesValue.toObject();
    const QString normalized = title.trimmed();
    if (normalized.isEmpty()) {
        titles.remove(chapterId);
    } else {
        titles.insert(chapterId, normalized);
    }
    book.insert(QStringLiteral("chapterTitles"), titles);
    return commitBookChange(book, {}, chapterId, chapterId);
}

ChapterStructureResult ChapterStructure::moveChapter(const QString &chapterId, int index)
{
    QJsonObject book;
    QJsonArray order;
    QString error;
    int oldIndex = -1;
    if (!currentBook(&book, &error) || !chapterOrder(book, &order, &error) ||
        !chapterIndex(order, chapterId, &oldIndex)) {
        return fail(error.isEmpty() ? QStringLiteral("The chapter is no longer in this book.")
                                    : error);
    }
    if (index < 0 || index >= order.size()) {
        return fail(QStringLiteral("Choose a chapter position within this book."));
    }
    if (oldIndex == index) {
        ChapterStructureResult result;
        result.ok = true;
        result.chapterId = chapterId;
        return result;
    }
    order.removeAt(oldIndex);
    order.insert(index, chapterId);
    book.insert(QStringLiteral("chapterOrder"), order);
    return commitBookChange(book, {}, chapterId, chapterId);
}

ChapterStructureResult ChapterStructure::splitChapter(
    const QString &chapterId, int position, const ChapterEditSource &source,
    const QString &newTitle)
{
    QJsonObject book;
    QJsonArray order;
    QString error;
    int index = -1;
    if (!currentBook(&book, &error) || !chapterOrder(book, &order, &error) ||
        !chapterIndex(order, chapterId, &index)) {
        return fail(error.isEmpty() ? QStringLiteral("The chapter is no longer in this book.")
                                    : error);
    }
    if (source.expectedHash.isEmpty() ||
        LibraryPersistence::hash(source.originalBytes) != source.expectedHash) {
        return fail(QStringLiteral("LEO no longer has the original chapter bytes; reopen the chapter before splitting."),
                    true);
    }
    if (!contentOperationSafe(book, {chapterId}, &error)) {
        return fail(error);
    }
    LegacyChapterLinkContext links;
    if (!loadChapterLinks(chapterId, &links, &error)) {
        return fail(error);
    }
    const LegacyChapterDocument document =
        LegacyChapterCodec::decode(source.currentBytes, links);
    if (!document.refusalReason.isEmpty() || document.hasProtectedContent()) {
        return fail(document.refusalReason.isEmpty()
                        ? QStringLiteral("This chapter contains protected legacy content. Split was refused without changing the Library.")
                        : document.refusalReason + QStringLiteral(" Split was refused without changing the Library."));
    }
    if (position < 0 || position > document.text.size()) {
        return fail(QStringLiteral("The split point is outside the chapter text."));
    }
    if (position > 0 && position < document.text.size() &&
        document.text.at(position - 1).isHighSurrogate() &&
        document.text.at(position).isLowSurrogate()) {
        return fail(QStringLiteral("The split point falls inside a Unicode character."));
    }

    const QString addedId = newChapterId();
    const QString normalizedTitle = newTitle.trimmed().isEmpty()
        ? QStringLiteral("Chapter %1").arg(index + 2)
        : newTitle.trimmed();
    QString encodeError;
    const QByteArray firstBytes = LegacyChapterCodec::encode(
        document.text.left(position), document.hasUtf8Bom, &encodeError);
    if (!encodeError.isEmpty()) {
        return fail(encodeError);
    }
    const QByteArray secondBytes = LegacyChapterCodec::encode(
        document.text.mid(position), false, &encodeError);
    if (!encodeError.isEmpty()) {
        return fail(encodeError);
    }
    const QJsonValue titlesValue = book.value(QStringLiteral("chapterTitles"));
    if (!titlesValue.isUndefined() && !titlesValue.isObject()) {
        return fail(QStringLiteral("Chapter titles in book.json are invalid; the Library was not changed."));
    }
    QJsonObject titles = titlesValue.toObject();
    titles.insert(addedId, normalizedTitle);
    order.insert(index + 1, addedId);
    book.insert(QStringLiteral("chapterOrder"), order);
    book.insert(QStringLiteral("chapterTitles"), titles);

    PlannedFile createChapter;
    createChapter.change = {chapterPath(bookId_, addedId), {}, true, secondBytes};
    createChapter.beforeExists = false;
    PlannedFile updateChapter;
    updateChapter.change = {chapterPath(bookId_, chapterId), source.expectedHash,
                            false, firstBytes};
    updateChapter.beforeBytes = source.originalBytes;
    return commitBookChange(book, {createChapter, updateChapter}, chapterId, addedId);
}

ChapterStructureResult ChapterStructure::joinChapter(
    const QString &chapterId, bool withPrevious, const ChapterEditSource &source)
{
    QJsonObject book;
    QJsonArray order;
    QString error;
    int sourceIndex = -1;
    if (!currentBook(&book, &error) || !chapterOrder(book, &order, &error) ||
        !chapterIndex(order, chapterId, &sourceIndex)) {
        return fail(error.isEmpty() ? QStringLiteral("The chapter is no longer in this book.")
                                    : error);
    }
    const int destinationIndex = withPrevious ? sourceIndex - 1 : sourceIndex;
    const int removedIndex = withPrevious ? sourceIndex : sourceIndex + 1;
    if (destinationIndex < 0 || removedIndex >= order.size()) {
        return fail(withPrevious ? QStringLiteral("There is no previous chapter to join.")
                                : QStringLiteral("There is no next chapter to join."));
    }
    if (source.expectedHash.isEmpty() ||
        LibraryPersistence::hash(source.originalBytes) != source.expectedHash) {
        return fail(QStringLiteral("LEO no longer has the original chapter bytes; reopen the chapter before joining."),
                    true);
    }

    const QString destinationId = order.at(destinationIndex).toString();
    const QString removedId = order.at(removedIndex).toString();
    if (!contentOperationSafe(book, {destinationId, removedId}, &error)) {
        return fail(error);
    }

    LegacyChapterLinkContext currentLinks;
    if (!loadChapterLinks(chapterId, &currentLinks, &error)) {
        return fail(error);
    }
    const LegacyChapterDocument currentDocument =
        LegacyChapterCodec::decode(source.currentBytes, currentLinks);
    if (!currentDocument.refusalReason.isEmpty() || currentDocument.hasProtectedContent()) {
        return fail(currentDocument.refusalReason.isEmpty()
                        ? QStringLiteral("This chapter contains protected legacy content. Join was refused without changing the Library.")
                        : currentDocument.refusalReason + QStringLiteral(" Join was refused without changing the Library."));
    }

    const QString otherId = withPrevious ? destinationId : removedId;
    QByteArray otherBytes;
    QString otherPath;
    if (!readChapter(otherId, &otherBytes, &otherPath, &error)) {
        return fail(error);
    }
    LegacyChapterLinkContext otherLinks;
    if (!loadChapterLinks(otherId, &otherLinks, &error)) {
        return fail(error);
    }
    const LegacyChapterDocument otherDocument = LegacyChapterCodec::decode(otherBytes, otherLinks);
    if (!otherDocument.refusalReason.isEmpty() || otherDocument.hasProtectedContent()) {
        return fail(otherDocument.refusalReason.isEmpty()
                        ? QStringLiteral("This chapter contains protected legacy content. Join was refused without changing the Library.")
                        : otherDocument.refusalReason + QStringLiteral(" Join was refused without changing the Library."));
    }

    const QString joinedText = withPrevious
        ? mergedParagraphs(otherDocument.text, currentDocument.text)
        : mergedParagraphs(currentDocument.text, otherDocument.text);
    QString encodeError;
    const bool hasBom = withPrevious ? otherDocument.hasUtf8Bom : currentDocument.hasUtf8Bom;
    const QByteArray joinedBytes = LegacyChapterCodec::encode(joinedText, hasBom, &encodeError);
    if (!encodeError.isEmpty()) {
        return fail(encodeError);
    }

    QVector<PlannedFile> changedFiles;
    if (withPrevious) {
        PlannedFile destination;
        destination.change = {otherPath, LibraryPersistence::hash(otherBytes), false,
                              joinedBytes};
        destination.beforeBytes = otherBytes;
        changedFiles.append(destination);
        PlannedFile removedSource;
        removedSource.change = {chapterPath(bookId_, chapterId), source.expectedHash,
                                false, source.currentBytes};
        removedSource.beforeBytes = source.originalBytes;
        changedFiles.append(removedSource);
    } else {
        PlannedFile destination;
        destination.change = {chapterPath(bookId_, chapterId), source.expectedHash,
                              false, joinedBytes};
        destination.beforeBytes = source.originalBytes;
        changedFiles.append(destination);
        PlannedFile removedSource;
        removedSource.change = {otherPath, LibraryPersistence::hash(otherBytes),
                                false, otherBytes};
        removedSource.beforeBytes = otherBytes;
        changedFiles.append(removedSource);
    }

    order.removeAt(removedIndex);
    book.insert(QStringLiteral("chapterOrder"), order);
    const QJsonValue titlesValue = book.value(QStringLiteral("chapterTitles"));
    if (!titlesValue.isUndefined() && !titlesValue.isObject()) {
        return fail(QStringLiteral("Chapter titles in book.json are invalid; the Library was not changed."));
    }
    QJsonObject titles = titlesValue.toObject();
    titles.remove(removedId);
    book.insert(QStringLiteral("chapterTitles"), titles);
    removeEmptyMetadataEntry(&book, QStringLiteral("chapterNotes"), removedId);
    removeEmptyMetadataEntry(&book, QStringLiteral("sectionNotes"), removedId);
    updateLastPosition(&book, removedId, destinationId);
    return commitBookChange(book, changedFiles, chapterId, destinationId);
}

ChapterStructureResult ChapterStructure::deleteChapter(
    const QString &chapterId, const ChapterEditSource &source)
{
    QJsonObject book;
    QJsonArray order;
    QString error;
    int index = -1;
    if (!currentBook(&book, &error) || !chapterOrder(book, &order, &error) ||
        !chapterIndex(order, chapterId, &index)) {
        return fail(error.isEmpty() ? QStringLiteral("The chapter is no longer in this book.")
                                    : error);
    }
    if (source.expectedHash.isEmpty() ||
        LibraryPersistence::hash(source.originalBytes) != source.expectedHash) {
        return fail(QStringLiteral("LEO no longer has the original chapter bytes; reopen the chapter before deleting."),
                    true);
    }
    if (!contentOperationSafe(book, {chapterId}, &error)) {
        return fail(error);
    }
    LegacyChapterLinkContext links;
    if (!loadChapterLinks(chapterId, &links, &error)) {
        return fail(error);
    }
    const LegacyChapterDocument document =
        LegacyChapterCodec::decode(source.originalBytes, links);
    if (!document.refusalReason.isEmpty() || document.hasProtectedContent()) {
        return fail(document.refusalReason.isEmpty()
                        ? QStringLiteral("This chapter contains protected legacy content. Delete was refused without changing the Library.")
                        : document.refusalReason + QStringLiteral(" Delete was refused without changing the Library."));
    }

    PlannedFile guard;
    guard.change = {chapterPath(bookId_, chapterId), source.expectedHash,
                    false, source.originalBytes};
    guard.beforeBytes = source.originalBytes;
    QJsonObject updated = book;
    order.removeAt(index);
    updated.insert(QStringLiteral("chapterOrder"), order);
    const QJsonValue titlesValue = updated.value(QStringLiteral("chapterTitles"));
    if (!titlesValue.isUndefined() && !titlesValue.isObject()) {
        return fail(QStringLiteral("Chapter titles in book.json are invalid; the Library was not changed."));
    }
    QJsonObject titles = titlesValue.toObject();
    titles.remove(chapterId);
    updated.insert(QStringLiteral("chapterTitles"), titles);
    removeEmptyMetadataEntry(&updated, QStringLiteral("chapterNotes"), chapterId);
    removeEmptyMetadataEntry(&updated, QStringLiteral("sectionNotes"), chapterId);
    const QString replacementId = order.isEmpty()
        ? QString()
        : order.at(std::min(index, static_cast<int>(order.size() - 1))).toString();
    updateLastPosition(&updated, chapterId, replacementId);
    return commitBookChange(updated, {guard}, chapterId, replacementId);
}

ChapterStructureResult ChapterStructure::undo()
{
    if (!canUndo()) {
        return fail(QStringLiteral("There is no chapter structure change to undo."));
    }
    const HistoryEntry &entry = history_.at(historyIndex_ - 1);
    QVector<PersistenceFileChange> changes;
    for (const HistoryFile &file : entry.files) {
        changes.append({file.relativePath, LibraryPersistence::hash(file.afterBytes), false,
                        file.beforeExists ? file.beforeBytes : file.afterBytes});
    }
    const PersistenceResult saved = LibraryPersistence::saveFiles(libraryPath_, changes);
    if (!saved.ok) {
        return fail(saved.error, saved.conflict);
    }
    for (const HistoryFile &file : entry.files) {
        if (file.relativePath == bookRelativePath_) {
            bookBytes_ = file.beforeBytes;
            bookHash_ = LibraryPersistence::hash(bookBytes_);
            break;
        }
    }
    --historyIndex_;
    ChapterStructureResult result;
    result.ok = true;
    result.chapterId = entry.chapterBefore;
    return result;
}

ChapterStructureResult ChapterStructure::redo()
{
    if (!canRedo()) {
        return fail(QStringLiteral("There is no chapter structure change to redo."));
    }
    const HistoryEntry &entry = history_.at(historyIndex_);
    QVector<PersistenceFileChange> changes;
    for (const HistoryFile &file : entry.files) {
        const QByteArray expected = file.beforeExists ? file.beforeBytes : file.afterBytes;
        changes.append({file.relativePath, LibraryPersistence::hash(expected), false,
                        file.afterBytes});
    }
    const PersistenceResult saved = LibraryPersistence::saveFiles(libraryPath_, changes);
    if (!saved.ok) {
        return fail(saved.error, saved.conflict);
    }
    for (const HistoryFile &file : entry.files) {
        if (file.relativePath == bookRelativePath_) {
            bookBytes_ = file.afterBytes;
            bookHash_ = LibraryPersistence::hash(bookBytes_);
            break;
        }
    }
    ++historyIndex_;
    ChapterStructureResult result;
    result.ok = true;
    result.chapterId = entry.chapterAfter;
    return result;
}

bool ChapterStructure::canUndo() const
{
    return historyIndex_ > 0;
}

bool ChapterStructure::canRedo() const
{
    return historyIndex_ < history_.size();
}

void ChapterStructure::invalidateHistoryForChapterEdit(const QString &relativePath)
{
    bool affectsHistory = false;
    for (int index = 0; index < historyIndex_; ++index) {
        const HistoryEntry &entry = history_.at(index);
        affectsHistory = std::any_of(
            entry.files.cbegin(), entry.files.cend(),
            [&relativePath](const HistoryFile &file) {
                return file.relativePath == relativePath;
            });
        if (affectsHistory) {
            break;
        }
    }
    if (affectsHistory) {
        history_.clear();
        historyIndex_ = 0;
    } else {
        history_.resize(historyIndex_);
    }
}

ChapterStructureResult ChapterStructure::commitBookChange(
    const QJsonObject &updatedBook, const QVector<PlannedFile> &otherFiles,
    const QString &chapterBefore, const QString &chapterAfter)
{
    QString error;
    QJsonObject current;
    if (!currentBook(&current, &error)) {
        return fail(error);
    }

    const QByteArray updatedBookBytes =
        QJsonDocument(updatedBook).toJson(QJsonDocument::Indented);
    QVector<PersistenceFileChange> changes;
    HistoryEntry entry;
    entry.chapterBefore = chapterBefore;
    entry.chapterAfter = chapterAfter;
    for (const PlannedFile &planned : otherFiles) {
        if (planned.change.relativePath == bookRelativePath_) {
            return fail(QStringLiteral("A chapter transaction cannot replace book.json twice."));
        }
        if (planned.beforeExists &&
            LibraryPersistence::hash(planned.beforeBytes) != planned.change.expectedHash) {
            return fail(QStringLiteral("LEO could not verify the chapter bytes needed for a safe structure change."),
                        true);
        }
        changes.append(planned.change);
        entry.files.append({planned.change.relativePath, planned.beforeExists,
                            planned.beforeBytes, planned.change.newBytes});
    }

    changes.append({bookRelativePath_, bookHash_, false, updatedBookBytes});
    entry.files.append({bookRelativePath_, true, bookBytes_, updatedBookBytes});
    bool anyChange = false;
    for (const PersistenceFileChange &change : changes) {
        anyChange = anyChange || change.expectedAbsent ||
                    change.expectedHash != LibraryPersistence::hash(change.newBytes);
    }
    if (!anyChange) {
        ChapterStructureResult result;
        result.ok = true;
        result.chapterId = chapterAfter;
        return result;
    }

    const PersistenceResult saved = LibraryPersistence::saveFiles(libraryPath_, changes);
    if (!saved.ok) {
        return fail(saved.error, saved.conflict);
    }
    history_.resize(historyIndex_);
    history_.append(entry);
    historyIndex_ = history_.size();
    bookBytes_ = updatedBookBytes;
    bookHash_ = LibraryPersistence::hash(bookBytes_);

    ChapterStructureResult result;
    result.ok = true;
    result.chapterId = chapterAfter;
    return result;
}

bool ChapterStructure::currentBook(QJsonObject *book, QString *error) const
{
    if (bookBytes_.isEmpty()) {
        if (error) {
            *error = QStringLiteral("The book metadata is not loaded.");
        }
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bookBytes_, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) {
            *error = QStringLiteral("book.json is invalid; chapter structure was left unchanged.");
        }
        return false;
    }
    const QJsonObject result = document.object();
    const QJsonValue idValue = result.value(QStringLiteral("id"));
    if (idValue.isString() && !idValue.toString().isEmpty() &&
        idValue.toString() != bookId_) {
        if (error) {
            *error = QStringLiteral("book.json belongs to a different book; chapter structure was left unchanged.");
        }
        return false;
    }
    *book = result;
    return true;
}

bool ChapterStructure::chapterOrder(const QJsonObject &book, QJsonArray *order,
                                   QString *error) const
{
    const QJsonValue value = book.value(QStringLiteral("chapterOrder"));
    if (!value.isArray()) {
        if (error) {
            *error = QStringLiteral("chapterOrder in book.json is invalid; the Library was not changed.");
        }
        return false;
    }
    QSet<QString> ids;
    for (const QJsonValue &chapter : value.toArray()) {
        if (!chapter.isString() || chapter.toString().isEmpty() ||
            ids.contains(chapter.toString())) {
            if (error) {
                *error = QStringLiteral("chapterOrder contains an invalid or duplicate chapter ID.");
            }
            return false;
        }
        ids.insert(chapter.toString());
    }
    *order = value.toArray();
    return true;
}

bool ChapterStructure::chapterIndex(const QJsonArray &order, const QString &chapterId,
                                    int *index) const
{
    for (int position = 0; position < order.size(); ++position) {
        if (order.at(position).toString() == chapterId) {
            *index = position;
            return true;
        }
    }
    return false;
}

bool ChapterStructure::loadChapterLinks(const QString &chapterId,
                                        LegacyChapterLinkContext *links,
                                        QString *error) const
{
    links->chapterId = chapterId;
    const QString prefix = bookId_ + QLatin1Char('/');
    if (!readOptionalRecords(libraryPath_, prefix + QStringLiteral("stickies.json"),
                             &links->stickies, error) ||
        !readOptionalRecords(libraryPath_, prefix + QStringLiteral("darlings.json"),
                             &links->darlings, error)) {
        return false;
    }

    QJsonObject book;
    if (!currentBook(&book, error)) {
        return false;
    }
    const QJsonValue sectionNotesValue = book.value(QStringLiteral("sectionNotes"));
    if (sectionNotesValue.isUndefined()) {
        return true;
    }
    if (!sectionNotesValue.isObject()) {
        *error = QStringLiteral("book.json has invalid sectionNotes metadata.");
        return false;
    }
    const QJsonValue chapterSections =
        sectionNotesValue.toObject().value(chapterId);
    if (chapterSections.isUndefined()) {
        return true;
    }
    if (!chapterSections.isArray()) {
        *error = QStringLiteral("book.json has invalid section notes for chapter '%1'.")
                     .arg(chapterId);
        return false;
    }
    for (int index = 0; index < chapterSections.toArray().size(); ++index) {
        const QJsonValue sectionValue = chapterSections.toArray().at(index);
        if (!sectionValue.isObject()) {
            *error = QStringLiteral("book.json has an invalid section note for chapter '%1'.")
                         .arg(chapterId);
            return false;
        }
        const QJsonValue idValue = sectionValue.toObject().value(QStringLiteral("id"));
        if (!idValue.isString() || idValue.toString().isEmpty() ||
            links->sectionIds.contains(idValue.toString())) {
            *error = QStringLiteral("book.json has an invalid or duplicate section ID for chapter '%1'.")
                         .arg(chapterId);
            return false;
        }
        links->sectionIds.insert(idValue.toString());
    }
    return true;
}

bool ChapterStructure::contentOperationSafe(const QJsonObject &book,
                                            const QStringList &chapterIds,
                                            QString *error) const
{
    QSet<QString> affected;
    for (const QString &chapterId : chapterIds) {
        affected.insert(chapterId);
    }
    for (const QString &metadataKey : {QStringLiteral("chapterNotes"),
                                       QStringLiteral("sectionNotes")}) {
        const QJsonValue value = book.value(metadataKey);
        if (value.isUndefined()) {
            continue;
        }
        if (!value.isObject()) {
            *error = QStringLiteral("%1 in book.json cannot be verified. The structural edit was refused.")
                         .arg(metadataKey);
            return false;
        }
        const QJsonObject records = value.toObject();
        for (const QString &chapterId : chapterIds) {
            if (metadataValueHasContent(records.value(chapterId))) {
                *error = QStringLiteral("This chapter has notes or outline links. The structural edit was refused so their ownership stays intact.");
                return false;
            }
        }
    }

    static const QSet<QString> knownFields{
        QStringLiteral("id"), QStringLiteral("title"), QStringLiteral("author"),
        QStringLiteral("chapterOrder"), QStringLiteral("chapterTitles"),
        QStringLiteral("chapterNotes"), QStringLiteral("sectionNotes"),
        QStringLiteral("lastPosition")};
    for (auto field = book.begin(); field != book.end(); ++field) {
        if (!knownFields.contains(field.key())) {
            for (const QString &chapterId : chapterIds) {
                if (containsExactString(field.value(), chapterId)) {
                    *error = QStringLiteral("Unknown book metadata refers to this chapter. The structural edit was refused to preserve that relationship.");
                    return false;
                }
            }
        }
    }

    LegacyChapterRecordLinks stickies;
    LegacyChapterRecordLinks darlings;
    if (!readOptionalRecords(libraryPath_, bookId_ + QStringLiteral("/stickies.json"),
                             &stickies, error) ||
        !readOptionalRecords(libraryPath_, bookId_ + QStringLiteral("/darlings.json"),
                             &darlings, error)) {
        return false;
    }
    for (const LegacyChapterRecordLinks *records : {&stickies, &darlings}) {
        if (records->chapterIds.size() != records->ids.size()) {
            *error = QStringLiteral("A sticky or Darling record has no verifiable chapter owner. The structural edit was refused.");
            return false;
        }
        for (auto record = records->chapterIds.cbegin();
             record != records->chapterIds.cend(); ++record) {
            if (affected.contains(record.value())) {
                *error = QStringLiteral("This chapter has linked sticky or Darling content. The structural edit was refused so ownership stays intact.");
                return false;
            }
        }
    }
    return true;
}

bool ChapterStructure::readChapter(const QString &chapterId, QByteArray *bytes,
                                   QString *relativePath, QString *error) const
{
    *relativePath = chapterPath(bookId_, chapterId);
    return LibraryPersistence::readLibraryFile(libraryPath_, *relativePath, bytes, error);
}

ChapterStructureResult ChapterStructure::fail(const QString &error, bool conflict) const
{
    ChapterStructureResult result;
    result.conflict = conflict;
    result.error = error;
    return result;
}
