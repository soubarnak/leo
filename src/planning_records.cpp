#include "planning_records.h"
#include "chapter_links.h"
#include "legacy_chapter_codec.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QUuid>

#include <utility>

namespace {

QByteArray jsonBytes(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Indented);
}

QByteArray jsonBytes(const QJsonArray &array)
{
    return QJsonDocument(array).toJson(QJsonDocument::Indented);
}

QString escaped(const QString &text)
{
    QString result = text.toHtmlEscaped();
    return result.replace(QLatin1Char('\n'), QStringLiteral(" "));
}

QString newId(const QString &prefix)
{
    return prefix + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

PlanningResult fail(const QString &message)
{
    PlanningResult result;
    result.error = message;
    return result;
}

// Only the exact marker element is removed. Surrounding prose stays byte-for-byte intact.
bool extractMarker(QString *html, const QString &id, QString *marker)
{
    const QRegularExpression expression(
        QStringLiteral("<span\\b(?=[^>]*\\bclass=\\\"ph-mark\\\")(?=[^>]*\\bdata-sid=\\\"") +
        QRegularExpression::escape(id) + QStringLiteral("\\\")[^>]*>.*?</span>"),
        QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch match = expression.match(*html);
    if (!match.hasMatch()) return false;
    *marker = match.captured();
    html->remove(match.capturedStart(), match.capturedLength());
    return true;
}

bool extractParagraph(QString *html, const QString &attribute, const QString &id,
                      QString *paragraph)
{
    const QRegularExpression expression(
        QStringLiteral("<p\\b(?=[^>]*\\b") + attribute + QStringLiteral("=\\\"") +
        QRegularExpression::escape(id) + QStringLiteral("\\\")[^>]*>.*?</p>"),
        QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch match = expression.match(*html);
    if (!match.hasMatch()) return false;
    *paragraph = match.captured();
    html->remove(match.capturedStart(), match.capturedLength());
    return true;
}

} // namespace

PlanningRecords::PlanningRecords(QString libraryPath, QString bookId)
    : libraryPath_(std::move(libraryPath)), bookId_(std::move(bookId))
{
}

QString PlanningRecords::chapterPath(const QString &chapterId) const
{
    return bookId_ + QStringLiteral("/chapters/") + chapterId + QStringLiteral(".html");
}

bool PlanningRecords::read(const QString &path, QByteArray *bytes, QString *error) const
{
    return LibraryPersistence::readLibraryFile(libraryPath_, path, bytes, error);
}

bool PlanningRecords::readBook(QByteArray *bytes, QJsonObject *book, QString *error) const
{
    if (!read(bookId_ + QStringLiteral("/book.json"), bytes, error)) return false;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(*bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        *error = QStringLiteral("book.json is invalid.");
        return false;
    }
    *book = document.object();
    return true;
}

bool PlanningRecords::readStickies(QByteArray *bytes, QJsonArray *records, QString *error) const
{
    if (!read(bookId_ + QStringLiteral("/stickies.json"), bytes, error)) return false;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(*bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
        *error = QStringLiteral("stickies.json is invalid.");
        return false;
    }
    *records = document.array();
    return true;
}

bool PlanningRecords::hasChapter(const QJsonObject &book, const QString &chapterId) const
{
    for (const QJsonValue &value : book.value(QStringLiteral("chapterOrder")).toArray()) {
        if (value.toString() == chapterId) return true;
    }
    return false;
}

PlanningResult PlanningRecords::save(const QVector<FileState> &files, const QString &id)
{
    QByteArray futureBook;
    QByteArray futureStickies;
    for (const FileState &file : files) {
        if (file.path == bookId_ + QStringLiteral("/book.json")) futureBook = file.after;
        if (file.path == bookId_ + QStringLiteral("/stickies.json")) futureStickies = file.after;
    }
    for (const FileState &file : files) {
        if (!file.path.startsWith(bookId_ + QStringLiteral("/chapters/")) ||
            !file.path.endsWith(QStringLiteral(".html"))) continue;
        const QString chapterId = QFileInfo(file.path).completeBaseName();
        LegacyChapterLinkContext links = loadChapterLinks(libraryPath_, bookId_, chapterId);
        const LegacyChapterDocument before = LegacyChapterCodec::decode(file.before, links);
        if (!before.refusalReason.isEmpty()) return fail(before.refusalReason);
        if (!futureStickies.isEmpty()) {
            links.stickies = {};
            for (const QJsonValue &value : QJsonDocument::fromJson(futureStickies).array()) {
                const QJsonObject record = value.toObject();
                const QString recordId = record.value(QStringLiteral("id")).toString();
                links.stickies.ids.insert(recordId);
                links.stickies.chapterIds.insert(recordId,
                    record.value(QStringLiteral("chapterId")).toString());
            }
        }
        if (!futureBook.isEmpty()) {
            links.sectionIds.clear();
            const QJsonObject book = QJsonDocument::fromJson(futureBook).object();
            const QJsonArray sections = book.value(QStringLiteral("sectionNotes")).toObject()
                                           .value(chapterId).toArray();
            for (const QJsonValue &value : sections) {
                links.sectionIds.insert(value.toObject().value(QStringLiteral("id")).toString());
            }
        }
        const LegacyChapterDocument after = LegacyChapterCodec::decode(file.after, links);
        if (!after.refusalReason.isEmpty()) return fail(after.refusalReason);
    }
    QVector<PersistenceFileChange> changes;
    for (const FileState &file : files) {
        changes.append({file.path, LibraryPersistence::hash(file.before), false, file.after});
    }
    const PersistenceResult saved = LibraryPersistence::saveFiles(libraryPath_, changes);
    if (!saved.ok) {
        PlanningResult result = fail(saved.error);
        result.conflict = saved.conflict;
        return result;
    }
    history_.append(files);
    PlanningResult result;
    result.ok = true;
    result.id = id;
    return result;
}

PlanningResult PlanningRecords::addSticky(const QString &chapterId, const QString &note,
                                          int afterBlock)
{
    QByteArray bookBytes, chapterBytes, stickyBytes;
    QJsonObject book;
    QJsonArray records;
    QString error;
    if (!readBook(&bookBytes, &book, &error) ||
        !readStickies(&stickyBytes, &records, &error) ||
        !read(chapterPath(chapterId), &chapterBytes, &error)) return fail(error);
    if (!hasChapter(book, chapterId)) return fail(QStringLiteral("Chapter is not in this book."));
    const QString id = newId(QStringLiteral("s-"));
    QJsonObject record{{QStringLiteral("id"), id}, {QStringLiteral("chapterId"), chapterId},
                       {QStringLiteral("text"), note}, {QStringLiteral("resolved"), false}};
    records.append(record);
    QByteArray after = chapterBytes;
    const QByteArray marker = QStringLiteral(
        "<p><span class=\"ph-mark\" data-sid=\"%1\" contenteditable=\"false\">⚑</span></p>")
                                  .arg(id).toUtf8();
    int insertAt = -1;
    int from = 0;
    for (int block = 0; block <= afterBlock; ++block) {
        const int close = after.indexOf("</p>", from);
        if (close < 0) break;
        insertAt = close + 4;
        from = insertAt;
    }
    if (insertAt < 0) after.append(marker);
    else after.insert(insertAt, marker);
    return save({{chapterPath(chapterId), chapterBytes, after},
                 {bookId_ + QStringLiteral("/stickies.json"), stickyBytes, jsonBytes(records)}}, id);
}

PlanningResult PlanningRecords::addSection(const QString &chapterId, const QString &note)
{
    QByteArray bookBytes, chapterBytes;
    QJsonObject book;
    QString error;
    if (!readBook(&bookBytes, &book, &error) ||
        !read(chapterPath(chapterId), &chapterBytes, &error)) return fail(error);
    if (!hasChapter(book, chapterId)) return fail(QStringLiteral("Chapter is not in this book."));
    const QJsonValue existing = book.value(QStringLiteral("sectionNotes"));
    if (!existing.isUndefined() && !existing.isObject()) return fail(QStringLiteral("sectionNotes is invalid."));
    QJsonObject sections = existing.toObject();
    const QJsonValue chapterSections = sections.value(chapterId);
    if (!chapterSections.isUndefined() && !chapterSections.isArray()) return fail(QStringLiteral("Chapter section notes are invalid."));
    const QString id = newId(QStringLiteral("sec-"));
    QJsonArray list = chapterSections.toArray();
    list.append(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("text"), note}});
    sections.insert(chapterId, list);
    book.insert(QStringLiteral("sectionNotes"), sections);
    QByteArray after = chapterBytes;
    if (!after.isEmpty()) {
        after.append(QStringLiteral("<p class=\"scene-break\" data-sec-brk=\"%1\">***</p>")
                         .arg(id).toUtf8());
    }
    after.append(QStringLiteral("<p class=\"ghost\" data-sec-id=\"%1\">%2</p>")
                     .arg(id, escaped(note)).toUtf8());
    return save({{chapterPath(chapterId), chapterBytes, after},
                 {bookId_ + QStringLiteral("/book.json"), bookBytes, jsonBytes(book)}}, id);
}

