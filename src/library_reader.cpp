#include "library_reader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>

namespace {

bool readObject(const QString &path, const QString &description,
                QJsonObject *object, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Cannot read %1: %2").arg(path, file.errorString());
        return false;
    }

    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        *error = QStringLiteral("Cannot read %1: %2").arg(path, file.errorString());
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        *error = QStringLiteral("Invalid JSON in %1 at byte %2: %3")
                     .arg(path)
                     .arg(parseError.offset)
                     .arg(parseError.errorString());
        return false;
    }
    if (!document.isObject()) {
        *error = QStringLiteral("Invalid %1 in %2: expected a JSON object.")
                     .arg(description, path);
        return false;
    }

    *object = document.object();
    return true;
}

bool requiredString(const QJsonObject &object, const QString &key,
                    const QString &path, QString *value, QString *error)
{
    const QJsonValue field = object.value(key);
    if (!field.isString()) {
        *error = QStringLiteral("Invalid metadata in %1: '%2' must be a string.")
                     .arg(path, key);
        return false;
    }
    *value = field.toString();
    return true;
}

bool optionalString(const QJsonObject &object, const QString &key,
                    const QString &path, QString *value, QString *error)
{
    const QJsonValue field = object.value(key);
    if (field.isUndefined()) {
        value->clear();
        return true;
    }
    if (!field.isString()) {
        *error = QStringLiteral("Invalid %1: '%2' must be a string.").arg(path, key);
        return false;
    }
    *value = field.toString();
    return true;
}

bool validPathComponent(const QString &component)
{
    return !component.isEmpty() && component != QStringLiteral(".") &&
           component != QStringLiteral("..") && !component.contains(QLatin1Char('/')) &&
           !component.contains(QLatin1Char('\\')) && !component.contains(QChar::Null);
}

bool loadBook(const QString &root, const QString &id, Book *book, QString *error)
{
    if (!validPathComponent(id)) {
        *error = QStringLiteral("Invalid book ID in Library membership: '%1'.").arg(id);
        return false;
    }

    const QString directory = QDir(root).filePath(id);
    const QFileInfo directoryInfo(directory);
    if (!directoryInfo.isDir()) {
        *error = QStringLiteral("Book folder is missing or unreadable: %1").arg(directory);
        return false;
    }

    const QString metadataPath = QDir(directory).filePath(QStringLiteral("book.json"));
    QJsonObject metadata;
    if (!readObject(metadataPath, QStringLiteral("book metadata"), &metadata, error)) {
        return false;
    }

    QString storedId;
    if (!optionalString(metadata, QStringLiteral("id"), metadataPath, &storedId, error)) {
        return false;
    }
    if (!storedId.isEmpty() && storedId != id) {
        *error = QStringLiteral("Book ID in %1 does not match its folder '%2'.")
                     .arg(metadataPath, id);
        return false;
    }

    Book parsedBook;
    parsedBook.id = id;
    if (!optionalString(metadata, QStringLiteral("title"), metadataPath,
                        &parsedBook.title, error) ||
        !optionalString(metadata, QStringLiteral("author"), metadataPath,
                        &parsedBook.author, error)) {
        return false;
    }
    if (parsedBook.title.isEmpty()) {
        parsedBook.title = QStringLiteral("Untitled");
    }

    const QJsonValue orderValue = metadata.value(QStringLiteral("chapterOrder"));
    if (!orderValue.isArray()) {
        *error = QStringLiteral("Invalid book metadata in %1: 'chapterOrder' must be an array.")
                     .arg(metadataPath);
        return false;
    }

    QJsonObject titles;
    const QJsonValue titlesValue = metadata.value(QStringLiteral("chapterTitles"));
    if (!titlesValue.isUndefined()) {
        if (!titlesValue.isObject()) {
            *error = QStringLiteral("Invalid book metadata in %1: 'chapterTitles' must be an object.")
                         .arg(metadataPath);
            return false;
        }
        titles = titlesValue.toObject();
    }

    const QJsonArray order = orderValue.toArray();
    parsedBook.chapters.reserve(order.size());
    for (int index = 0; index < order.size(); ++index) {
        if (!order.at(index).isString() || order.at(index).toString().isEmpty()) {
            *error = QStringLiteral("Invalid chapter ID at position %1 in %2.")
                         .arg(index + 1)
                         .arg(metadataPath);
            return false;
        }

        const QString chapterId = order.at(index).toString();
        const QJsonValue titleValue = titles.value(chapterId);
        if (!titleValue.isUndefined() && !titleValue.isString()) {
            *error = QStringLiteral("Invalid chapter title for '%1' in %2.")
                         .arg(chapterId, metadataPath);
            return false;
        }
        parsedBook.chapters.append({chapterId, titleValue.toString()});
    }

    *book = parsedBook;
    return true;
}

