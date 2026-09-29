#pragma once

#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>

class QTextDocument;

struct LegacyTextStyleRun {
    int start = 0;
    int length = 0;
    bool bold = false;
    bool italic = false;
};

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
    bool sceneBreak = false;
    Qt::Alignment alignment = Qt::AlignLeft;
    QVector<LegacyTextStyleRun> styles;
};

struct LegacyChapterRecordLinks {
    QSet<QString> ids;
    QHash<QString, QString> chapterIds;
    QString readError;
};

struct LegacyChapterLinkContext {
    QString chapterId;
    LegacyChapterRecordLinks stickies;
    LegacyChapterRecordLinks darlings;
    QSet<QString> sectionIds;
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
    static void applyFormatting(const LegacyChapterDocument &source,
                                QTextDocument *document);
    static QByteArray encodeRich(const LegacyChapterDocument &source,
                                 const QTextDocument *document,
                                 QString *error);
    static QByteArray encode(const QString &text, bool hasUtf8Bom, QString *error);
};
