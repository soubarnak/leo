#pragma once

#include <QLibrary>
#include <QSet>
#include <QStringList>

struct MisspelledWord { QString word; int position = 0; int length = 0; };

class Spellcheck final {
public:
    explicit Spellcheck(QString libraryPath);
    ~Spellcheck();
    bool ready() const;
    QString error() const;
    QVector<MisspelledWord> check(const QString &text) const;
    QStringList suggestions(const QString &word) const;
    bool learn(const QString &word, QString *error);

private:
    using Create = void *(*)(const char *, const char *);
    using Destroy = void (*)(void *);
    using Spell = int (*)(void *, const char *);
    using Suggest = int (*)(void *, char ***, const char *);
    using FreeList = void (*)(void *, char ***, int);
    QString libraryPath_;
    QLibrary library_;
    void *handle_ = nullptr;
    Destroy destroy_ = nullptr;
    Spell spell_ = nullptr;
    Suggest suggest_ = nullptr;
    FreeList freeList_ = nullptr;
    QSet<QString> customWords_;
    QString error_;
};