PlanningResult PlanningRecords::updateSticky(const QString &id, const QString &note)
{
    QByteArray bytes;
    QJsonArray records;
    QString error;
    if (!readStickies(&bytes, &records, &error)) return fail(error);
    for (int index = 0; index < records.size(); ++index) {
        QJsonObject record = records.at(index).toObject();
        if (record.value(QStringLiteral("id")).toString() != id) continue;
        record.insert(QStringLiteral("text"), note);
        records.replace(index, record);
        return save({{bookId_ + QStringLiteral("/stickies.json"), bytes, jsonBytes(records)}}, id);
    }
    return fail(QStringLiteral("Sticky record was not found."));
}

PlanningResult PlanningRecords::updateSection(const QString &chapterId, const QString &id,
                                               const QString &note)
{
    QByteArray bookBytes, chapterBytes;
    QJsonObject book;
    QString error;
    if (!readBook(&bookBytes, &book, &error) ||
        !read(chapterPath(chapterId), &chapterBytes, &error)) return fail(error);
    QJsonObject sections = book.value(QStringLiteral("sectionNotes")).toObject();
    QJsonArray list = sections.value(chapterId).toArray();
    for (int index = 0; index < list.size(); ++index) {
        QJsonObject section = list.at(index).toObject();
        if (section.value(QStringLiteral("id")).toString() != id) continue;
        section.insert(QStringLiteral("text"), note);
        list.replace(index, section);
        sections.insert(chapterId, list);
        book.insert(QStringLiteral("sectionNotes"), sections);
        const QString original = QString::fromUtf8(chapterBytes);
        QString html = original;
        QString ghost;
        if (extractParagraph(&html, QStringLiteral("data-sec-id"), id, &ghost) &&
            ghost.contains(QStringLiteral("class=\"ghost\""))) {
            const QString replacement = QStringLiteral("<p class=\"ghost\" data-sec-id=\"%1\">%2</p>")
                                            .arg(id, escaped(note));
            html = original;
            html.replace(original.indexOf(ghost), ghost.size(), replacement);
        } else {
            html = original;
        }
        return save({{bookId_ + QStringLiteral("/book.json"), bookBytes, jsonBytes(book)},
                     {chapterPath(chapterId), chapterBytes, html.toUtf8()}}, id);
    }
    return fail(QStringLiteral("Section was not found."));
}