bool parseAuthors(const QJsonObject &libraryObject, const QString &path,
                  QVector<Author> *authors, QString *error)
{
    const QJsonValue authorsValue = libraryObject.value(QStringLiteral("authors"));
    if (!authorsValue.isUndefined() && !authorsValue.isArray()) {
        *error = QStringLiteral("Invalid Library metadata in %1: 'authors' must be an array.")
                     .arg(path);
        return false;
    }

    QSet<QString> ids;
    if (authorsValue.isArray()) {
        const QJsonArray authorArray = authorsValue.toArray();
        for (int index = 0; index < authorArray.size(); ++index) {
            if (!authorArray.at(index).isObject()) {
                *error = QStringLiteral("Invalid author at position %1 in %2.")
                             .arg(index + 1)
                             .arg(path);
                return false;
            }

            const QJsonObject authorObject = authorArray.at(index).toObject();
            Author author;
            if (!requiredString(authorObject, QStringLiteral("id"), path, &author.id, error) ||
                !requiredString(authorObject, QStringLiteral("name"), path, &author.name, error)) {
                return false;
            }
            if (author.id.isEmpty() || ids.contains(author.id)) {
                *error = QStringLiteral("Invalid or duplicate author ID '%1' in %2.")
                             .arg(author.id, path);
                return false;
            }
            if (author.name.isEmpty()) {
                author.name = QStringLiteral("Anonymous");
            }
            ids.insert(author.id);
            authors->append(author);
        }
    }

    if (!authors->isEmpty()) {
        return true;
    }

    QString legacyName;
    if (!optionalString(libraryObject, QStringLiteral("authorName"), path,
                        &legacyName, error)) {
        return false;
    }
    if (legacyName.isEmpty()) {
        const QJsonValue penNames = libraryObject.value(QStringLiteral("penNames"));
        if (!penNames.isUndefined()) {
            if (!penNames.isArray()) {
                *error = QStringLiteral("Invalid Library metadata in %1: 'penNames' must be an array.")
                             .arg(path);
                return false;
            }
            const QJsonArray names = penNames.toArray();
            if (!names.isEmpty()) {
                if (!names.first().isString()) {
                    *error = QStringLiteral("Invalid legacy author name in %1.").arg(path);
                    return false;
                }
                legacyName = names.first().toString();
            }
        }
    }

    authors->append({QStringLiteral("a1"),
                     legacyName.isEmpty() ? QStringLiteral("Anonymous") : legacyName,
                     {}});
    return true;
}

}

