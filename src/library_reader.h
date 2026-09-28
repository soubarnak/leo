#pragma once

#include <QString>
#include <QVector>

struct Chapter {
    QString id;
    QString title;
};

struct Book {
    QString id;
    QString title;
    QString author;
    QVector<Chapter> chapters;
};

struct Shelf {
    QString id;
    QString name;
    QVector<Book> books;
};

struct Author {
    QString id;
    QString name;
    QVector<Shelf> shelves;
};

struct Library {
    QString path;
    QVector<Author> authors;
    QVector<Book> unfiledBooks;
};

struct LibraryReadResult {
    Library library;
    QString error;

    bool ok() const { return error.isEmpty(); }
};

class LibraryReader {
public:
    static LibraryReadResult read(const QString &path);
};
