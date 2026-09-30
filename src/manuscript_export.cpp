#include "manuscript_export.h"
#include "book_covers.h"
#include "library_persistence.h"
#include <QBuffer>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringDecoder>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFragment>
#include <QXmlStreamReader>

namespace {
struct Element {
    QString name, text;
    QMap<QString, QString> attributes;
    QVector<Element> children;
};
Element readElement(QXmlStreamReader &xml)
{
    Element node; node.name = xml.name().toString().toLower();
    for (const auto &a : xml.attributes()) node.attributes.insert(a.name().toString(), a.value().toString());
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isEndElement()) break;
        if (xml.isStartElement()) node.children.append(readElement(xml));
        else if (xml.isCharacters()) { Element text; text.text = xml.text().toString(); node.children.append(text); }
    }
    return node;
}
bool hasClass(const Element &node, const QString &name)
{ return node.attributes.value("class").split(QRegularExpression("\\s+"), Qt::SkipEmptyParts).contains(name); }
void collectGhosts(const Element &node, QSet<QString> &ids)
{
    if (hasClass(node, "ghost")) ids.insert(node.attributes.value("data-sec-id"));
    for (const auto &child : node.children) collectGhosts(child, ids);
}
QString clean(const Element &node, bool *ok)
{
    if (hasClass(node, "ghost") || hasClass(node, "ph-mark") || hasClass(node, "darling-anchor")) return {};
    if (node.name.isEmpty()) return node.text.toHtmlEscaped();
    if (!QStringList{"p", "span", "b", "strong", "i", "em", "br"}.contains(node.name)) { *ok = false; return {}; }
    QString body;
    for (const auto &child : node.children) body += clean(child, ok);
    if (node.name == "br") return "<br/>";
    if (node.name == "span" || node.name == "p") return body;
    return "<" + node.name + ">" + body + "</" + node.name + ">";
}
QString markdownText(QString text)
{
    QString escaped;
    for (const QChar c : text) {
        if (QStringLiteral("\\`*_{}[]()#+.!>~-").contains(c)) escaped += QLatin1Char('\\');
        escaped += c;
    }
    return escaped;
}
struct Paragraph { QString text, html, markdown; };
bool parseManuscriptParagraphs(const QByteArray &bytes, QVector<Paragraph> &paragraphs, QString &error)
{
    QStringDecoder decoder(QStringDecoder::Utf8);
    QString source = decoder(bytes);
    if (decoder.hasError()) { error = "Chapter is not valid UTF-8."; return false; }
    source.replace("&nbsp;", "&#160;");
    source.replace(QRegularExpression("<br\\s*/?>", QRegularExpression::CaseInsensitiveOption), "<br/>");
    QXmlStreamReader xml("<root>" + source + "</root>");
    xml.readNextStartElement();
    const Element root = readElement(xml);
    if (xml.hasError()) { error = "Chapter markup is malformed: " + xml.errorString(); return false; }
    QSet<QString> ghosts; collectGhosts(root, ghosts);
    for (const auto &node : root.children) {
        if (node.name.isEmpty() && node.text.trimmed().isEmpty()) continue;
        if (node.name != "p") { error = "Unsupported manuscript structure."; return false; }
        if (hasClass(node, "ghost") || (hasClass(node, "scene-break") && ghosts.contains(node.attributes.value("data-sec-brk")) && node.attributes.contains("data-sec-brk"))) continue;
        bool ok = true;
        const QString cleaned = clean(node, &ok);
        if (!ok) { error = "Unsupported manuscript markup."; return false; }
        if (hasClass(node, "scene-break")) { paragraphs.append({"***", "<p style=\"text-align:center\">***</p>", "***"}); continue; }
        QTextDocument document; document.setHtml(cleaned);
        Paragraph p; p.text = document.toPlainText().replace(QChar(0xa0), ' ').replace(QChar(0x2028), '\n');
        if (p.text.trimmed().isEmpty()) continue;
        QString html, md;
        for (auto block = document.begin(); block.isValid(); block = block.next()) {
            if (block != document.begin()) { html += "<br/>"; md += "  \n"; }
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const auto fragment = it.fragment();
                if (!fragment.isValid()) continue;
                const QString text = fragment.text().replace(QChar(0xa0), ' ');
                QString run = text.toHtmlEscaped().replace(QChar(0x2028), "<br/>");
                const bool bold = fragment.charFormat().fontWeight() >= QFont::Bold;
                const bool italic = fragment.charFormat().fontItalic();
                if (italic) run = "<i>" + run + "</i>";
                if (bold) run = "<b>" + run + "</b>";
                html += run;
                const QString mark = bold && italic ? "***" : bold ? "**" : italic ? "*" : "";
                const auto first = text.indexOf(QRegularExpression("\\S"));
                if (first < 0 || mark.isEmpty()) md += markdownText(text);
                else {
                    int end = text.size(); while (end > first && text.at(end - 1).isSpace()) --end;
                    md += markdownText(text.left(first)) + mark + markdownText(text.mid(first, end-first)) + mark + markdownText(text.mid(end));
                }
            }
        }
        const auto align = QRegularExpression("text-align\\s*:\\s*(left|center|right|justify)", QRegularExpression::CaseInsensitiveOption).match(node.attributes.value("style"));
        p.html = "<p" + (align.hasMatch() ? " style=\"text-align:" + align.captured(1).toLower() + "\"" : QString()) + ">" + html + "</p>";
        p.markdown = md.replace(QChar(0x2028), "  \n");
        paragraphs.append(p);
    }
    return true;
}
}

