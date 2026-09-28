#include "legacy_chapter_codec.h"

#include <QStringList>

namespace {

LegacyChapterDocument refused(const QString &reason)
{
    LegacyChapterDocument document;
    document.refusalReason = reason;
    return document;
}

bool decodeEntity(const QString &entity, QString *decoded)
{
    if (entity == QStringLiteral("amp")) {
        *decoded = QStringLiteral("&");
        return true;
    }
    if (entity == QStringLiteral("lt")) {
        *decoded = QStringLiteral("<");
        return true;
    }
    if (entity == QStringLiteral("gt")) {
        *decoded = QStringLiteral(">");
        return true;
    }
    if (entity == QStringLiteral("quot")) {
        *decoded = QStringLiteral("\"");
        return true;
    }
    if (entity == QStringLiteral("apos")) {
        *decoded = QStringLiteral("'");
        return true;
    }
    if (entity == QStringLiteral("nbsp")) {
        *decoded = QString(QChar(0x00a0));
        return true;
    }

    if (!entity.startsWith(QLatin1Char('#'))) {
        return false;
    }

    bool hexadecimal = entity.size() > 2 &&
                       (entity.at(1) == QLatin1Char('x') ||
                        entity.at(1) == QLatin1Char('X'));
    const QString digits = entity.mid(hexadecimal ? 2 : 1);
    bool ok = false;
    const uint codepoint = digits.toUInt(&ok, hexadecimal ? 16 : 10);
    if (!ok || codepoint == 0 || codepoint > 0x10ffff ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
        return false;
    }

    if (codepoint <= 0xffff) {
        *decoded = QString(QChar(static_cast<ushort>(codepoint)));
    } else {
        const char32_t scalar[] = {static_cast<char32_t>(codepoint)};
        *decoded = QString::fromUcs4(scalar, 1);
    }
    return true;
}

bool decodePlainText(const QString &source, QString *text, QString *error)
{
    text->clear();
    for (qsizetype index = 0; index < source.size();) {
        const QChar current = source.at(index);
        if (current == QLatin1Char('<')) {
            *error = QStringLiteral("This chapter contains markup inside a prose paragraph.");
            return false;
        }
        if (current == QLatin1Char('\n') || current == QLatin1Char('\r')) {
            *error = QStringLiteral("This chapter contains a line break inside a prose paragraph.");
            return false;
        }
        if (current != QLatin1Char('&')) {
            text->append(current);
            ++index;
            continue;
        }

        const qsizetype end = source.indexOf(QLatin1Char(';'), index + 1);
        if (end < 0) {
            *error = QStringLiteral("This chapter contains an incomplete HTML character reference.");
            return false;
        }
        QString decoded;
        if (!decodeEntity(source.mid(index + 1, end - index - 1), &decoded)) {
            *error = QStringLiteral("This chapter contains an unsupported HTML character reference.");
            return false;
        }
        if (decoded.contains(QLatin1Char('\n')) || decoded.contains(QLatin1Char('\r'))) {
            *error = QStringLiteral("This chapter contains a line break inside a prose paragraph.");
            return false;
        }
        text->append(decoded);
        index = end + 1;
    }
    return true;
}

QString escapeHtmlText(const QString &text)
{
    QString escaped;
    escaped.reserve(text.size());
    for (const QChar character : text) {
        switch (character.unicode()) {
        case '&':
            escaped.append(QStringLiteral("&amp;"));
            break;
        case '<':
            escaped.append(QStringLiteral("&lt;"));
            break;
        case '>':
            escaped.append(QStringLiteral("&gt;"));
            break;
        default:
            escaped.append(character);
            break;
        }
    }
    return escaped;
}

}

LegacyChapterDocument LegacyChapterCodec::decode(const QByteArray &source)
{
    LegacyChapterDocument document;
    document.hasUtf8Bom = source.startsWith(QByteArray::fromHex("efbbbf"));
    const QByteArray utf8 = document.hasUtf8Bom ? source.mid(3) : source;
    const QString html = QString::fromUtf8(utf8);
    if (html.toUtf8() != utf8) {
        return refused(QStringLiteral("This chapter is not valid UTF-8. It is shown as source text."));
    }
    if (html.isEmpty()) {
        return document;
    }

    QStringList paragraphs;
    qsizetype offset = 0;
    while (offset < html.size()) {
        while (offset < html.size() && html.at(offset).isSpace()) {
            ++offset;
        }
        if (offset == html.size()) {
            break;
        }
        if (!html.mid(offset).startsWith(QStringLiteral("<p>"))) {
            return refused(QStringLiteral(
                "This chapter uses markup that the plain-prose editor cannot preserve."));
        }

        const qsizetype bodyStart = offset + 3;
        const qsizetype bodyEnd = html.indexOf(QStringLiteral("</p>"), bodyStart);
        if (bodyEnd < 0) {
            return refused(QStringLiteral("This chapter has an incomplete paragraph tag."));
        }
        const QString body = html.mid(bodyStart, bodyEnd - bodyStart);
        if (body == QStringLiteral("<br>") || body == QStringLiteral("<br/>") ||
            body == QStringLiteral("<br />")) {
            paragraphs.append(QString());
        } else {
            QString paragraph;
            QString error;
            if (!decodePlainText(body, &paragraph, &error)) {
                return refused(error + QStringLiteral(" It is shown as source text."));
            }
            paragraphs.append(paragraph);
        }
        offset = bodyEnd + 4;
    }

    if (paragraphs.isEmpty() && !html.trimmed().isEmpty()) {
        return refused(QStringLiteral(
            "This chapter uses markup that the plain-prose editor cannot preserve."));
    }
    document.text = paragraphs.join(QLatin1Char('\n'));
    return document;
}

QByteArray LegacyChapterCodec::encode(const QString &text, bool hasUtf8Bom, QString *error)
{
    if (error) {
        error->clear();
    }
    if (text.contains(QChar::Null)) {
        if (error) {
            *error = QStringLiteral("A chapter cannot contain a null character.");
        }
        return {};
    }

    const QStringList paragraphs = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    QByteArray html;
    if (hasUtf8Bom) {
        html.append(QByteArray::fromHex("efbbbf"));
    }
    for (const QString &paragraph : paragraphs) {
        if (paragraph.isEmpty()) {
            html.append("<p><br></p>");
        } else {
            html.append("<p>");
            html.append(escapeHtmlText(paragraph).toUtf8());
            html.append("</p>");
        }
    }
    return html;
}
