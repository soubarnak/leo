#include "chapter_links.h"
#include "library_persistence.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

namespace {
bool readJsonArray(const QString &libraryPath,
                   const QString &relativePath,
                   QSet<QString> *ids,
                   QHash<QString, QString> *chapterIds,
                   QString *error)
{
    const QString absolutePath = QDir(libraryPath).filePath(relativePath);
    if (!QFileInfo::exists(absolutePath)) {
        *error = QStringLiteral("%1 is missing.").arg(relativePath);
        return false;
    }

    QByteArray bytes;
    QString readError;
    if (!LibraryPersistence::readLibraryFile(libraryPath, relativePath, &bytes, &readError)) {
        *error = QStringLiteral("%1 could not be read: %2").arg(relativePath, readError);
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
        *error = QStringLiteral("%1 is not a valid JSON array.").arg(relativePath);
        return false;
    }

    const QJsonArray records = document.array();
    for (int index = 0; index < records.size(); ++index) {
        if (!records.at(index).isObject()) {
            *error = QStringLiteral("%1 has an invalid record at position %2.")
                         .arg(relativePath)
                         .arg(index + 1);
            return false;
        }
        const QJsonObject record = records.at(index).toObject();
        const QJsonValue idValue = record.value(QStringLiteral("id"));
        if (!idValue.isString() || idValue.toString().isEmpty()) {
            *error = QStringLiteral("%1 has a record without a valid ID.").arg(relativePath);
            return false;
        }
        const QString id = idValue.toString();
        if (ids->contains(id)) {
            *error = QStringLiteral("%1 contains duplicate ID '%2'.").arg(relativePath, id);
            return false;
        }
        ids->insert(id);
        if (chapterIds) {
            const QJsonValue chapterValue = record.value(QStringLiteral("chapterId"));
            if (!chapterValue.isUndefined() && !chapterValue.isNull() &&
                !chapterValue.isString()) {
                *error = QStringLiteral("%1 has an invalid chapterId for '%2'.")
                             .arg(relativePath, id);
                return false;
            }
            if (chapterValue.isString()) {
                chapterIds->insert(id, chapterValue.toString());
            }
        }
    }
    return true;
}

} // namespace

LegacyChapterLinkContext loadChapterLinks(const QString &libraryPath,
                                          const QString &bookId,
                                          const QString &chapterId)
{
    LegacyChapterLinkContext links;
    links.chapterId = chapterId;
    const QString bookDirectory = bookId + QLatin1Char('/');
    readJsonArray(libraryPath, bookDirectory + QStringLiteral("stickies.json"),
                  &links.stickies.ids, &links.stickies.chapterIds, &links.stickies.readError);
    readJsonArray(libraryPath, bookDirectory + QStringLiteral("darlings.json"),
                  &links.darlings.ids, &links.darlings.chapterIds, &links.darlings.readError);

    const QString metadataPath = bookDirectory + QStringLiteral("book.json");
    QByteArray metadataBytes;
    QString readError;
    if (!LibraryPersistence::readLibraryFile(libraryPath, metadataPath,
                                             &metadataBytes, &readError)) {
        links.sectionReadError = QStringLiteral("%1 could not be read: %2")
                                     .arg(metadataPath, readError);
        return links;
    }
    QJsonParseError parseError;
    const QJsonDocument metadataDocument = QJsonDocument::fromJson(metadataBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !metadataDocument.isObject()) {
        links.sectionReadError = QStringLiteral("%1 is not a valid JSON object.")
                                     .arg(metadataPath);
        return links;
    }

    const QJsonValue sectionNotesValue =
        metadataDocument.object().value(QStringLiteral("sectionNotes"));
    if (sectionNotesValue.isUndefined()) {
        return links;
    }
    if (!sectionNotesValue.isObject()) {
        links.sectionReadError = QStringLiteral("%1 has invalid sectionNotes metadata.")
                                     .arg(metadataPath);
        return links;
    }

    const QJsonValue chapterSections =
        sectionNotesValue.toObject().value(chapterId);
    if (chapterSections.isUndefined()) {
        return links;
    }
    if (!chapterSections.isArray()) {
        links.sectionReadError = QStringLiteral("%1 has invalid section notes for chapter '%2'.")
                                     .arg(metadataPath, chapterId);
        return links;
    }
    const QJsonArray sections = chapterSections.toArray();
    for (int index = 0; index < sections.size(); ++index) {
        if (!sections.at(index).isObject()) {
            links.sectionReadError = QStringLiteral(
                "%1 has an invalid section at position %2 for chapter '%3'.")
                                         .arg(metadataPath)
                                         .arg(index + 1)
                                         .arg(chapterId);
            return links;
        }
        const QJsonValue idValue = sections.at(index).toObject().value(QStringLiteral("id"));
        if (!idValue.isString() || idValue.toString().isEmpty()) {
            links.sectionReadError = QStringLiteral(
                "%1 has a section without a valid ID for chapter '%2'.")
                                         .arg(metadataPath, chapterId);
            return links;
        }
        const QString id = idValue.toString();
        if (links.sectionIds.contains(id)) {
            links.sectionReadError = QStringLiteral(
                "%1 contains duplicate section ID '%2' for chapter '%3'.")
                                         .arg(metadataPath, id, chapterId);
            return links;
        }
        links.sectionIds.insert(id);
    }
    return links;
}