ManuscriptExportResult ManuscriptExport::write(const QString &root, const QString &id,
                                               ManuscriptFormat format, const QString &destination)
{
    if (destination.isEmpty()) return {false, "Export cancelled."};
    if (format != ManuscriptFormat::Text && format != ManuscriptFormat::Markdown && format != ManuscriptFormat::Html) return {false, "Unknown export format."};
    const QString library = QFileInfo(root).canonicalFilePath();
    const QString parent = QFileInfo(QFileInfo(destination).absolutePath()).canonicalFilePath();
    const QString existing = QFileInfo(destination).canonicalFilePath();
    if (!existing.isEmpty() && (existing == library || existing.startsWith(library + '/'))) return {false, "Choose an export destination outside the Library."};
    const QString target = QDir(parent).filePath(QFileInfo(destination).fileName());
    if (parent.isEmpty() || library.isEmpty() || target == library || target.startsWith(library + '/')) return {false, "Choose an export destination outside the Library."};
    QByteArray metadata; QString error;
    if (!LibraryPersistence::readLibraryFile(root, id + "/book.json", &metadata, &error)) return {false, error};
    QJsonParseError parse;
    const auto json = QJsonDocument::fromJson(metadata, &parse);
    if (parse.error != QJsonParseError::NoError || !json.isObject()) return {false, "Invalid book metadata."};
    const auto book = json.object(); const auto order = book.value("chapterOrder").toArray();
    if (order.isEmpty()) return {false, "Book has no chapters."};
    const QString title = book.value("title").toString("Untitled"), author = book.value("author").toString();
    QString output;
    if (format == ManuscriptFormat::Html) {
        output = "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"/><title>" + title.toHtmlEscaped() + "</title></head><body>\n";
        const QImage cover = BookCovers::exportCover(root, id);
        QByteArray png; QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly);
        if (cover.isNull() || !cover.save(&buffer, "PNG")) return {false, "Could not prepare export cover."};
        output += "<img alt=\"Cover\" style=\"max-width:100%\" src=\"data:image/png;base64," + QString::fromLatin1(png.toBase64()) + "\"/>\n<h1>" + title.toHtmlEscaped() + "</h1>\n<p>by " + author.toHtmlEscaped() + "</p>\n";
    } else output = (format == ManuscriptFormat::Markdown ? "# " + markdownText(title) : title) + "\n\nby " + (format == ManuscriptFormat::Markdown ? markdownText(author) : author) + "\n\n";
    const QString subtitle = book.value("subtitle").toString();
    if (!subtitle.isEmpty()) output += format == ManuscriptFormat::Html ? "<p>" + subtitle.toHtmlEscaped() + "</p>\n" : (format == ManuscriptFormat::Markdown ? markdownText(subtitle) : subtitle) + "\n\n";
    QSet<QString> seen;
    for (int i = 0; i < order.size(); ++i) {
        const QString chapter = order.at(i).toString();
        if (chapter.isEmpty() || seen.contains(chapter)) return {false, "Invalid chapter order."};
        seen.insert(chapter);
        QByteArray source;
        if (!LibraryPersistence::readLibraryFile(root, id + "/chapters/" + chapter + ".html", &source, &error)) return {false, error};
        QVector<Paragraph> paragraphs;
        if (!parseManuscriptParagraphs(source, paragraphs, error)) return {false, error};
        if (order.size() > 1) {
            QString heading = QStringLiteral("Chapter %1").arg(i + 1);
            const QString name = book.value("chapterTitles").toObject().value(chapter).toString();
            if (!name.isEmpty()) heading += QStringLiteral(" — ") + name;
            output += format == ManuscriptFormat::Html ? "<h2>" + heading.toHtmlEscaped() + "</h2>\n" : (format == ManuscriptFormat::Markdown ? "## " + markdownText(heading) : heading) + "\n\n";
        }
        for (const auto &p : paragraphs) output += (format == ManuscriptFormat::Html ? p.html : format == ManuscriptFormat::Markdown ? p.markdown : p.text) + (format == ManuscriptFormat::Html ? "\n" : "\n\n");
    }
    if (format == ManuscriptFormat::Html) {
        output += "</body></html>\n";
        QXmlStreamReader reader(output); while (!reader.atEnd()) reader.readNext();
        if (reader.hasError()) return {false, "Generated HTML failed validation."};
    }
    const QByteArray bytes = output.toUtf8();
    QStringDecoder utf8(QStringDecoder::Utf8); utf8(bytes);
    if (utf8.hasError()) return {false, "Generated text failed validation."};
    QSaveFile file(destination); file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.flush()) return {false, file.errorString()};
    if (!file.commit()) return {false, file.errorString()};
    return {true, {}};
}
