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

// A word-target sprint measured on one book. Completion latches once the target is reached,
// so later deletions do not undo it. Cancelling only ends a running sprint.
class WritingSprint {
public:
    void start(const QString &bookId, int bookWords, int target);
    void cancel();
    void observe(const QString &bookId, int bookWords);
    void reset();
    bool running(const QString &bookId) const;
    bool complete(const QString &bookId) const;
    int target() const;
    // Words gained since start. Negative after deletions below the start point.
    // Frozen at the achieved count once complete.
    int progress(int bookWords) const;

private:
    enum class Phase { Idle, Running, Complete };
    Phase phase_ = Phase::Idle;
    QString bookId_;
    int startWords_ = 0;
    int target_ = 0;
    int achieved_ = 0;
};

class WritingProgress {
public:
    explicit WritingProgress(QString libraryPath);
    bool load(const QString &bookId, QString *error,
              const QDateTime &now = QDateTime::currentDateTime());
    bool updateCount(int count, const QDateTime &now, QString *error);
    bool setGoals(int dailyGoal, int bookGoal, int dayEndsAt, QString *error);
    void refreshDay(const QDateTime &now);
    WritingProgressSnapshot snapshot() const;
    static int countWords(const QString &text);
    static QDate writingDay(const QDateTime &now, int dayEndsAt);
    static int countBook(const QString &libraryPath, const QString &bookId,
                         QString *error, const QString &activeChapter = {},
                         const QString &activeText = {});

private:
    bool saveBook(QString *error);
    bool reloadFile(const QString &relativePath, QByteArray *bytes, QString *error);
    QDate resolveDay(const QDateTime &now, int dayEndsAt) const;
    QString libraryPath_;
    QString bookId_;
    QByteArray bookBytes_;
    QByteArray libraryBytes_;
    WritingProgressSnapshot data_;
};
