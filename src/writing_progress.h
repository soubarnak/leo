#pragma once

#include <QDate>
#include <QDateTime>
#include <QMap>
#include <QString>

struct WritingDayCount {
    int start = 0;
    int end = 0;
};

struct WritingProgressSnapshot {
    int bookWords = 0;
    int dailyGoal = 0;
    int bookGoal = 0;
    int dayEndsAt = 0;
    QDate day;
    QMap<QDate, WritingDayCount> history;
    int todayWords() const;
};

class WritingProgress {
public:
    explicit WritingProgress(QString libraryPath);
    bool load(const QString &bookId, QString *error);
    bool updateCount(int count, const QDateTime &now, QString *error);
    bool setGoals(int dailyGoal, int bookGoal, int dayEndsAt, QString *error);
    WritingProgressSnapshot snapshot() const;
    static int countWords(const QString &text);
    static QDate writingDay(const QDateTime &now, int dayEndsAt);
    static int countBook(const QString &libraryPath, const QString &bookId,
                         QString *error, const QString &activeChapter = {},
                         const QString &activeText = {});

private:
    bool saveBook(QString *error);
    QString libraryPath_;
    QString bookId_;
    QByteArray bookBytes_;
    QByteArray libraryBytes_;
    WritingProgressSnapshot data_;
};
