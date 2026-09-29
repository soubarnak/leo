#include "spellcheck.h"

#include "library_persistence.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

Spellcheck::Spellcheck(QString libraryPath)
    : libraryPath_(std::move(libraryPath)), library_(QStringLiteral("libhunspell-1.7.so.0"))
{
    if (!library_.load()) {
        error_ = QStringLiteral("Hunspell is unavailable. Install the system Hunspell library.");
        return;
    }
    const auto create = reinterpret_cast<Create>(library_.resolve("Hunspell_create"));
    destroy_ = reinterpret_cast<Destroy>(library_.resolve("Hunspell_destroy"));
    spell_ = reinterpret_cast<Spell>(library_.resolve("Hunspell_spell"));
    suggest_ = reinterpret_cast<Suggest>(library_.resolve("Hunspell_suggest"));
    freeList_ = reinterpret_cast<FreeList>(library_.resolve("Hunspell_free_list"));
    if (!create || !destroy_ || !spell_ || !suggest_ || !freeList_) {
        error_ = QStringLiteral("Hunspell functions are unavailable.");
        return;
    }
    const QString base = QStringLiteral("/usr/share/hunspell/en_US");
    if (!QFile::exists(base + QStringLiteral(".aff")) ||
        !QFile::exists(base + QStringLiteral(".dic"))) {
        error_ = QStringLiteral("The en_US Hunspell dictionary is unavailable.");
        return;
    }
    handle_ = create(QFile::encodeName(base + QStringLiteral(".aff")).constData(),
                     QFile::encodeName(base + QStringLiteral(".dic")).constData());
    if (!handle_) {
        error_ = QStringLiteral("The en_US Hunspell dictionary could not be opened.");
        return;
    }
    QByteArray bytes;
    QString readError;
    if (!LibraryPersistence::readLibraryFile(libraryPath_, QStringLiteral("library.json"),
                                             &bytes, &readError)) {
        error_ = readError;
        return;
    }
    const QJsonArray words = QJsonDocument::fromJson(bytes).object()
                                 .value(QStringLiteral("customWords")).toArray();
    for (const QJsonValue &word : words)
        if (word.isString()) customWords_.insert(word.toString().toCaseFolded());
}

Spellcheck::~Spellcheck() { if (handle_ && destroy_) destroy_(handle_); }
bool Spellcheck::ready() const { return handle_ && error_.isEmpty(); }
QString Spellcheck::error() const { return error_; }

QVector<MisspelledWord> Spellcheck::check(const QString &text) const
{
    QVector<MisspelledWord> words;
    if (!ready()) return words;
    static const QRegularExpression pattern(QStringLiteral("[\\p{L}]+(?:['’][\\p{L}]+)*"));
    auto iterator = pattern.globalMatch(text);
    while (iterator.hasNext()) {
        const auto match = iterator.next();
        const QString word = match.captured();
        if (!customWords_.contains(word.toCaseFolded()) &&
            !spell_(handle_, word.toUtf8().constData()))
            words.append({word, static_cast<int>(match.capturedStart()),
                          static_cast<int>(match.capturedLength())});
    }
    return words;
}

QStringList Spellcheck::suggestions(const QString &word) const
{
    QStringList result;
    if (!ready()) return result;
    char **list = nullptr;
    const int count = suggest_(handle_, &list, word.toUtf8().constData());
    for (int index = 0; index < qMin(count, 6); ++index)
        result.append(QString::fromUtf8(list[index]));
    if (list) freeList_(handle_, &list, count);
    return result;
}

bool Spellcheck::learn(const QString &word, QString *error)
{
    static const QRegularExpression valid(QStringLiteral("^[\\p{L}]+(?:['’][\\p{L}]+)*$"));
    if (!ready() || !valid.match(word).hasMatch()) {
        *error = ready() ? QStringLiteral("Enter one word to add.") : error_;
        return false;
    }
    QByteArray bytes;
    if (!LibraryPersistence::readLibraryFile(libraryPath_, QStringLiteral("library.json"),
                                             &bytes, error)) return false;
    const QJsonDocument json = QJsonDocument::fromJson(bytes);
    if (!json.isObject()) { *error = QStringLiteral("Library metadata is invalid."); return false; }
    QJsonObject metadata = json.object();
    QJsonArray words = metadata.value(QStringLiteral("customWords")).toArray();
    const QString normalized = word.toCaseFolded();
    if (customWords_.contains(normalized)) return true;
    words.append(word);
    metadata.insert(QStringLiteral("customWords"), words);
    const auto saved = LibraryPersistence::saveFile(
        libraryPath_, QStringLiteral("library.json"), LibraryPersistence::hash(bytes),
        QJsonDocument(metadata).toJson(QJsonDocument::Indented));
    if (!saved.ok) { *error = saved.error; return false; }
    customWords_.insert(normalized);
    return true;
}
