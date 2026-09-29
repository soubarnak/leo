#pragma once

#include "legacy_chapter_codec.h"
#include "library_persistence.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

struct ChapterStructureResult {
    bool ok = false;
    bool conflict = false;
    QString error;
    QString chapterId;
};

struct ChapterEditSource {
    QByteArray currentBytes;
    QByteArray expectedHash;
    QByteArray originalBytes;
};

class ChapterStructure final {
public:
    ChapterStructure(QString libraryPath, QString bookId);

    bool load(QString *error);
    ChapterStructureResult addChapter(int index, const QString &title);
    ChapterStructureResult renameChapter(const QString &chapterId,
                                         const QString &title);
    ChapterStructureResult moveChapter(const QString &chapterId, int index);
    ChapterStructureResult splitChapter(const QString &chapterId, int position,
                                        const ChapterEditSource &source,
                                        const QString &newTitle);
    ChapterStructureResult joinChapter(const QString &chapterId, bool withPrevious,
                                       const ChapterEditSource &source);
    ChapterStructureResult deleteChapter(const QString &chapterId,
                                         const ChapterEditSource &source);

    ChapterStructureResult undo();
    ChapterStructureResult redo();
    bool canUndo() const;
    bool canRedo() const;
    void invalidateHistoryForChapterEdit(const QString &relativePath);

private:
    struct PlannedFile {
        PersistenceFileChange change;
        bool beforeExists = true;
        QByteArray beforeBytes;
    };

    struct HistoryFile {
        QString relativePath;
        bool beforeExists = true;
        QByteArray beforeBytes;
        QByteArray afterBytes;
    };

    struct HistoryEntry {
        QVector<HistoryFile> files;
        QString chapterBefore;
        QString chapterAfter;
    };

    ChapterStructureResult commitBookChange(const QJsonObject &updatedBook,
                                            const QVector<PlannedFile> &otherFiles,
                                            const QString &chapterBefore,
                                            const QString &chapterAfter);
    bool currentBook(QJsonObject *book, QString *error) const;
    bool chapterOrder(const QJsonObject &book, QJsonArray *order,
                      QString *error) const;
    bool chapterIndex(const QJsonArray &order, const QString &chapterId,
                      int *index) const;
    bool loadChapterLinks(const QString &chapterId,
                          LegacyChapterLinkContext *links,
                          QString *error) const;
    bool contentOperationSafe(const QJsonObject &book,
                              const QStringList &chapterIds,
                              QString *error, bool allowPlanningRecords = false) const;
    bool readChapter(const QString &chapterId, QByteArray *bytes,
                     QString *relativePath, QString *error) const;
    ChapterStructureResult fail(const QString &error, bool conflict = false) const;

    QString libraryPath_;
    QString bookId_;
    QString bookRelativePath_;
    QByteArray bookBytes_;
    QByteArray bookHash_;
    QVector<HistoryEntry> history_;
    int historyIndex_ = 0;
    bool loaded_ = false;
};
