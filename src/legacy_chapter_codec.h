#pragma once

#include <QByteArray>
#include <QString>

struct LegacyChapterDocument {
    QString text;
    QString refusalReason;
    bool hasUtf8Bom = false;

    bool editable() const { return refusalReason.isEmpty(); }
};

class LegacyChapterCodec final {
public:
    static LegacyChapterDocument decode(const QByteArray &source);
    static QByteArray encode(const QString &text, bool hasUtf8Bom, QString *error);
};
