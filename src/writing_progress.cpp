#include "writing_progress.h"

#include "library_persistence.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTextDocument>

namespace {
bool saveJson(const QString &root, const QString &relative, QByteArray *original,
              const QJsonObject &object, QString *error)
{
    const QByteArray updated = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (updated == *original) return true;
    const PersistenceResult result = LibraryPersistence::saveFile(
        root, relative, LibraryPersistence::hash(*original), updated);
    if (!result.ok) {
        *error = result.error;
        return false;
    }
    *original = updated;
    return true;
}

int nonnegative(const QJsonValue &value)
{
    return value.isDouble() && value.toDouble() >= 0 ? value.toInt() : 0;
}
}

int WritingProgressSnapshot::todayWords() const
{
    const auto entry = history.constFind(day);
    return entry == history.cend() ? 0 : entry->end - entry->start;
}

WritingProgress::WritingProgress(QString libraryPath) : libraryPath_(std::move(libraryPath)) {}

bool WritingProgress::load(const QString &bookId, QString *error, const QDateTime &now)
{
    bookId_ = bookId;
    data_ = {};
    if (!LibraryPersistence::readLibraryFile(libraryPath_, QStringLiteral("library.json"), &libraryBytes_, error) ||
        !LibraryPersistence::readLibraryFile(libraryPath_, bookId + QStringLiteral("/book.json"), &bookBytes_, error)) return false;
    const QJsonDocument library = QJsonDocument::fromJson(libraryBytes_);
    const QJsonDocument book = QJsonDocument::fromJson(bookBytes_);
    if (!library.isObject() || !book.isObject()) {
        *error = QStringLiteral("Progress metadata is invalid JSON.");
        return false;
    }
    data_.dailyGoal = nonnegative(library.object().value(QStringLiteral("dailyGoal")));
    data_.dayEndsAt = qBound(0, library.object().value(QStringLiteral("dayEndsAt")).toInt(), 6);
    data_.bookGoal = nonnegative(book.object().value(QStringLiteral("wordGoal")));
    const QJsonValue storedCount = book.object().value(QStringLiteral("wordCount"));
    if (storedCount.isDouble()) {
        data_.bookWords = nonnegative(storedCount);
    } else {
        data_.bookWords = countBook(libraryPath_, bookId_, error);
        if (data_.bookWords < 0) return false;
    }
    const QJsonObject counts = book.object().value(QStringLiteral("dailyCounts")).toObject();
    for (auto it = counts.begin(); it != counts.end(); ++it) {
        const QDate date = QDate::fromString(it.key(), Qt::ISODate);
        if (date.isValid() && it.value().isObject()) {
            const QJsonObject item = it.value().toObject();
            data_.history.insert(date, {nonnegative(item.value(QStringLiteral("start"))),
                                        nonnegative(item.value(QStringLiteral("end")))});
        }
    }
    data_.day = resolveDay(now, data_.dayEndsAt);
    return true;
}

QDate WritingProgress::writingDay(const QDateTime &now, int dayEndsAt)
{
    const QDateTime local = now.toLocalTime();
    return local.time().hour() < dayEndsAt ? local.date().addDays(-1) : local.date();
}

QDate WritingProgress::resolveDay(const QDateTime &now, int dayEndsAt) const
{
    const QDate day = writingDay(now, dayEndsAt);
    // A clock or timezone change can move the local date backwards. Keep counting on the
    // last recorded day so one writing day never gains a second history entry.
    return data_.history.isEmpty() ? day : qMax(day, data_.history.lastKey());
}

int WritingProgress::countWords(const QString &text)
{
    // QChar::isSpace covers U+00A0 and the U+2029 paragraph separator that a text cursor emits.
    int count = 0;
    bool inWord = false;
    for (const QChar ch : text) {
        if (ch.isSpace()) {
            inWord = false;
        } else if (!inWord) {
            inWord = true;
            ++count;
        }
    }
    return count;
}

int WritingProgress::countBook(const QString &libraryPath, const QString &bookId,
                               QString *error, const QString &activeChapter,
                               const QString &activeText)
{
    QByteArray bytes;
    if (!LibraryPersistence::readLibraryFile(libraryPath, bookId + QStringLiteral("/book.json"), &bytes, error)) return -1;
    const QJsonArray chapters = QJsonDocument::fromJson(bytes).object()
                                    .value(QStringLiteral("chapterOrder")).toArray();
    int total = 0;
    for (const QJsonValue &chapter : chapters) {
        const QString relative = bookId + QStringLiteral("/chapters/") + chapter.toString() +
                                 QStringLiteral(".html");
        if (relative == activeChapter) {
            total += countWords(activeText);
            continue;
        }
        if (!LibraryPersistence::readLibraryFile(libraryPath, relative, &bytes, error)) return -1;
        QTextDocument document;
        document.setHtml(QString::fromUtf8(bytes));
        total += countWords(document.toPlainText());
    }
    return total;
}

bool WritingProgress::updateCount(int count, const QDateTime &now, QString *error)
{
    if (count < 0 || bookId_.isEmpty()) return false;
    const QDate day = resolveDay(now, data_.dayEndsAt);
    data_.day = day;
    if (!data_.history.contains(day))
        data_.history.insert(day, {data_.bookWords, count});
    else data_.history[day].end = count;
    data_.bookWords = count;
    return saveBook(error);
}

