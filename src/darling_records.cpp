#include "darling_records.h"

#include "chapter_links.h"
#include "legacy_chapter_codec.h"
#include "library_persistence.h"

#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QUuid>

namespace {

QString chapterPath(const QString &bookId, const QString &chapterId)
{
    return bookId + QStringLiteral("/chapters/") + chapterId + QStringLiteral(".html");
}

DarlingResult failure(const QString &error, bool review = false)
{
    DarlingResult result;
    result.error = error;
    result.review = review;
    return result;
}

bool readJson(const QString &library, const QString &path, QByteArray *bytes,
              QJsonDocument *document, QString *error)
{
    if (!LibraryPersistence::readLibraryFile(library, path, bytes, error)) return false;
    QJsonParseError parseError;
    *document = QJsonDocument::fromJson(*bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        *error = QStringLiteral("%1 is invalid JSON.").arg(path);
        return false;
    }
    return true;
}

int uniqueAnchorPosition(const QString &text, const QString &prefix,
                         const QString &suffix)
{
    if (prefix.isEmpty() && suffix.isEmpty()) return -1;
    const QString context = prefix + suffix;
    const int found = text.indexOf(context);
    if (found < 0 || text.indexOf(context, found + 1) >= 0) return -1;
    return found + prefix.size();
}

} // namespace

DarlingRecords::DarlingRecords(QString libraryPath, QString bookId)
    : libraryPath_(std::move(libraryPath)), bookId_(std::move(bookId)) {}

bool DarlingRecords::list(QJsonArray *records, QString *error) const
{
    QByteArray bytes;
    QJsonDocument document;
    const QString path = bookId_ + QStringLiteral("/darlings.json");
    if (!readJson(libraryPath_, path, &bytes, &document, error)) return false;
    if (!document.isArray()) {
        *error = QStringLiteral("%1 is not a JSON array.").arg(path);
        return false;
    }
    QSet<QString> ids;
    for (const QJsonValue &value : document.array()) {
        const QJsonObject object = value.toObject();
        const QString id = object.value(QStringLiteral("id")).toString();
        const QJsonValue chapter = object.value(QStringLiteral("chapterId"));
        const QJsonValue html = object.value(QStringLiteral("html"));
        const QJsonValue text = object.value(QStringLiteral("text"));
        const QJsonValue prefix = object.value(QStringLiteral("anchorPrefix"));
        const QJsonValue suffix = object.value(QStringLiteral("anchorSuffix"));
        const auto optionalString = [](const QJsonValue &field) {
            return field.isUndefined() || field.isNull() || field.isString();
        };
        if (!value.isObject() || id.isEmpty() || ids.contains(id) ||
            !optionalString(chapter) || !optionalString(html) || !optionalString(text) ||
            !optionalString(prefix) || !optionalString(suffix)) {
            *error = QStringLiteral("Darlings contain an invalid or duplicate record.");
            return false;
        }
        ids.insert(id);
    }
    *records = document.array();
    return true;
}

