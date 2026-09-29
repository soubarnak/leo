#include "book_search.h"

#include "legacy_chapter_codec.h"
#include "chapter_links.h"
#include "library_persistence.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextCursor>
#include <QTextDocument>
#include <QFileInfo>
#include <QSet>

namespace {
struct ChapterSource {
    QString id;
    QString path;
    QByteArray bytes;
    LegacyChapterDocument decoded;
};

bool load(const QString &library, const QString &book, QVector<ChapterSource> *chapters,
          QString *error)
{
    QByteArray metadata;
    if (!LibraryPersistence::readLibraryFile(library, book + QStringLiteral("/book.json"),
                                             &metadata, error)) return false;
    const QJsonDocument json = QJsonDocument::fromJson(metadata);
    const QJsonValue order = json.object().value(QStringLiteral("chapterOrder"));
    if (!json.isObject() || !order.isArray()) {
        *error = QStringLiteral("Book chapter order is invalid.");
        return false;
    }
    QSet<QString> seen;
    for (const QJsonValue &value : order.toArray()) {
        if (!value.isString() || value.toString().isEmpty() ||
            seen.contains(value.toString())) {
            *error = QStringLiteral("Book chapter order contains an invalid chapter.");
            return false;
        }
        seen.insert(value.toString());
        ChapterSource chapter;
        chapter.id = value.toString();
        chapter.path = book + QStringLiteral("/chapters/") + chapter.id + QStringLiteral(".html");
        if (!LibraryPersistence::readLibraryFile(library, chapter.path, &chapter.bytes, error))
            return false;
        chapter.decoded = LegacyChapterCodec::decode(
            chapter.bytes, loadChapterLinks(library, book, chapter.id));
        chapters->append(chapter);
    }
    return true;
}

QVector<BookSearchHit> matches(const ChapterSource &chapter, const QString &query)
{
    QVector<BookSearchHit> hits;
    if (query.isEmpty()) return hits;
    const QString text = chapter.decoded.text;
    int from = 0;
    while ((from = text.indexOf(query, from, Qt::CaseInsensitive)) >= 0) {
        bool synthetic = false;
        int offset = 0;
        for (const auto &fragment : chapter.decoded.fragments) {
            const int end = offset + (fragment.kind == LegacyChapterContentKind::Protected
                                          ? fragment.token.size() : fragment.text.size());
            if (fragment.kind == LegacyChapterContentKind::Protected &&
                from < end && from + query.size() > offset) synthetic = true;
            offset = end + 1;
        }
        if (!synthetic)
            hits.append({chapter.id, from, static_cast<int>(query.size()),
                         !chapter.decoded.editable()});
        from += query.size();
    }
    // Semantic HTML is hidden behind a token. Report source matches as blocked.
    for (const auto &fragment : chapter.decoded.fragments) {
        if (fragment.kind != LegacyChapterContentKind::Protected) continue;
        int sourcePosition = 0;
        while ((sourcePosition = fragment.rawSource.indexOf(
                    query, sourcePosition, Qt::CaseInsensitive)) >= 0) {
            hits.append({chapter.id, -1, static_cast<int>(query.size()), true});
            sourcePosition += query.size();
        }
    }
    if (!chapter.decoded.editable() && hits.isEmpty() &&
        QString::fromUtf8(chapter.bytes).contains(query, Qt::CaseInsensitive))
        hits.append({chapter.id, -1, static_cast<int>(query.size()), true});
    return hits;
}
}

BookSearch::BookSearch(QString libraryPath, QString bookId)
    : libraryPath_(std::move(libraryPath)), bookId_(std::move(bookId)) {}

BookSearchResult BookSearch::find(const QString &query) const
{
    BookSearchResult result;
    if (query.isEmpty()) { result.error = QStringLiteral("Enter text to find."); return result; }
    QVector<ChapterSource> chapters;
    if (!load(libraryPath_, bookId_, &chapters, &result.error)) return result;
    for (const ChapterSource &chapter : chapters) result.hits += matches(chapter, query);
    result.ok = true;
    return result;
}

BookSearchResult BookSearch::replaceAll(const QString &query, const QString &replacement)
{ return replace(query, replacement, {}, -1, true); }

BookSearchResult BookSearch::replaceOne(const QString &query, const QString &replacement,
                                        const QString &chapterId, int position)
{ return replace(query, replacement, chapterId, position, false); }

BookSearchResult BookSearch::replace(const QString &query, const QString &replacement,
                                     const QString &chapterId, int position, bool all)
{
    BookSearchResult result;
    if (query.isEmpty()) { result.error = QStringLiteral("Enter text to find."); return result; }
    QVector<ChapterSource> chapters;
    if (!load(libraryPath_, bookId_, &chapters, &result.error)) return result;
    QVector<PersistenceFileChange> changes;
    QVector<UndoFile> undo;
    for (const ChapterSource &chapter : chapters) {
        const auto hits = matches(chapter, query);
        result.hits += hits;
        for (const auto &hit : hits) {
            if (hit.blocked && (all || (hit.chapterId == chapterId && hit.position == position))) {
                result.error = QStringLiteral("A match is in protected or semantic content in chapter %1. No chapters were changed.").arg(chapter.id);
                return result;
            }
        }
        QVector<BookSearchHit> selected;
        for (const auto &hit : hits)
            if (!hit.blocked && (all || (hit.chapterId == chapterId && hit.position == position)))
                selected.append(hit);
        if (selected.isEmpty()) continue;
        QTextDocument document;
        document.setPlainText(chapter.decoded.text);
        LegacyChapterCodec::applyFormatting(chapter.decoded, &document);
        for (int index = selected.size() - 1; index >= 0; --index) {
            QTextCursor cursor(&document);
            cursor.setPosition(selected.at(index).position);
            cursor.setPosition(selected.at(index).position + selected.at(index).length,
                               QTextCursor::KeepAnchor);
            cursor.insertText(replacement);
        }
        QString error;
        const QByteArray after = LegacyChapterCodec::encodeRich(chapter.decoded, &document, &error);
        if (!error.isEmpty()) { result.error = error; return result; }
        changes.append({chapter.path, LibraryPersistence::hash(chapter.bytes), false, after});
        undo.append({chapter.path, chapter.bytes, after});
        result.changed += selected.size();
    }
    if (changes.isEmpty()) {
        if (!all) result.error = QStringLiteral("The selected match is no longer available. Find again.");
        else result.ok = true;
        return result;
    }
    const auto saved = LibraryPersistence::saveFiles(libraryPath_, changes);
    if (!saved.ok) { result.error = saved.error; result.changed = 0; return result; }
    undoBatches_.append(undo);
    result.ok = true;
    return result;
}

bool BookSearch::canUndo() const { return !undoBatches_.isEmpty(); }

BookSearchResult BookSearch::undo()
{
    BookSearchResult result;
    if (undoBatches_.isEmpty()) {
        result.error = QStringLiteral("There is no replacement batch to undo.");
        return result;
    }
    QVector<PersistenceFileChange> changes;
    for (const UndoFile &file : undoBatches_.last())
        changes.append({file.path, LibraryPersistence::hash(file.after), false, file.before});
    const auto saved = LibraryPersistence::saveFiles(libraryPath_, changes);
    if (!saved.ok) { result.error = saved.error; return result; }
    result.ok = true;
    result.changed = changes.size();
    undoBatches_.removeLast();
    return result;
}