PlanningResult PlanningRecords::setChapterNote(const QString &chapterId, const QString &note)
{
    QByteArray bytes;
    QJsonObject book;
    QString error;
    if (!readBook(&bytes, &book, &error)) return fail(error);
    if (!hasChapter(book, chapterId)) return fail(QStringLiteral("Chapter is not in this book."));
    const QJsonValue value = book.value(QStringLiteral("chapterNotes"));
    if (!value.isUndefined() && !value.isObject()) return fail(QStringLiteral("chapterNotes is invalid."));
    QJsonObject notes = value.toObject();
    notes.insert(chapterId, note);
    book.insert(QStringLiteral("chapterNotes"), notes);
    return save({{bookId_ + QStringLiteral("/book.json"), bytes, jsonBytes(book)}}, chapterId);
}

PlanningResult PlanningRecords::transferSticky(const QString &id,
                                                const QString &destinationChapterId, bool copy)
{
    QByteArray bookBytes, stickyBytes, sourceBytes, destinationBytes;
    QJsonObject book;
    QJsonArray records;
    QString error;
    if (!readBook(&bookBytes, &book, &error) || !readStickies(&stickyBytes, &records, &error))
        return fail(error);
    if (!hasChapter(book, destinationChapterId)) return fail(QStringLiteral("Destination chapter is missing."));
    for (int index = 0; index < records.size(); ++index) {
        QJsonObject record = records.at(index).toObject();
        if (record.value(QStringLiteral("id")).toString() != id) continue;
        const QString sourceChapterId = record.value(QStringLiteral("chapterId")).toString();
        if (!hasChapter(book, sourceChapterId)) return fail(QStringLiteral("Sticky owner is missing."));
        if (!read(chapterPath(sourceChapterId), &sourceBytes, &error)) return fail(error);
        QString source = QString::fromUtf8(sourceBytes);
        QString marker;
        if (!extractMarker(&source, id, &marker)) return fail(QStringLiteral("Sticky marker is missing."));
        const QString nextId = copy ? newId(QStringLiteral("s-")) : id;
        if (copy) {
            source = QString::fromUtf8(sourceBytes);
            marker.replace(id, nextId);
            record.insert(QStringLiteral("id"), nextId);
            record.insert(QStringLiteral("chapterId"), destinationChapterId);
            records.append(record);
        } else {
            record.insert(QStringLiteral("chapterId"), destinationChapterId);
            records.replace(index, record);
        }
        if (sourceChapterId == destinationChapterId) {
            source.append(QStringLiteral("<p>%1</p>").arg(marker));
            return save({{chapterPath(sourceChapterId), sourceBytes, source.toUtf8()},
                         {bookId_ + QStringLiteral("/stickies.json"), stickyBytes, jsonBytes(records)}}, nextId);
        }
        if (!read(chapterPath(destinationChapterId), &destinationBytes, &error)) return fail(error);
        QString destination = QString::fromUtf8(destinationBytes);
        destination.append(QStringLiteral("<p>%1</p>").arg(marker));
        return save({{chapterPath(sourceChapterId), sourceBytes, source.toUtf8()},
                     {chapterPath(destinationChapterId), destinationBytes, destination.toUtf8()},
                     {bookId_ + QStringLiteral("/stickies.json"), stickyBytes, jsonBytes(records)}}, nextId);
    }
    return fail(QStringLiteral("Sticky record was not found."));
}