DarlingResult DarlingRecords::cut(const QString &chapterId, int start, int end) const
{
    QJsonArray records;
    QString error;
    if (!list(&records, &error)) return failure(error);
    const QString path = chapterPath(bookId_, chapterId);
    QByteArray chapterBytes, recordBytes;
    if (!LibraryPersistence::readLibraryFile(libraryPath_, path, &chapterBytes, &error) ||
        !LibraryPersistence::readLibraryFile(libraryPath_, bookId_ + QStringLiteral("/darlings.json"),
                                             &recordBytes, &error)) return failure(error);
    const LegacyChapterDocument source = LegacyChapterCodec::decode(
        chapterBytes, loadChapterLinks(libraryPath_, bookId_, chapterId));
    if (!source.editable() || source.hasProtectedContent())
        return failure(QStringLiteral("This chapter cannot be cut safely."));
    if (start < 0 || end > source.text.size() || start >= end)
        return failure(QStringLiteral("Select prose to save as a Darling."));

    QTextDocument document;
    document.setPlainText(source.text);
    LegacyChapterCodec::applyFormatting(source, &document);
    QTextCursor cursor(&document);
    cursor.setPosition(start);
    cursor.setPosition(end, QTextCursor::KeepAnchor);
    const QString selected = cursor.selectedText().replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    if (selected.trimmed().isEmpty()) return failure(QStringLiteral("Select prose to save as a Darling."));
    QTextDocument fragmentDocument;
    QTextCursor fragmentCursor(&fragmentDocument);
    fragmentCursor.insertFragment(cursor.selection());
    const QByteArray html = LegacyChapterCodec::encodeRich({}, &fragmentDocument, &error);
    if (!error.isEmpty()) return failure(error);
    cursor.removeSelectedText();
    const int position = cursor.position();
    const QString remaining = document.toPlainText();
    if (remaining != source.text.left(start) + source.text.mid(end))
        return failure(QStringLiteral("Removing this selection would change surrounding text."));
    const QByteArray updatedChapter = LegacyChapterCodec::encodeRich(source, &document, &error);
    if (!error.isEmpty()) return failure(error);
    if (LegacyChapterCodec::decode(updatedChapter,
            loadChapterLinks(libraryPath_, bookId_, chapterId)).text != remaining)
        return failure(QStringLiteral("The saved chapter would change surrounding text."));

    QByteArray bookBytes;
    QJsonDocument book;
    if (!readJson(libraryPath_, bookId_ + QStringLiteral("/book.json"),
                  &bookBytes, &book, &error) || !book.isObject())
        return failure(error.isEmpty() ? QStringLiteral("Book metadata is invalid.") : error);
    const QString chapterLabel = book.object().value(QStringLiteral("chapterTitles"))
        .toObject().value(chapterId).toString(chapterId);
    QJsonObject record;
    record.insert(QStringLiteral("id"), QStringLiteral("d-") + QUuid::createUuid().toString(QUuid::WithoutBraces));
    record.insert(QStringLiteral("html"), QString::fromUtf8(html));
    record.insert(QStringLiteral("text"), selected);
    record.insert(QStringLiteral("chapterId"), chapterId);
    record.insert(QStringLiteral("chapterLabel"), chapterLabel);
    record.insert(QStringLiteral("textComplete"), true);
    record.insert(QStringLiteral("anchorPrefix"), remaining.mid(qMax(0, position - 60), qMin(60, position)));
    record.insert(QStringLiteral("anchorSuffix"), remaining.mid(position, 60));
    record.insert(QStringLiteral("date"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    records.prepend(record);
    const auto saved = LibraryPersistence::saveFiles(libraryPath_, {
        {path, LibraryPersistence::hash(chapterBytes), false, updatedChapter},
        {bookId_ + QStringLiteral("/darlings.json"), LibraryPersistence::hash(recordBytes),
         false, QJsonDocument(records).toJson(QJsonDocument::Indented)}});
    if (!saved.ok) return failure(saved.error);
    DarlingResult result;
    result.ok = true;
    result.chapterId = chapterId;
    result.position = position;
    return result;
}

DarlingResult DarlingRecords::restore(const QString &id, bool allowFallback) const
{
    QJsonArray records;
    QString error;
    if (!list(&records, &error)) return failure(error);
    int recordIndex = -1;
    QJsonObject record;
    for (int i = 0; i < records.size(); ++i) {
        if (records.at(i).toObject().value(QStringLiteral("id")).toString() == id) {
            recordIndex = i;
            record = records.at(i).toObject();
            break;
        }
    }
    if (recordIndex < 0) return failure(QStringLiteral("This Darling is no longer available."));
    const QString html = record.value(QStringLiteral("html")).toString();
    const QString text = record.value(QStringLiteral("text")).toString();
    if (html.isEmpty() && text.isEmpty()) return failure(QStringLiteral("This Darling has no restorable content."));
    QString insertedText = text;
    const bool legacyDeleted = !record.value(QStringLiteral("textComplete")).toBool() &&
        record.value(QStringLiteral("chapterId")).toString().isEmpty() && !html.isEmpty();
    if (legacyDeleted) {
        QTextDocument htmlDocument;
        htmlDocument.setHtml(html);
        insertedText = htmlDocument.toPlainText();
        if (insertedText.isEmpty() || !insertedText.startsWith(text))
            return failure(QStringLiteral("The legacy Darling HTML and text disagree; it was kept."));
    }

    QByteArray bookBytes, recordBytes;
    QJsonDocument book;
    if (!readJson(libraryPath_, bookId_ + QStringLiteral("/book.json"), &bookBytes, &book, &error) ||
        !LibraryPersistence::readLibraryFile(libraryPath_, bookId_ + QStringLiteral("/darlings.json"),
                                             &recordBytes, &error)) return failure(error);
    if (!book.isObject() || !book.object().value(QStringLiteral("chapterOrder")).isArray())
        return failure(QStringLiteral("Book chapter order cannot be verified."));
    const QJsonArray order = book.object().value(QStringLiteral("chapterOrder")).toArray();
    const QString originalChapter = record.value(QStringLiteral("chapterId")).toString();
    QString destination;
    for (const QJsonValue &value : order) {
        if (value.toString() == originalChapter) destination = originalChapter;
    }
    if (destination.isEmpty()) {
        if (!allowFallback) return failure(QStringLiteral("The original chapter is gone. Review before restoring to the last chapter."), true);
        if (order.isEmpty()) return failure(QStringLiteral("Create a chapter before restoring this Darling."), true);
        destination = order.last().toString();
    }
    const QString path = chapterPath(bookId_, destination);
    QByteArray chapterBytes;
    if (!LibraryPersistence::readLibraryFile(libraryPath_, path, &chapterBytes, &error)) return failure(error);
    const LegacyChapterDocument source = LegacyChapterCodec::decode(
        chapterBytes, loadChapterLinks(libraryPath_, bookId_, destination));
    if (!source.editable() || source.hasProtectedContent())
        return failure(QStringLiteral("The destination chapter cannot be edited safely."));
    const QString prefix = record.value(QStringLiteral("anchorPrefix")).toString();
    const QString suffix = record.value(QStringLiteral("anchorSuffix")).toString();
    int position = destination == originalChapter
        ? uniqueAnchorPosition(source.text, prefix, suffix) : -1;
    bool fallback = false;
    if (position < 0) {
        if (!allowFallback) return failure(QStringLiteral("The original cut point cannot be verified. Review before restoring to the end of the chapter."), true);
        position = source.text.size();
        fallback = true;
    }
    QTextDocument document;
    document.setPlainText(source.text);
    LegacyChapterCodec::applyFormatting(source, &document);
    QTextCursor cursor(&document);
    cursor.setPosition(position);
    const QString separator = fallback && !source.text.isEmpty() ? QStringLiteral("\n") : QString();
    if (!separator.isEmpty()) cursor.insertBlock();
    if (!html.isEmpty()) {
        cursor.insertFragment(QTextDocumentFragment::fromHtml(html));
    }
    else cursor.insertText(insertedText);
    const QString expectedText = source.text.left(position) + separator + insertedText +
                                 source.text.mid(position);
    const int insertedEnd = position + separator.size() + insertedText.size();
    if (document.toPlainText() == expectedText.left(insertedEnd) +
                                  QLatin1Char('\n') + expectedText.mid(insertedEnd)) {
        QTextCursor extra(&document);
        extra.setPosition(insertedEnd);
        extra.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
        extra.removeSelectedText();
    }
    if (document.toPlainText() != expectedText)
        return failure(QStringLiteral("Restoring this passage would change its text; the Darling was kept."));
    const QByteArray updatedChapter = LegacyChapterCodec::encodeRich(source, &document, &error);
    if (!error.isEmpty()) return failure(error);
    if (LegacyChapterCodec::decode(updatedChapter,
            loadChapterLinks(libraryPath_, bookId_, destination)).text != document.toPlainText())
        return failure(QStringLiteral("The restored chapter would not preserve the passage exactly."));
    records.removeAt(recordIndex);
    const auto saved = LibraryPersistence::saveFiles(libraryPath_, {
        {path, LibraryPersistence::hash(chapterBytes), false, updatedChapter},
        {bookId_ + QStringLiteral("/darlings.json"), LibraryPersistence::hash(recordBytes),
         false, QJsonDocument(records).toJson(QJsonDocument::Indented)}});
    if (!saved.ok) return failure(saved.error);
    DarlingResult result;
    result.ok = true;
    result.chapterId = destination;
    result.position = position;
    return result;
}
