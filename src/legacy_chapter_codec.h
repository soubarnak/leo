#pragma once

#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>

enum class LegacyChapterContentKind {
    Supported,
    Protected,
    Refused
};

struct LegacyChapterFragment {
    LegacyChapterContentKind kind = LegacyChapterContentKind::Supported;
    QString text;
    QString token;
    QString sourcePrefix;
    QString openingTag;
    QString closingTag;
    QString rawSource;
};

struct LegacyChapterLinkContext {
    QString chapterId;
    QSet<QString> stickyIds;
    QHash<QString, QString> stickyChapterIds;
    QSet<QString> darlingIds;
    QHash<QString, QString> darlingChapterIds;
    QSet<QString> sectionIds;
    QString stickyReadError;
    QString darlingReadError;
    QString sectionReadError;
};

struct LegacyChapterDocument {
    QString text;
    QString refusalReason;
    QVector<LegacyChapterFragment> fragments;
    QString trailingSource;
    bool hasUtf8Bom = false;
    bool hasProtectedContent() const;
    bool hasEditableProse() const;

    bool editable() const;
};

class LegacyChapterCodec final {
public:
    static LegacyChapterDocument decode(const QByteArray &source,
                                        const LegacyChapterLinkContext &links = {});
    static bool validateEditedText(const LegacyChapterDocument &document,
                                   const QString &text,
                                   QString *error);
    static QByteArray encode(const LegacyChapterDocument &document,
                             const QString &text,
                             QString *error);
    static QByteArray encode(const QString &text, bool hasUtf8Bom, QString *error);
};