PlanningResult PlanningRecords::transferSection(const QString &chapterId, const QString &id,
                                                 const QString &destinationChapterId, bool copy)
{
    QByteArray bookBytes, sourceBytes, destinationBytes;
    QJsonObject book;
    QString error;
    if (!readBook(&bookBytes, &book, &error)) return fail(error);
    if (!hasChapter(book, chapterId) || !hasChapter(book, destinationChapterId))
        return fail(QStringLiteral("Chapter is missing."));
    QJsonObject sections = book.value(QStringLiteral("sectionNotes")).toObject();
    QJsonArray sourceList = sections.value(chapterId).toArray();
    for (int index = 0; index < sourceList.size(); ++index) {
        QJsonObject section = sourceList.at(index).toObject();
        if (section.value(QStringLiteral("id")).toString() != id) continue;
        if (!read(chapterPath(chapterId), &sourceBytes, &error)) return fail(error);
        QString source = QString::fromUtf8(sourceBytes);
        QString ghost, sceneBreak;
        if (!extractParagraph(&source, QStringLiteral("data-sec-id"), id, &ghost))
            return fail(QStringLiteral("Section paragraph is missing."));
        extractParagraph(&source, QStringLiteral("data-sec-brk"), id, &sceneBreak);
        const QString nextId = copy ? newId(QStringLiteral("sec-")) : id;
        if (copy) {
            source = QString::fromUtf8(sourceBytes);
            ghost.replace(id, nextId);
            sceneBreak.replace(id, nextId);
            section.insert(QStringLiteral("id"), nextId);
        } else {
            sourceList.removeAt(index);
        }
        QJsonArray destinationList = chapterId == destinationChapterId
            ? sourceList : sections.value(destinationChapterId).toArray();
        destinationList.append(section);
        if (chapterId != destinationChapterId) sections.insert(chapterId, sourceList);
        sections.insert(destinationChapterId, destinationList);
        book.insert(QStringLiteral("sectionNotes"), sections);
        if (chapterId == destinationChapterId) {
            if (sceneBreak.isEmpty() && !source.isEmpty()) {
                sceneBreak = QStringLiteral("<p class=\"scene-break\" data-sec-brk=\"%1\">***</p>")
                                 .arg(nextId);
            }
            source.append(sceneBreak + ghost);
            return save({{chapterPath(chapterId), sourceBytes, source.toUtf8()},
                         {bookId_ + QStringLiteral("/book.json"), bookBytes, jsonBytes(book)}}, nextId);
        }
        if (!read(chapterPath(destinationChapterId), &destinationBytes, &error)) return fail(error);
        QString destination = QString::fromUtf8(destinationBytes);
        if (sceneBreak.isEmpty() && !destination.isEmpty()) {
            sceneBreak = QStringLiteral("<p class=\"scene-break\" data-sec-brk=\"%1\">***</p>")
                             .arg(nextId);
        }
        destination.append(sceneBreak + ghost);
        return save({{chapterPath(chapterId), sourceBytes, source.toUtf8()},
                     {chapterPath(destinationChapterId), destinationBytes, destination.toUtf8()},
                     {bookId_ + QStringLiteral("/book.json"), bookBytes, jsonBytes(book)}}, nextId);
    }
    return fail(QStringLiteral("Section record was not found."));
}

PlanningResult PlanningRecords::undo()
{
    if (history_.isEmpty()) return fail(QStringLiteral("There is no planning change to undo."));
    const QVector<FileState> files = history_.last();
    QVector<PersistenceFileChange> changes;
    for (const FileState &file : files) {
        changes.append({file.path, LibraryPersistence::hash(file.after), false, file.before});
    }
    const PersistenceResult result = LibraryPersistence::saveFiles(libraryPath_, changes);
    if (!result.ok) {
        PlanningResult failed = fail(result.error);
        failed.conflict = result.conflict;
        return failed;
    }
    history_.removeLast();
    PlanningResult undone;
    undone.ok = true;
    return undone;
}
