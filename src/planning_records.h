#pragma once

#include "library_persistence.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

struct PlanningResult {
    bool ok = false;
    bool conflict = false;
    QString error;
    QString id;
};

// Changes to manuscript markers and their records are saved as one transaction.
class PlanningRecords final {
public:
    PlanningRecords(QString libraryPath, QString bookId);

    PlanningResult addSticky(const QString &chapterId, const QString &note,
                             int afterBlock = -1);
    PlanningResult addSection(const QString &chapterId, const QString &note);
    PlanningResult updateSticky(const QString &id, const QString &note);
    PlanningResult updateSection(const QString &chapterId, const QString &id,
                                 const QString &note);
    PlanningResult setChapterNote(const QString &chapterId, const QString &note);
    PlanningResult transferSticky(const QString &id, const QString &destinationChapterId,
                                  bool copy);
    PlanningResult transferSection(const QString &chapterId, const QString &id,
                                   const QString &destinationChapterId, bool copy);
    PlanningResult undo();

private:
    struct FileState {
        QString path;
        QByteArray before;
        QByteArray after;
    };
    PlanningResult save(const QVector<FileState> &files, const QString &id = {});
    bool read(const QString &path, QByteArray *bytes, QString *error) const;
    bool readBook(QByteArray *bytes, QJsonObject *book, QString *error) const;
    bool readStickies(QByteArray *bytes, QJsonArray *records, QString *error) const;
    bool hasChapter(const QJsonObject &book, const QString &chapterId) const;
    QString chapterPath(const QString &chapterId) const;

    QString libraryPath_;
    QString bookId_;
    QVector<QVector<FileState>> history_;
};
