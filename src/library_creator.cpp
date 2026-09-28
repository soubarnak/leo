#include "library_creator.h"

#include "font_preferences.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QUuid>

namespace {

QString newId(const QString &prefix)
{
    return prefix + QUuid::createUuid().toString(QUuid::Id128);
}

bool writeFile(const QString &root, const QString &relativePath,
               const QByteArray &bytes, QString *error)
{
    const QString path = QDir(root).filePath(relativePath);
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

QByteArray jsonBytes(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Indented);
}

bool validDropCap(const QString &style)
{
    return style == QStringLiteral("literary") || style == QStringLiteral("fantasy") ||
           style == QStringLiteral("scifi");
}

}

NewLibraryResult LibraryCreator::create(const NewLibraryOptions &options)
{
    NewLibraryResult result;
    if (options.path.trimmed().isEmpty()) {
        result.error = QStringLiteral("Choose a location for the new Library.");
        return result;
    }

    const QString targetPath = QDir::cleanPath(QFileInfo(options.path).absoluteFilePath());
    const QFileInfo targetInfo(targetPath);
    if (targetInfo.exists() || targetInfo.isSymLink()) {
        result.error = QStringLiteral(
            "That location already exists. Open an existing Library or choose a new location.");
        return result;
    }
    if (targetInfo.fileName().isEmpty()) {
        result.error = QStringLiteral("Choose a folder name for the new Library.");
        return result;
    }

    const QFileInfo parentInfo(targetInfo.absolutePath());
    if (!parentInfo.isDir() || !parentInfo.isWritable()) {
        result.error = QStringLiteral("The parent folder is missing or not writable: %1")
                           .arg(parentInfo.absoluteFilePath());
        return result;
    }

    QString writingStyle = options.writingStyle;
    if (writingStyle != QStringLiteral("pantser") &&
        writingStyle != QStringLiteral("plotter")) {
        writingStyle = QStringLiteral("pantser");
        result.usedPreferenceFallback = true;
    }

    QString bodyFont = options.bodyFont.trimmed();
    if (bodyFont.compare(QStringLiteral("Serif"), Qt::CaseInsensitive) == 0 ||
        FontPreferences::installedFamily({bodyFont}).isEmpty()) {
        bodyFont = FontPreferences::systemSerifFamily();
        result.usedPreferenceFallback = true;
    }

    QString dropCapStyle = options.dropCapStyle;
    if (!validDropCap(dropCapStyle)) {
        dropCapStyle = QStringLiteral("literary");
        result.usedPreferenceFallback = true;
    }

    QTemporaryDir staging(QDir(parentInfo.absoluteFilePath())
                              .filePath(QStringLiteral(".leo-new-library-XXXXXX")));
    if (!staging.isValid()) {
        result.error = QStringLiteral("Could not prepare a new Library beside %1.")
                           .arg(targetPath);
        return result;
    }

    const QString authorId = newId(QStringLiteral("author-"));
    const QString shelfId = newId(QStringLiteral("shelf-"));
    const QString bookId = newId(QStringLiteral("book-"));
    const QString chapterId = writingStyle == QStringLiteral("pantser")
                                  ? newId(QStringLiteral("chapter-"))
                                  : QString();
    const QString authorName = options.authorName.trimmed().isEmpty()
                                   ? QStringLiteral("Anonymous")
                                   : options.authorName.trimmed();
    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);

    QJsonArray authors;
    authors.append(QJsonObject{
        {QStringLiteral("id"), authorId},
        {QStringLiteral("name"), authorName}});

    QJsonArray bookIds;
    bookIds.append(bookId);
    QJsonArray shelves;
    shelves.append(QJsonObject{
        {QStringLiteral("id"), shelfId},
        {QStringLiteral("name"), QStringLiteral("Works in Progress")},
        {QStringLiteral("authorId"), authorId},
        {QStringLiteral("bookIds"), bookIds}});

    QJsonObject chapterTitles;
    QJsonArray chapterOrder;
    if (!chapterId.isEmpty()) {
        chapterOrder.append(chapterId);
        chapterTitles.insert(chapterId, QStringLiteral("Chapter 1"));
    }

    const QJsonObject libraryObject{
        {QStringLiteral("authors"), authors},
        {QStringLiteral("authorName"), authorName},
        {QStringLiteral("penNames"), QJsonArray{}},
        {QStringLiteral("currentAuthorId"), authorId},
        {QStringLiteral("firstRunDone"), true},
        {QStringLiteral("initialBookId"), bookId},
        {QStringLiteral("writingStyle"), writingStyle},
        {QStringLiteral("fonts"), QJsonObject{
             {QStringLiteral("body"), bodyFont},
             {QStringLiteral("dropcap"), dropCapStyle}}},
        {QStringLiteral("pageTheme"), QStringLiteral("night")},
        {QStringLiteral("shelves"), shelves}};

    const QJsonObject bookObject{
        {QStringLiteral("id"), bookId},
        {QStringLiteral("title"), QStringLiteral("Untitled")},
        {QStringLiteral("subtitle"), QString()},
        {QStringLiteral("series"), QString()},
        {QStringLiteral("author"), authorName},
        {QStringLiteral("wordGoal"), 0},
        {QStringLiteral("created"), now},
        {QStringLiteral("modified"), now},
        {QStringLiteral("chapterOrder"), chapterOrder},
        {QStringLiteral("chapterTitles"), chapterTitles},
        {QStringLiteral("tabNames"), QJsonObject{
             {QStringLiteral("notes"), QStringLiteral("Notes")},
             {QStringLiteral("outline"), QStringLiteral("Outline")}}}};

    const QString bookDirectory = bookId + QLatin1Char('/');
    if (!writeFile(staging.path(), QStringLiteral("library.json"),
                   jsonBytes(libraryObject), &result.error) ||
        !writeFile(staging.path(), bookDirectory + QStringLiteral("book.json"),
                   jsonBytes(bookObject), &result.error) ||
        !writeFile(staging.path(), bookDirectory + QStringLiteral("notes.html"),
                   QByteArrayLiteral(""), &result.error) ||
        !writeFile(staging.path(), bookDirectory + QStringLiteral("outline.html"),
                   QByteArrayLiteral(""), &result.error) ||
        !writeFile(staging.path(), bookDirectory + QStringLiteral("darlings.json"),
                   QByteArrayLiteral("[]\n"), &result.error) ||
        !writeFile(staging.path(), bookDirectory + QStringLiteral("stickies.json"),
                   QByteArrayLiteral("[]\n"), &result.error)) {
        return result;
    }

    if (!chapterId.isEmpty() &&
        !writeFile(staging.path(), bookDirectory + QStringLiteral("chapters/") +
                       chapterId + QStringLiteral(".html"),
                   QByteArrayLiteral("<p><br></p>\n"), &result.error)) {
        return result;
    }

    const QString stagingName = QFileInfo(staging.path()).fileName();
    if (!QDir(parentInfo.absoluteFilePath()).rename(stagingName, targetInfo.fileName())) {
        result.error = QStringLiteral(
            "Could not finish creating the Library. Existing files were left unchanged.");
        return result;
    }

    result.path = targetPath;
    result.ok = true;
    return result;
}