bool WritingProgress::setGoals(int dailyGoal, int bookGoal, int dayEndsAt, QString *error)
{
    if (dailyGoal < 0 || bookGoal < 0 || dayEndsAt < 0 || dayEndsAt > 6) {
        *error = QStringLiteral("Goals and writing-day boundary are out of range.");
        return false;
    }
    if (!reloadFile(QStringLiteral("library.json"), &libraryBytes_, error) ||
        !reloadFile(bookId_ + QStringLiteral("/book.json"), &bookBytes_, error)) return false;
    QJsonObject library = QJsonDocument::fromJson(libraryBytes_).object();
    library.insert(QStringLiteral("dailyGoal"), dailyGoal);
    library.insert(QStringLiteral("dayEndsAt"), dayEndsAt);
    QJsonObject book = QJsonDocument::fromJson(bookBytes_).object();
    book.insert(QStringLiteral("wordGoal"), bookGoal);
    const QByteArray newLibrary = QJsonDocument(library).toJson(QJsonDocument::Indented);
    const QByteArray newBook = QJsonDocument(book).toJson(QJsonDocument::Indented);
    QVector<PersistenceFileChange> changes;
    if (newLibrary != libraryBytes_)
        changes.append({QStringLiteral("library.json"),
                        LibraryPersistence::hash(libraryBytes_), false, newLibrary});
    if (newBook != bookBytes_)
        changes.append({bookId_ + QStringLiteral("/book.json"),
                        LibraryPersistence::hash(bookBytes_), false, newBook});
    if (!changes.isEmpty()) {
        const PersistenceResult result = LibraryPersistence::saveFiles(libraryPath_, changes);
        if (!result.ok) {
            *error = result.error;
            return false;
        }
    }
    libraryBytes_ = newLibrary;
    bookBytes_ = newBook;
    data_.dailyGoal = dailyGoal;
    data_.bookGoal = bookGoal;
    data_.dayEndsAt = dayEndsAt;
    data_.day = resolveDay(QDateTime::currentDateTime(), dayEndsAt);
    return true;
}

WritingProgressSnapshot WritingProgress::snapshot() const { return data_; }

void WritingProgress::refreshDay(const QDateTime &now)
{
    data_.day = resolveDay(now, data_.dayEndsAt);
}

void WritingSprint::start(const QString &bookId, int bookWords, int target)
{
    phase_ = Phase::Running;
    bookId_ = bookId;
    startWords_ = bookWords;
    target_ = target;
    achieved_ = 0;
}

void WritingSprint::cancel()
{
    if (phase_ == Phase::Running) phase_ = Phase::Idle;
}

void WritingSprint::observe(const QString &bookId, int bookWords)
{
    if (phase_ != Phase::Running || bookId != bookId_) return;
    if (bookWords - startWords_ >= target_) {
        phase_ = Phase::Complete;
        achieved_ = bookWords - startWords_;
    }
}

void WritingSprint::reset()
{
    *this = WritingSprint();
}

bool WritingSprint::running(const QString &bookId) const
{
    return phase_ == Phase::Running && bookId == bookId_;
}

bool WritingSprint::complete(const QString &bookId) const
{
    return phase_ == Phase::Complete && bookId == bookId_;
}

int WritingSprint::target() const { return target_; }

int WritingSprint::progress(int bookWords) const
{
    return phase_ == Phase::Complete ? achieved_ : bookWords - startWords_;
}

bool WritingProgress::reloadFile(const QString &relativePath, QByteArray *bytes, QString *error)
{
    // Preferences, spellcheck and cover edits rewrite these files outside this object.
    // Merge progress fields into the current bytes so the save hash matches what is on disk.
    QByteArray current;
    if (!LibraryPersistence::readLibraryFile(libraryPath_, relativePath, &current, error)) return false;
    if (!QJsonDocument::fromJson(current).isObject()) {
        *error = QStringLiteral("Progress metadata is invalid JSON.");
        return false;
    }
    *bytes = std::move(current);
    return true;
}

bool WritingProgress::saveBook(QString *error)
{
    if (!reloadFile(bookId_ + QStringLiteral("/book.json"), &bookBytes_, error)) return false;
    QJsonObject book = QJsonDocument::fromJson(bookBytes_).object();
    book.insert(QStringLiteral("wordCount"), data_.bookWords);
    book.insert(QStringLiteral("wordGoal"), data_.bookGoal);
    QJsonObject counts = book.value(QStringLiteral("dailyCounts")).toObject();
    for (auto it = data_.history.cbegin(); it != data_.history.cend(); ++it) {
        QJsonObject entry = counts.value(it.key().toString(Qt::ISODate)).toObject();
        entry.insert(QStringLiteral("start"), it.value().start);
        entry.insert(QStringLiteral("end"), it.value().end);
        counts.insert(it.key().toString(Qt::ISODate), entry);
    }
    book.insert(QStringLiteral("dailyCounts"), counts);
    return saveJson(libraryPath_, bookId_ + QStringLiteral("/book.json"), &bookBytes_, book, error);
}

