#include "legacy_chapter_codec.h"

#include <QMap>
#include <QRegularExpression>
#include <QStringList>

namespace {

struct HtmlTag {
    QString name;
    QMap<QString, QString> attributes;
    qsizetype end = 0;
    bool closing = false;
    bool selfClosing = false;
    bool special = false;
};

bool readTagEnd(const QString &source, qsizetype start, qsizetype *end)
{
    QChar quote;
    for (qsizetype index = start + 1; index < source.size(); ++index) {
        const QChar current = source.at(index);
        if (!quote.isNull()) {
            if (current == quote) {
                quote = QChar();
            }
        } else if (current == QLatin1Char('\'') || current == QLatin1Char('"')) {
            quote = current;
        } else if (current == QLatin1Char('>')) {
            *end = index + 1;
            return true;
        }
    }
    return false;
}

bool parseTag(const QString &source, qsizetype start, HtmlTag *tag, QString *error)
{
    if (start >= source.size() || source.at(start) != QLatin1Char('<')) {
        *error = QStringLiteral("Expected an HTML tag.");
        return false;
    }

    if (source.mid(start).startsWith(QStringLiteral("<!--"))) {
        const qsizetype commentEnd = source.indexOf(QStringLiteral("-->"), start + 4);
        if (commentEnd < 0) {
            *error = QStringLiteral("This chapter contains an incomplete HTML comment.");
            return false;
        }
        tag->name = QStringLiteral("#comment");
        tag->end = commentEnd + 3;
        tag->special = true;
        return true;
    }

    if (source.mid(start).startsWith(QStringLiteral("<![CDATA["))) {
        const qsizetype cdataEnd = source.indexOf(QStringLiteral("]]>"), start + 9);
        if (cdataEnd < 0) {
            *error = QStringLiteral("This chapter contains an incomplete CDATA section.");
            return false;
        }
        tag->name = QStringLiteral("#cdata");
        tag->end = cdataEnd + 3;
        tag->special = true;
        return true;
    }

    if (source.mid(start).startsWith(QStringLiteral("<!")) ||
        source.mid(start).startsWith(QStringLiteral("<?"))) {
        if (!readTagEnd(source, start, &tag->end)) {
            *error = QStringLiteral("This chapter contains an incomplete HTML declaration.");
            return false;
        }
        tag->name = QStringLiteral("#declaration");
        tag->special = true;
        return true;
    }

    qsizetype end = 0;
    if (!readTagEnd(source, start, &end)) {
        *error = QStringLiteral("This chapter contains an incomplete HTML tag.");
        return false;
    }

    qsizetype cursor = start + 1;
    if (cursor < end - 1 && source.at(cursor) == QLatin1Char('/')) {
        tag->closing = true;
        ++cursor;
    }
    const qsizetype nameStart = cursor;
    while (cursor < end - 1) {
        const QChar character = source.at(cursor);
        if (!character.isLetterOrNumber() && character != QLatin1Char(':') &&
            character != QLatin1Char('-') && character != QLatin1Char('_')) {
            break;
        }
        ++cursor;
    }
    if (cursor == nameStart) {
        *error = QStringLiteral("This chapter contains an invalid HTML tag name.");
        return false;
    }
    tag->name = source.mid(nameStart, cursor - nameStart).toLower();
    tag->end = end;

    if (tag->closing) {
        while (cursor < end - 1 && source.at(cursor).isSpace()) {
            ++cursor;
        }
        if (cursor != end - 1) {
            *error = QStringLiteral("This chapter contains attributes on a closing HTML tag.");
            return false;
        }
        return true;
    }

    while (cursor < end - 1) {
        while (cursor < end - 1 && source.at(cursor).isSpace()) {
            ++cursor;
        }
        if (cursor >= end - 1) {
            break;
        }
        if (source.at(cursor) == QLatin1Char('/')) {
            tag->selfClosing = true;
            ++cursor;
            while (cursor < end - 1 && source.at(cursor).isSpace()) {
                ++cursor;
            }
            if (cursor != end - 1) {
                *error = QStringLiteral("This chapter contains an invalid self-closing HTML tag.");
                return false;
            }
            break;
        }

        const qsizetype attributeStart = cursor;
        while (cursor < end - 1 && !source.at(cursor).isSpace() &&
               source.at(cursor) != QLatin1Char('=') &&
               source.at(cursor) != QLatin1Char('/') &&
               source.at(cursor) != QLatin1Char('>')) {
            ++cursor;
        }
        if (cursor == attributeStart) {
            *error = QStringLiteral("This chapter contains an invalid HTML attribute.");
            return false;
        }
        const QString name = source.mid(attributeStart, cursor - attributeStart).toLower();
        while (cursor < end - 1 && source.at(cursor).isSpace()) {
            ++cursor;
        }

        QString value;
        if (cursor < end - 1 && source.at(cursor) == QLatin1Char('=')) {
            ++cursor;
            while (cursor < end - 1 && source.at(cursor).isSpace()) {
                ++cursor;
            }
            if (cursor < end - 1 &&
                (source.at(cursor) == QLatin1Char('\'') ||
                 source.at(cursor) == QLatin1Char('"'))) {
                const QChar quote = source.at(cursor++);
                const qsizetype valueStart = cursor;
                while (cursor < end - 1 && source.at(cursor) != quote) {
                    ++cursor;
                }
                if (cursor >= end - 1) {
                    *error = QStringLiteral("This chapter contains an incomplete HTML attribute.");
                    return false;
                }
                value = source.mid(valueStart, cursor - valueStart);
                ++cursor;
            } else {
                const qsizetype valueStart = cursor;
                while (cursor < end - 1 && !source.at(cursor).isSpace() &&
                       source.at(cursor) != QLatin1Char('>')) {
                    ++cursor;
                }
                value = source.mid(valueStart, cursor - valueStart);
            }
        }

        if (tag->attributes.contains(name)) {
            *error = QStringLiteral("This chapter contains a duplicate HTML attribute '%1'.")
                         .arg(name);
            return false;
        }
        tag->attributes.insert(name, value);
    }
    return true;
}

bool isVoidElement(const QString &name)
{
    static const QSet<QString> names = {
        QStringLiteral("area"), QStringLiteral("base"), QStringLiteral("br"),
        QStringLiteral("col"), QStringLiteral("embed"), QStringLiteral("hr"),
        QStringLiteral("img"), QStringLiteral("input"), QStringLiteral("link"),
        QStringLiteral("meta"), QStringLiteral("param"), QStringLiteral("source"),
        QStringLiteral("track"), QStringLiteral("wbr")};
    return names.contains(name);
}

bool findTopLevelChunkEnd(const QString &source,
                          qsizetype start,
                          HtmlTag *root,
                          qsizetype *chunkEnd,
                          qsizetype *closingStart,
                          QString *error)
{
    if (!parseTag(source, start, root, error)) {
        return false;
    }
    if (root->special) {
        *chunkEnd = root->end;
        *closingStart = root->end;
        return true;
    }
    if (root->closing) {
        *error = QStringLiteral("This chapter has an unmatched closing tag '%1'.")
                     .arg(root->name);
        return false;
    }
    if (root->selfClosing || isVoidElement(root->name)) {
        *chunkEnd = root->end;
        *closingStart = root->end;
        return true;
    }

    QStringList openElements{root->name};
    qsizetype cursor = root->end;
    while (cursor < source.size()) {
        const qsizetype tagStart = source.indexOf(QLatin1Char('<'), cursor);
        if (tagStart < 0) {
            break;
        }
        HtmlTag nested;
        if (!parseTag(source, tagStart, &nested, error)) {
            return false;
        }
        cursor = nested.end;
        if (nested.special) {
            continue;
        }
        if (nested.closing) {
            if (openElements.isEmpty() || openElements.constLast() != nested.name) {
                *error = QStringLiteral("This chapter has mismatched HTML tag '%1'.")
                             .arg(nested.name);
                return false;
            }
            openElements.removeLast();
            if (openElements.isEmpty()) {
                *chunkEnd = nested.end;
                *closingStart = tagStart;
                return true;
            }
        } else if (!nested.selfClosing && !isVoidElement(nested.name)) {
            if (nested.name == QStringLiteral("p") &&
                openElements.contains(QStringLiteral("p"))) {
                *error = QStringLiteral("This chapter contains nested paragraph tags.");
                return false;
            }
            openElements.append(nested.name);
        }
    }

    *error = QStringLiteral("This chapter has an incomplete '%1' element.").arg(root->name);
    return false;
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

    const bool hexadecimal = entity.size() > 2 &&
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

QString decodedAttributeValue(const QString &value)
{
    QString decodedValue;
    for (qsizetype index = 0; index < value.size();) {
        if (value.at(index) != QLatin1Char('&')) {
            decodedValue.append(value.at(index++));
            continue;
        }
        const qsizetype end = value.indexOf(QLatin1Char(';'), index + 1);
        if (end < 0) {
            decodedValue.append(value.at(index++));
            continue;
        }
        QString decodedEntity;
        if (decodeEntity(value.mid(index + 1, end - index - 1), &decodedEntity)) {
            decodedValue.append(decodedEntity);
        } else {
            decodedValue.append(value.mid(index, end - index + 1));
        }
        index = end + 1;
    }
    return decodedValue;
}

bool decodePlainText(const QString &source, QString *text, QString *error)
{
    text->clear();
    for (qsizetype index = 0; index < source.size();) {
        const QChar current = source.at(index);
        if (current == QLatin1Char('<')) {
            *error = QStringLiteral("This paragraph contains markup.");
            return false;
        }
        if (current == QLatin1Char('\n') || current == QLatin1Char('\r')) {
            *error = QStringLiteral("This paragraph contains an embedded line break.");
            return false;
        }
        if (current != QLatin1Char('&')) {
            text->append(current);
            ++index;
            continue;
        }

        const qsizetype end = source.indexOf(QLatin1Char(';'), index + 1);
        if (end < 0) {
            *error = QStringLiteral("This paragraph contains an incomplete HTML character reference.");
            return false;
        }
        QString decoded;
        if (!decodeEntity(source.mid(index + 1, end - index - 1), &decoded)) {
            *error = QStringLiteral("This paragraph contains an unsupported HTML character reference.");
            return false;
        }
        if (decoded.contains(QLatin1Char('\n')) || decoded.contains(QLatin1Char('\r'))) {
            *error = QStringLiteral("This paragraph contains a line break inside a prose paragraph.");
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

QStringList classes(const HtmlTag &tag)
{
    return decodedAttributeValue(tag.attributes.value(QStringLiteral("class"))).split(
        QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
}

bool verifyRecord(const QString &kind,
                  const QString &id,
                  const QSet<QString> &knownIds,
                  const QString &readError,
                  const QString &fileName,
                  QString *error)
{
    if (!readError.isEmpty()) {
        *error = QStringLiteral("Cannot verify %1 link '%2': %3")
                     .arg(kind, id, readError);
        return false;
    }
    if (id.isEmpty() || !knownIds.contains(id)) {
        *error = QStringLiteral("%1 link '%2' has no matching record in %3.")
                     .arg(kind, id, fileName);
        return false;
    }
    return true;
}

struct LinkedMarkerRules {
    QString className;
    QString kind;
    QString markerName;
    QString idAttribute;
    QString fileName;
    const QSet<QString> &knownIds;
    const QString &readError;
    const QHash<QString, QString> &chapterIds;
    QSet<QString> &seenIds;
};

bool validateLinkedMarker(const HtmlTag &tag,
                          const QStringList &tagClasses,
                          const QString &chapterId,
                          const LinkedMarkerRules &rules,
                          QString *error)
{
    if (!tagClasses.contains(rules.className)) {
        return true;
    }
    if (tag.name != QStringLiteral("span")) {
        *error = QStringLiteral("A %1 link is attached to a non-span element.")
                     .arg(rules.kind);
        return false;
    }
    if (!tag.attributes.contains(rules.idAttribute)) {
        *error = QStringLiteral("A %1 has no %2 link.")
                     .arg(rules.markerName, rules.idAttribute);
        return false;
    }
    const QString id = decodedAttributeValue(tag.attributes.value(rules.idAttribute));
    if (!verifyRecord(rules.kind, id, rules.knownIds, rules.readError, rules.fileName, error)) {
        return false;
    }
    if (rules.seenIds.contains(id)) {
        *error = QStringLiteral("%1 link '%2' appears more than once in this chapter.")
                     .arg(rules.kind, id);
        return false;
    }
    const QString linkedChapter = rules.chapterIds.value(id);
    if (!linkedChapter.isEmpty() && !chapterId.isEmpty() && linkedChapter != chapterId) {
        *error = QStringLiteral("%1 link '%2' points to another chapter.")
                     .arg(rules.kind, id);
        return false;
    }
    rules.seenIds.insert(id);
    return true;
}

bool validateLinks(const QString &html,
                   const LegacyChapterLinkContext &links,
                   QString *error)
{
    QSet<QString> stickyMarkers;
    QSet<QString> darlingMarkers;
    QSet<QString> sectionParagraphs;
    QSet<QString> sectionBreaks;
    const LinkedMarkerRules stickyRules{
        QStringLiteral("ph-mark"), QStringLiteral("Placeholder"),
        QStringLiteral("placeholder marker"), QStringLiteral("data-sid"),
        QStringLiteral("stickies.json"), links.stickies.ids, links.stickies.readError,
        links.stickies.chapterIds, stickyMarkers};
    const LinkedMarkerRules darlingRules{
        QStringLiteral("darling-anchor"), QStringLiteral("Darling"),
        QStringLiteral("Darling anchor"), QStringLiteral("data-did"),
        QStringLiteral("darlings.json"), links.darlings.ids, links.darlings.readError,
        links.darlings.chapterIds, darlingMarkers};
    qsizetype cursor = 0;
    while (cursor < html.size()) {
        const qsizetype tagStart = html.indexOf(QLatin1Char('<'), cursor);
        if (tagStart < 0) {
            break;
        }
        HtmlTag tag;
        QString parseError;
        if (!parseTag(html, tagStart, &tag, &parseError)) {
            *error = parseError;
            return false;
        }
        cursor = tag.end;
        if (tag.special || tag.closing) {
            continue;
        }

        const QStringList tagClasses = classes(tag);
        if (!validateLinkedMarker(tag, tagClasses, links.chapterId, stickyRules, error) ||
            !validateLinkedMarker(tag, tagClasses, links.chapterId, darlingRules, error)) {
            return false;
        }

        const bool ghost = tagClasses.contains(QStringLiteral("ghost"));
        const bool sceneBreak = tagClasses.contains(QStringLiteral("scene-break"));
        if (ghost && tag.name != QStringLiteral("p")) {
            *error = QStringLiteral("An outline ghost is attached to a non-paragraph element.");
            return false;
        }
        if (sceneBreak && tag.name != QStringLiteral("p")) {
            *error = QStringLiteral("A scene-break link is attached to a non-paragraph element.");
            return false;
        }

        if (ghost && !tag.attributes.contains(QStringLiteral("data-sec-id"))) {
            *error = QStringLiteral("An outline ghost has no data-sec-id section link.");
            return false;
        }
        if (tag.attributes.contains(QStringLiteral("data-sec-id"))) {
            const QString id = decodedAttributeValue(
                tag.attributes.value(QStringLiteral("data-sec-id")));
            if (!verifyRecord(QStringLiteral("Section"), id, links.sectionIds,
                              links.sectionReadError, QStringLiteral("book.json sectionNotes"),
                              error)) {
                return false;
            }
            if (tag.name != QStringLiteral("p")) {
                *error = QStringLiteral("Section link '%1' is attached to a non-paragraph element.")
                             .arg(id);
                return false;
            }
            if (sectionParagraphs.contains(id)) {
                *error = QStringLiteral("Section link '%1' appears on more than one paragraph.")
                             .arg(id);
                return false;
            }
            sectionParagraphs.insert(id);
        }

        if (sceneBreak && tag.attributes.contains(QStringLiteral("data-sec-brk"))) {
            const QString id = decodedAttributeValue(
                tag.attributes.value(QStringLiteral("data-sec-brk")));
            if (!verifyRecord(QStringLiteral("Scene-break section"), id, links.sectionIds,
                              links.sectionReadError, QStringLiteral("book.json sectionNotes"),
                              error)) {
                return false;
            }
            if (sectionBreaks.contains(id)) {
                *error = QStringLiteral("Scene-break section link '%1' appears more than once.")
                             .arg(id);
                return false;
            }
            sectionBreaks.insert(id);
        } else if (tag.attributes.contains(QStringLiteral("data-sec-brk"))) {
            *error = QStringLiteral("A data-sec-brk link is not attached to a scene-break paragraph.");
            return false;
        }
    }

    for (const QString &id : sectionBreaks) {
        if (!sectionParagraphs.contains(id)) {
            *error = QStringLiteral("Scene-break section link '%1' has no matching section paragraph.")
                         .arg(id);
            return false;
        }
    }
    return true;
}

LegacyChapterDocument refused(const QString &reason, bool hasUtf8Bom, const QString &source)
{
    LegacyChapterDocument document;
    document.refusalReason = reason;
    document.hasUtf8Bom = hasUtf8Bom;
    LegacyChapterFragment fragment;
    fragment.kind = LegacyChapterContentKind::Refused;
    fragment.rawSource = source;
    document.fragments.append(fragment);
    return document;
}

QString uniqueProtectedToken(const QString &source, int index)
{
    const QString base = QStringLiteral("[Protected legacy content %1]").arg(index);
    QString token = base;
    int suffix = 2;
    while (source.contains(token)) {
        token = base.left(base.size() - 1) + QStringLiteral(" %1]").arg(suffix++);
    }
    return token;
}

void encodeEditableGroup(const QVector<LegacyChapterFragment> &original,
                         const QStringList &edited,
                         QByteArray *output)
{
    for (qsizetype index = 0; index < edited.size(); ++index) {
        const LegacyChapterFragment *fragment =
            index < original.size() ? &original.at(index) : nullptr;
        if (fragment) {
            output->append(fragment->sourcePrefix.toUtf8());
        }
        const bool preserveSceneBreak = fragment && fragment->sceneBreak &&
                                         edited.at(index) == QStringLiteral("***");
        const QString openingTag = fragment && (!fragment->sceneBreak || preserveSceneBreak)
                                       ? fragment->openingTag
                                       : QStringLiteral("<p>");
        const QString closingTag = fragment && (!fragment->sceneBreak || preserveSceneBreak)
                                       ? fragment->closingTag
                                       : QStringLiteral("</p>");
        output->append(openingTag.toUtf8());
        if (edited.at(index).isEmpty()) {
            output->append("<br>");
        } else {
            output->append(escapeHtmlText(edited.at(index)).toUtf8());
        }
        output->append(closingTag.toUtf8());
    }
    for (qsizetype index = edited.size(); index < original.size(); ++index) {
        output->append(original.at(index).sourcePrefix.toUtf8());
    }
}

}

bool LegacyChapterDocument::hasProtectedContent() const
{
    for (const LegacyChapterFragment &fragment : fragments) {
        if (fragment.kind == LegacyChapterContentKind::Protected) {
            return true;
        }
    }
    return false;
}

bool LegacyChapterDocument::hasEditableProse() const
{
    for (const LegacyChapterFragment &fragment : fragments) {
        if (fragment.kind == LegacyChapterContentKind::Supported) {
            return true;
        }
    }
    return false;
}

bool LegacyChapterDocument::editable() const
{
    if (!refusalReason.isEmpty()) {
        return false;
    }
    return fragments.isEmpty() || hasEditableProse();
}

LegacyChapterDocument LegacyChapterCodec::decode(const QByteArray &source,
                                                  const LegacyChapterLinkContext &links)
{
    const QByteArray bom = QByteArray::fromHex("efbbbf");
    const bool hasUtf8Bom = source.startsWith(bom);
    const QByteArray utf8 = hasUtf8Bom ? source.mid(3) : source;
    const QString html = QString::fromUtf8(utf8);
    if (html.toUtf8() != utf8) {
        return refused(QStringLiteral("This chapter is not valid UTF-8. It is shown as source text."),
                       hasUtf8Bom, html);
    }
    if (html.contains(QChar::Null)) {
        return refused(QStringLiteral("This chapter contains a null character. It is shown as source text."),
                       hasUtf8Bom, html);
    }
    if (!html.isEmpty()) {
        QString linkError;
        if (!validateLinks(html, links, &linkError)) {
            return refused(linkError + QStringLiteral(" This chapter is shown as source text."),
                           hasUtf8Bom, html);
        }
    }

    LegacyChapterDocument document;
    document.hasUtf8Bom = hasUtf8Bom;
    QString pendingSource;
    qsizetype offset = 0;
    while (offset < html.size()) {
        if (html.at(offset).isSpace()) {
            pendingSource.append(html.at(offset));
            ++offset;
            continue;
        }

        if (html.at(offset) != QLatin1Char('<')) {
            qsizetype end = html.indexOf(QLatin1Char('<'), offset);
            if (end < 0) {
                end = html.size();
            }
            LegacyChapterFragment fragment;
            fragment.kind = LegacyChapterContentKind::Protected;
            fragment.rawSource = pendingSource + html.mid(offset, end - offset);
            document.fragments.append(fragment);
            pendingSource.clear();
            offset = end;
            continue;
        }

        HtmlTag root;
        qsizetype end = 0;
        qsizetype closingStart = 0;
        QString parseError;
        if (!findTopLevelChunkEnd(html, offset, &root, &end, &closingStart, &parseError)) {
            return refused(parseError + QStringLiteral(" It is shown as source text."),
                           hasUtf8Bom, html);
        }

        const QString chunk = html.mid(offset, end - offset);
        LegacyChapterFragment fragment;
        if (root.name == QStringLiteral("p") && !root.special &&
            !root.closing && !root.selfClosing && closingStart > root.end) {
            const QString body = html.mid(root.end, closingStart - root.end);
            QString prose;
            QString bodyError;
            const QStringList rootClasses = classes(root);
            const bool plainSceneBreak = root.attributes.size() == 1 &&
                rootClasses == QStringList{QStringLiteral("scene-break")} && body == QStringLiteral("***");
            const bool emptyBreak = body == QStringLiteral("<br>") ||
                                    body == QStringLiteral("<br/>") ||
                                    body == QStringLiteral("<br />");
            const bool plainBody = emptyBreak || decodePlainText(body, &prose, &bodyError);
            if (plainSceneBreak) {
                fragment.kind = LegacyChapterContentKind::Supported;
                fragment.text = QStringLiteral("***");
                fragment.sceneBreak = true;
                fragment.sourcePrefix = pendingSource;
                fragment.openingTag = html.mid(offset, root.end - offset);
                fragment.closingTag = html.mid(closingStart, end - closingStart);
            } else if (root.attributes.isEmpty() && plainBody) {
                fragment.kind = LegacyChapterContentKind::Supported;
                fragment.text = emptyBreak ? QString() : prose;
                fragment.sourcePrefix = pendingSource;
                fragment.openingTag = html.mid(offset, root.end - offset);
                fragment.closingTag = html.mid(closingStart, end - closingStart);
            } else {
                fragment.kind = LegacyChapterContentKind::Protected;
                fragment.rawSource = pendingSource + chunk;
            }
        } else {
            fragment.kind = LegacyChapterContentKind::Protected;
            fragment.rawSource = pendingSource + chunk;
        }
        document.fragments.append(fragment);
        pendingSource.clear();
        offset = end;
    }
    document.trailingSource = pendingSource;

    int protectedIndex = 1;
    for (LegacyChapterFragment &fragment : document.fragments) {
        if (fragment.kind == LegacyChapterContentKind::Protected) {
            fragment.token = uniqueProtectedToken(html, protectedIndex++);
        }
    }

    QStringList visibleLines;
    for (const LegacyChapterFragment &fragment : document.fragments) {
        visibleLines.append(fragment.kind == LegacyChapterContentKind::Protected
                                ? fragment.token
                                : fragment.text);
    }
    document.text = visibleLines.join(QLatin1Char('\n'));

    if (!document.fragments.isEmpty() && !document.hasEditableProse()) {
        document.refusalReason = QStringLiteral(
            "This chapter contains protected legacy content but no prose region LEO can prove safe to edit.");
    }
    return document;
}

bool LegacyChapterCodec::validateEditedText(const LegacyChapterDocument &document,
                                            const QString &text,
                                            QString *error)
{
    if (error) {
        error->clear();
    }
    if (!document.refusalReason.isEmpty()) {
        if (error) {
            *error = document.refusalReason;
        }
        return false;
    }
    if (text.contains(QChar::Null)) {
        if (error) {
            *error = QStringLiteral("A chapter cannot contain a null character.");
        }
        return false;
    }

    QStringList protectedTokens;
    for (const LegacyChapterFragment &fragment : document.fragments) {
        if (fragment.kind == LegacyChapterContentKind::Protected) {
            protectedTokens.append(fragment.token);
        }
    }
    if (protectedTokens.isEmpty()) {
        return true;
    }

    const QStringList lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    int nextProtected = 0;
    for (const QString &line : lines) {
        if (nextProtected < protectedTokens.size() &&
            line == protectedTokens.at(nextProtected)) {
            ++nextProtected;
            continue;
        }
        if (line.contains(QStringLiteral("[Protected legacy content"))) {
            if (error) {
                *error = QStringLiteral(
                    "Edit refused because it crosses protected legacy content. Protected source and linked records remain unchanged.");
            }
            return false;
        }
    }
    if (nextProtected != protectedTokens.size()) {
        if (error) {
            *error = QStringLiteral(
                "Edit refused because it changes protected legacy content. Protected source and linked records remain unchanged.");
        }
        return false;
    }
    return true;
}

QByteArray LegacyChapterCodec::encode(const LegacyChapterDocument &document,
                                      const QString &text,
                                      QString *error)
{
    if (error) {
        error->clear();
    }
    if (!validateEditedText(document, text, error)) {
        return {};
    }
    if (!document.hasProtectedContent()) {
        return encode(text, document.hasUtf8Bom, error);
    }

    const QStringList lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    QStringList editedGroup;
    QVector<QStringList> editedGroups;
    QStringList protectedTokens;
    for (const LegacyChapterFragment &fragment : document.fragments) {
        if (fragment.kind == LegacyChapterContentKind::Protected) {
            protectedTokens.append(fragment.token);
        }
    }
    int nextProtected = 0;
    for (const QString &line : lines) {
        if (nextProtected < protectedTokens.size() &&
            line == protectedTokens.at(nextProtected)) {
            editedGroups.append(editedGroup);
            editedGroup.clear();
            ++nextProtected;
        } else {
            editedGroup.append(line);
        }
    }
    editedGroups.append(editedGroup);

    QByteArray output;
    if (document.hasUtf8Bom) {
        output.append(QByteArray::fromHex("efbbbf"));
    }
    QVector<LegacyChapterFragment> editableGroup;
    int editedGroupIndex = 0;
    for (const LegacyChapterFragment &fragment : document.fragments) {
        if (fragment.kind == LegacyChapterContentKind::Protected) {
            encodeEditableGroup(editableGroup, editedGroups.at(editedGroupIndex++), &output);
            editableGroup.clear();
            output.append(fragment.rawSource.toUtf8());
        } else {
            editableGroup.append(fragment);
        }
    }
    encodeEditableGroup(editableGroup, editedGroups.at(editedGroupIndex), &output);
    output.append(document.trailingSource.toUtf8());
    return output;
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
        } else if (paragraph == QStringLiteral("***")) {
            html.append("<p class=\"scene-break\">***</p>");
        } else {
            html.append("<p>");
            html.append(escapeHtmlText(paragraph).toUtf8());
            html.append("</p>");
        }
    }
    return html;
}
