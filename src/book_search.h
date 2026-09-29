#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

struct BookSearchHit {
    QString chapterId;
    int position = 0;
    int length = 0;
    bool blocked = false;
};

struct BookSearchResult {
    bool ok = false;
    QString error;
    QVector<BookSearchHit> hits;
    int changed = 0;
};

class BookSearch final {
public:
    BookSearch(QString libraryPath, QString bookId);
    BookSearchResult find(const QString &query) const;
    BookSearchResult replaceAll(const QString &query, const QString &replacement);
    BookSearchResult replaceOne(const QString &query, const QString &replacement,
                                const QString &chapterId, int position);
    BookSearchResult undo();
    bool canUndo() const;

private:
    BookSearchResult replace(const QString &query, const QString &replacement,
                             const QString &chapterId, int position, bool all);
    QString libraryPath_;
    QString bookId_;
    struct UndoFile { QString path; QByteArray before; QByteArray after; };
    QVector<QVector<UndoFile>> undoBatches_;
};