LibraryReadResult LibraryReader::read(const QString &path)
{
    LibraryReadResult result;
    const QFileInfo rootInfo(path);
    if (!rootInfo.exists() || !rootInfo.isDir()) {
        result.error = QStringLiteral("Library folder does not exist: %1").arg(path);
        return result;
    }

    const QString root = rootInfo.absoluteFilePath();
    if (!rootInfo.isReadable()) {
        result.error = QStringLiteral("Cannot read Library folder: %1").arg(root);
        return result;
    }
    const QString libraryPath = QDir(root).filePath(QStringLiteral("library.json"));
    QJsonObject libraryObject;
    if (!readObject(libraryPath, QStringLiteral("Library metadata"), &libraryObject,
                    &result.error)) {
        return result;
    }

    const QJsonValue shelvesValue = libraryObject.value(QStringLiteral("shelves"));
    if (!shelvesValue.isArray()) {
        result.error = QStringLiteral("Invalid Library metadata in %1: 'shelves' must be an array.")
                           .arg(libraryPath);
        return result;
    }

    Library library;
    library.path = root;
    const QString pageTheme = libraryObject.value(QStringLiteral("pageTheme")).toString();
    if (pageTheme == QStringLiteral("paper") || pageTheme == QStringLiteral("night")) {
        library.preferences.pageTheme = pageTheme;
    }
    library.preferences.uiBright = libraryObject.value(QStringLiteral("uiBright")).toBool();
    library.preferences.chromePinned = libraryObject.value(QStringLiteral("chromePinned")).toBool();
    library.preferences.typewriter = libraryObject.value(QStringLiteral("typewriter")).toBool();
    const QJsonValue fontSize = libraryObject.value(QStringLiteral("editorFontSize"));
    if (fontSize.isDouble()) library.preferences.editorFontSize = qBound(14, fontSize.toInt(17), 22);
    const QJsonValue zoom = libraryObject.value(QStringLiteral("pageZoom"));
    if (zoom.isDouble()) library.preferences.pageZoom = qBound(0.75, zoom.toDouble(1.0), 1.6);
    const QJsonValue writingStyle = libraryObject.value(QStringLiteral("writingStyle"));
    if (writingStyle.isString()) {
        if (!writingStyle.toString().isEmpty()) {
            library.preferences.writingStyle = writingStyle.toString();
        } else {
            library.preferences.writingStyleInvalid = true;
        }
    } else if (!writingStyle.isUndefined()) {
        library.preferences.writingStyleInvalid = true;
    }
    const QJsonValue initialBookId = libraryObject.value(QStringLiteral("initialBookId"));
    if (initialBookId.isString()) {
        library.preferences.initialBookId = initialBookId.toString();
    }
    const QJsonValue fontsValue = libraryObject.value(QStringLiteral("fonts"));
    if (fontsValue.isObject()) {
        const QJsonObject fonts = fontsValue.toObject();
        const QJsonValue bodyFont = fonts.value(QStringLiteral("body"));
        if (bodyFont.isString()) {
            library.preferences.bodyFont = bodyFont.toString();
            library.preferences.bodyFontInvalid = bodyFont.toString().isEmpty();
        } else if (!bodyFont.isUndefined()) {
            library.preferences.bodyFontInvalid = true;
        }
        const QJsonValue dropCapStyle = fonts.value(QStringLiteral("dropcap"));
        if (dropCapStyle.isString()) {
            if (!dropCapStyle.toString().isEmpty()) {
                library.preferences.dropCapStyle = dropCapStyle.toString();
            } else {
                library.preferences.dropCapStyleInvalid = true;
            }
        } else if (!dropCapStyle.isUndefined()) {
            library.preferences.dropCapStyleInvalid = true;
        }
    } else if (!fontsValue.isUndefined()) {
        library.preferences.bodyFontInvalid = true;
        library.preferences.dropCapStyleInvalid = true;
    }
    if (!parseAuthors(libraryObject, libraryPath, &library.authors, &result.error)) {
        return result;
    }

    QHash<QString, int> authorPositions;
    for (int index = 0; index < library.authors.size(); ++index) {
        authorPositions.insert(library.authors.at(index).id, index);
    }

    QHash<QString, Book> loadedBooks;
    QSet<QString> memberships;
    const QJsonArray shelves = shelvesValue.toArray();
    for (int shelfIndex = 0; shelfIndex < shelves.size(); ++shelfIndex) {
        if (!shelves.at(shelfIndex).isObject()) {
            result.error = QStringLiteral("Invalid shelf at position %1 in %2.")
                               .arg(shelfIndex + 1)
                               .arg(libraryPath);
            return result;
        }

        const QJsonObject shelfObject = shelves.at(shelfIndex).toObject();
        Shelf shelf;
        if (!requiredString(shelfObject, QStringLiteral("id"), libraryPath,
                            &shelf.id, &result.error) ||
            !requiredString(shelfObject, QStringLiteral("name"), libraryPath,
                            &shelf.name, &result.error)) {
            return result;
        }
        if (shelf.id.isEmpty()) {
            result.error = QStringLiteral("Invalid shelf ID at position %1 in %2.")
                               .arg(shelfIndex + 1)
                               .arg(libraryPath);
            return result;
        }

        QString authorId;
        const QJsonValue authorIdValue = shelfObject.value(QStringLiteral("authorId"));
        if (!authorIdValue.isUndefined()) {
            if (!authorIdValue.isString()) {
                result.error = QStringLiteral("Invalid author ID on shelf '%1' in %2.")
                                   .arg(shelf.name, libraryPath);
                return result;
            }
            authorId = authorIdValue.toString();
        } else {
            authorId = library.authors.first().id;
        }

        const auto authorPosition = authorPositions.constFind(authorId);
        if (authorPosition == authorPositions.cend()) {
            result.error = QStringLiteral("Shelf '%1' refers to unknown author '%2' in %3.")
                               .arg(shelf.name, authorId, libraryPath);
            return result;
        }

        const QJsonValue bookIdsValue = shelfObject.value(QStringLiteral("bookIds"));
        if (!bookIdsValue.isArray()) {
            result.error = QStringLiteral("Invalid shelf '%1' in %2: 'bookIds' must be an array.")
                               .arg(shelf.name, libraryPath);
            return result;
        }
        const QJsonArray bookIds = bookIdsValue.toArray();
        shelf.books.reserve(bookIds.size());
        for (int index = 0; index < bookIds.size(); ++index) {
            if (!bookIds.at(index).isString() || bookIds.at(index).toString().isEmpty()) {
                result.error = QStringLiteral("Invalid book ID at position %1 on shelf '%2'.")
                                   .arg(index + 1)
                                   .arg(shelf.name);
                return result;
            }

            const QString bookId = bookIds.at(index).toString();
            Book book;
            if (loadedBooks.contains(bookId)) {
                book = loadedBooks.value(bookId);
            } else {
                if (!loadBook(root, bookId, &book, &result.error)) {
                    return result;
                }
                loadedBooks.insert(bookId, book);
            }
            shelf.books.append(book);
            memberships.insert(bookId);
        }

        library.authors[*authorPosition].shelves.append(shelf);
    }

    const QDir rootDirectory(root);
    const QFileInfoList entries = rootDirectory.entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
    for (const QFileInfo &entry : entries) {
        const QString id = entry.fileName();
        if (memberships.contains(id)) {
            continue;
        }
        const QString metadataPath = QDir(entry.absoluteFilePath()).filePath(QStringLiteral("book.json"));
        if (!id.startsWith(QStringLiteral("book-")) && !QFileInfo(metadataPath).exists()) {
            continue;
        }

        Book book;
        if (loadedBooks.contains(id)) {
            book = loadedBooks.value(id);
        } else if (!loadBook(root, id, &book, &result.error)) {
            return result;
        }
        library.unfiledBooks.append(book);
    }

    result.library = library;
    return result;
}
