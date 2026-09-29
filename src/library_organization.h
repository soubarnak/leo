#pragma once

#include "library_persistence.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <functional>

struct LibraryOrganizationResult {
    bool ok = false;
    bool conflict = false;
    QString id;
    QString error;
};

using LibraryTrashOperation = std::function<bool(const QString &, QString *)>;

class LibraryOrganization final {
public:
    explicit LibraryOrganization(const QString &libraryPath,
                                 const PersistenceCheckpointHook &checkpoint = {},
                                 const PersistenceIoFailureHook &ioFailure = {});

    bool load(QString *error);
    const QJsonObject &metadata() const;

    LibraryOrganizationResult addAuthor(const QString &name);
    LibraryOrganizationResult renameAuthor(const QString &authorId, const QString &name);
    LibraryOrganizationResult removeAuthor(const QString &authorId,
                                           const QString &targetAuthorId);
    LibraryOrganizationResult setCurrentAuthor(const QString &authorId);

    LibraryOrganizationResult addShelf(const QString &authorId, const QString &name);
    LibraryOrganizationResult renameShelf(const QString &shelfId, const QString &name);
    LibraryOrganizationResult removeShelf(const QString &shelfId);
    LibraryOrganizationResult moveShelf(const QString &shelfId,
                                        const QString &targetAuthorId, int index);
    LibraryOrganizationResult moveBook(const QString &bookId,
                                       const QString &targetShelfId, int index);
    LibraryOrganizationResult removeBookFromShelves(const QString &bookId);

    LibraryOrganizationResult createBook(const QString &shelfId, const QString &title);
    LibraryOrganizationResult renameBook(const QString &bookId, const QString &title);
    LibraryOrganizationResult moveBookToTrash(
        const QString &bookId, const LibraryTrashOperation &trashOperation = {});

private:
    LibraryOrganizationResult commitMetadata(const QJsonObject &updated);
    LibraryOrganizationResult commitBookMetadata(const QString &relativePath,
                                                 const QByteArray &oldBytes,
                                                 const QByteArray &newBytes);

    QString libraryPath_;
    QJsonObject metadata_;
    QByteArray metadataHash_;
    PersistenceCheckpointHook checkpoint_;
    PersistenceIoFailureHook ioFailure_;
    bool loaded_ = false;
};
