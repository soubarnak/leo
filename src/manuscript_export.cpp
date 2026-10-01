#include "manuscript_export.h"
#include "book_covers.h"
#include "library_persistence.h"
#include "pdf_renderer.h"
#include <QBuffer>
#include <QDateTime>
#include <QUuid>
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
#include <zlib.h>
#include <limits>

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
struct Paragraph { QString text, html, markdown, align; QVector<PdfRun> runs; bool sceneBreak = false; };
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
        if (hasClass(node, "scene-break")) { paragraphs.append({"***", "<p style=\"text-align:center\">***</p>", "***", {}, {}, true}); continue; }
        QTextDocument document; document.setHtml(cleaned);
        Paragraph p; p.text = document.toPlainText().replace(QChar(0xa0), ' ').replace(QChar(0x2028), '\n');
        if (p.text.trimmed().isEmpty()) continue;
        QString html, md;
        QVector<PdfRun> runs;
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
                if (!runs.isEmpty() && runs.last().bold == bold && runs.last().italic == italic) runs.last().text += text;
                else runs.append({text, bold, italic});
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
        p.align = align.hasMatch() ? align.captured(1).toLower() : QString(); p.runs = runs;
        p.html = "<p" + (align.hasMatch() ? " style=\"text-align:" + align.captured(1).toLower() + "\"" : QString()) + ">" + html + "</p>";
        p.markdown = md.replace(QChar(0x2028), "  \n");
        paragraphs.append(p);
    }
    return true;
}
QString docxParagraph(const QString &html, const QString &style = {}, bool pageBreak = false)
{
    QTextDocument document; document.setHtml(html);
    QString xml = "<w:p><w:pPr>";
    if (!style.isEmpty()) xml += "<w:pStyle w:val=\"" + style + "\"/>";
    if (pageBreak) xml += "<w:pageBreakBefore/>";
    const auto alignment = QRegularExpression("text-align:(left|center|right|justify)").match(html);
    const QString align = !alignment.hasMatch() ? "left"
        : alignment.captured(1) == "justify" ? "both" : alignment.captured(1);
    xml += "<w:jc w:val=\"" + align + "\"/></w:pPr>";
    for (auto block = document.begin(); block.isValid(); block = block.next()) {
        if (block != document.begin()) xml += "<w:r><w:br/></w:r>";
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (!fragment.isValid()) continue;
            xml += "<w:r><w:rPr>";
            if (fragment.charFormat().fontWeight() >= QFont::Bold) xml += "<w:b/>";
            if (fragment.charFormat().fontItalic()) xml += "<w:i/>";
            xml += "</w:rPr>";
            const auto lines = fragment.text().split(QChar(0x2028));
            for (int i = 0; i < lines.size(); ++i) {
                if (i) xml += "<w:br/>";
                xml += "<w:t xml:space=\"preserve\">" + lines[i].toHtmlEscaped() + "</w:t>";
            }
            xml += "</w:r>";
        }
    }
    return xml + "</w:p>";
}
using PublicationEntries = QVector<QPair<QByteArray, QByteArray>>;
QByteArray publicationArchive(const PublicationEntries &entries, QString &error)
{
    if (entries.size() > 65535) { error = "Publication exceeds ZIP entry limit."; return {}; }
    QByteArray archive, directory;
    auto appendLittleEndian = [](QByteArray &bytes, quint32 value, int count) {
        for (int i = 0; i < count; ++i) bytes.append(char((value >> (8 * i)) & 255));
    };
    for (const auto &entry : entries) {
        if (entry.first.endsWith(".xml") || entry.first.endsWith(".rels")
            || entry.first.endsWith(".opf") || entry.first.endsWith(".xhtml")) {
            QXmlStreamReader reader(entry.second);
            while (!reader.atEnd()) reader.readNext();
            if (reader.hasError()) { error = "Generated publication XML failed validation."; return {}; }
        }
        const auto size = entry.second.size();
        if (size > std::numeric_limits<quint32>::max() || archive.size() + size + directory.size() + 98 + 2 * entry.first.size() > std::numeric_limits<quint32>::max()) {
            error = "Publication exceeds ZIP size limit."; return {};
        }
        const quint32 offset = archive.size();
        const quint32 crc = crc32(0, reinterpret_cast<const Bytef *>(entry.second.constData()), size);
        // Stored ZIP local header: signature, version, flags, method, DOS time/date, CRC, sizes, name.
        appendLittleEndian(archive, 0x04034b50, 4); appendLittleEndian(archive, 20, 2); appendLittleEndian(archive, 0, 2); appendLittleEndian(archive, 0, 2);
        appendLittleEndian(archive, 0, 2); appendLittleEndian(archive, 0x21, 2); appendLittleEndian(archive, crc, 4);
        appendLittleEndian(archive, size, 4); appendLittleEndian(archive, size, 4); appendLittleEndian(archive, entry.first.size(), 2); appendLittleEndian(archive, 0, 2);
        archive += entry.first; archive += entry.second;
        // Central directory repeats the entry metadata and points to its local header.
        appendLittleEndian(directory, 0x02014b50, 4); appendLittleEndian(directory, 20, 2); appendLittleEndian(directory, 20, 2);
        appendLittleEndian(directory, 0, 2); appendLittleEndian(directory, 0, 2); appendLittleEndian(directory, 0, 2); appendLittleEndian(directory, 0x21, 2);
        appendLittleEndian(directory, crc, 4); appendLittleEndian(directory, size, 4); appendLittleEndian(directory, size, 4);
        appendLittleEndian(directory, entry.first.size(), 2); appendLittleEndian(directory, 0, 2); appendLittleEndian(directory, 0, 2);
        appendLittleEndian(directory, 0, 2); appendLittleEndian(directory, 0, 2); appendLittleEndian(directory, 0, 4); appendLittleEndian(directory, offset, 4);
        directory += entry.first;
    }
    const quint32 offset = archive.size(); archive += directory;
    // End record: disk numbers, entry counts, directory size/offset, empty comment.
    appendLittleEndian(archive, 0x06054b50, 4); appendLittleEndian(archive, 0, 2); appendLittleEndian(archive, 0, 2);
    appendLittleEndian(archive, entries.size(), 2); appendLittleEndian(archive, entries.size(), 2);
    appendLittleEndian(archive, directory.size(), 4); appendLittleEndian(archive, offset, 4); appendLittleEndian(archive, 0, 2);
    return archive;
}

struct EpubPublication {
    PublicationEntries entries = {
        {"mimetype", "application/epub+zip"},
        {"META-INF/container.xml", R"(<?xml version="1.0" encoding="UTF-8"?><container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>)"},
        {"OEBPS/style.css", R"(body { font-family: serif; line-height: 1.5; margin: 1em; }
h1, h2 { text-align: center; font-weight: normal; margin: 3em 0 2em; }
p { text-indent: 1.2em; margin: 0; }
h1 + p, h2 + p { text-indent: 0; }
p.brk { text-align: center; text-indent: 0; margin: 2.5em 0; letter-spacing: 0.5em; }
p.brk + p { text-indent: 0; }
p[style*="center"], p[style*="right"] { text-indent: 0; }
.titlepage { text-align: center; margin-top: 30%; }
.titlepage p { text-indent: 0; margin: 1em 0; }
.cover img { max-width: 100%; max-height: 100%; }
)"}
    };
    QString manifest = "<item id=\"css\" href=\"style.css\" media-type=\"text/css\"/>", spine, navigation;
    QString title, author;
    void resource(const QString &id, const QString &name, const QByteArray &bytes,
                  const QString &media, const QString &properties = {})
    {
        entries.append({("OEBPS/" + name).toUtf8(), bytes});
        manifest += "<item id=\"" + id + "\" href=\"" + name + "\" media-type=\"" + media + "\""
            + (properties.isEmpty() ? QString() : " properties=\"" + properties + "\"") + "/>";
    }
    void page(const QString &id, const QString &label, const QString &body,
              bool readingOrder = true, const QString &properties = {})
    {
        const QString document = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><!DOCTYPE html><html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\" xml:lang=\"en\"><head><title>"
            + label.toHtmlEscaped() + "</title><link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/></head><body>" + body + "</body></html>";
        resource(id, id + ".xhtml", document.toUtf8(), "application/xhtml+xml", properties);
        if (readingOrder) spine += "<itemref idref=\"" + id + "\"/>";
    }
    QByteArray archive(QString &error)
    {
        page("nav", "Contents", "<nav epub:type=\"toc\" id=\"toc\"><h1>Contents</h1><ol>" + navigation + "</ol></nav>", false, "nav");
        const QString package = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"bookid\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"bookid\">urn:uuid:"
            + QUuid::createUuid().toString(QUuid::WithoutBraces) + "</dc:identifier><dc:title>" + title.toHtmlEscaped()
            + "</dc:title><dc:creator>" + author.toHtmlEscaped() + "</dc:creator><dc:language>en</dc:language><meta property=\"dcterms:modified\">"
            + QDateTime::currentDateTimeUtc().toString("yyyy-MM-dd'T'HH:mm:ss'Z'")
            + "</meta></metadata><manifest>" + manifest + "</manifest><spine>" + spine + "</spine></package>";
        entries.append({"OEBPS/content.opf", package.toUtf8()});
        return publicationArchive(entries, error);
    }
};

QByteArray docxArchive(const QString &body, QString &error)
{
    const QByteArray document = ("<?xml version=\"1.0\" encoding=\"UTF-8\"?><w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:body>" + body +
        "<w:sectPr><w:pgSz w:w=\"12240\" w:h=\"15840\"/><w:pgMar w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\" w:left=\"1440\"/></w:sectPr></w:body></w:document>").toUtf8();
    const QVector<QPair<QByteArray, QByteArray>> entries = {
        {"[Content_Types].xml", R"(<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/><Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/><Override PartName="/word/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml"/></Types>)"},
        {"_rels/.rels", R"(<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="document" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/></Relationships>)"},
        {"word/_rels/document.xml.rels", R"(<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="styles" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/></Relationships>)"},
        {"word/styles.xml", R"(<w:styles xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:ascii="Georgia" w:hAnsi="Georgia"/><w:sz w:val="24"/></w:rPr></w:rPrDefault><w:pPrDefault><w:pPr><w:spacing w:line="360" w:lineRule="auto"/></w:pPr></w:pPrDefault></w:docDefaults><w:style w:type="paragraph" w:styleId="Title"><w:name w:val="Title"/><w:rPr><w:b/><w:sz w:val="56"/></w:rPr></w:style><w:style w:type="paragraph" w:styleId="Heading1"><w:name w:val="heading 1"/><w:pPr><w:outlineLvl w:val="0"/></w:pPr><w:rPr><w:sz w:val="28"/></w:rPr></w:style></w:styles>)"},
        {"word/document.xml", document}
    };
    return publicationArchive(entries, error);
}

}

static ManuscriptExportResult writeSelection(const QString &root, const QStringList &ids,
                                               ManuscriptFormat format, const QString &destination, const QString &anthologyTitle = {})
{
    if (destination.isEmpty()) return {false, "Export cancelled."};
    if (format != ManuscriptFormat::Text && format != ManuscriptFormat::Markdown && format != ManuscriptFormat::Html && format != ManuscriptFormat::Docx && format != ManuscriptFormat::Epub && format != ManuscriptFormat::Pdf) return {false, "Unknown export format."};
    const QString library = QFileInfo(root).canonicalFilePath();
    const QString parent = QFileInfo(QFileInfo(destination).absolutePath()).canonicalFilePath();
    const QString existing = QFileInfo(destination).canonicalFilePath();
    if (!existing.isEmpty() && (existing == library || existing.startsWith(library + '/'))) return {false, "Choose an export destination outside the Library."};
    const QString target = QDir(parent).filePath(QFileInfo(destination).fileName());
    if (parent.isEmpty() || library.isEmpty() || target == library || target.startsWith(library + '/')) return {false, "Choose an export destination outside the Library."};
    if (format == ManuscriptFormat::Pdf && ids.size() > 1) return {false, "PDF anthologies are not supported yet."};
    QString output, error;
    EpubPublication epub;
    PdfBook pdf;
    epub.title = anthologyTitle;
    if (!anthologyTitle.isEmpty() && format == ManuscriptFormat::Docx) output += docxParagraph("<p style=\"text-align:center\">" + anthologyTitle.toHtmlEscaped() + "</p>", "Title");
    int bookNumber = 0;
    for (const auto &id : ids) {
        const QString prefix = QStringLiteral("book%1").arg(++bookNumber);
        QByteArray metadata;
        if (!LibraryPersistence::readLibraryFile(root, id + "/book.json", &metadata, &error)) return {false, error};
        QJsonParseError parse;
        const auto json = QJsonDocument::fromJson(metadata, &parse);
        if (parse.error != QJsonParseError::NoError || !json.isObject()) return {false, "Invalid book metadata."};
        const auto book = json.object(); const auto order = book.value("chapterOrder").toArray();
        if (order.isEmpty()) return {false, "Book has no chapters."};
        const QString title = book.value("title").toString("Untitled"), author = book.value("author").toString();
        if (format == ManuscriptFormat::Pdf) {
            pdf.title = title; pdf.author = author; pdf.subtitle = book.value("subtitle").toString();
            pdf.cover = BookCovers::exportCover(root, id);
            if (pdf.cover.isNull()) return {false, "Could not prepare export cover."};
        } else if (format == ManuscriptFormat::Epub) {
            if (bookNumber == 1) {
                if (epub.title.isEmpty()) epub.title = title;
                epub.author = author;
                if (!anthologyTitle.isEmpty()) epub.page("anthology", anthologyTitle,
                    "<section class=\"titlepage\"><h1>" + anthologyTitle.toHtmlEscaped() + "</h1></section>");
            }
            const QImage cover = BookCovers::exportCover(root, id);
            QByteArray png; QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly);
            if (cover.isNull() || !cover.save(&buffer, "PNG")) return {false, "Could not prepare export cover."};
            epub.resource(prefix + "-image", prefix + ".png", png, "image/png", bookNumber == 1 ? "cover-image" : "");
            epub.page(prefix + "-cover", title + " — Cover", "<div class=\"cover\"><img alt=\"Cover\" src=\"" + prefix + ".png\"/></div>");
            epub.page(prefix + "-title", title, "<section class=\"titlepage\"><h1>" + title.toHtmlEscaped()
                + "</h1><p><i>" + book.value("subtitle").toString().toHtmlEscaped() + "</i></p><p>" + author.toHtmlEscaped() + "</p></section>");
            epub.navigation += "<li><a href=\"" + prefix + "-title.xhtml\">" + title.toHtmlEscaped() + "</a><ol>";
        } else if (format == ManuscriptFormat::Html) {
            output = "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"/><title>" + title.toHtmlEscaped() + "</title></head><body>\n";
            const QImage cover = BookCovers::exportCover(root, id);
            QByteArray png; QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly);
            if (cover.isNull() || !cover.save(&buffer, "PNG")) return {false, "Could not prepare export cover."};
            output += "<img alt=\"Cover\" style=\"max-width:100%\" src=\"data:image/png;base64," + QString::fromLatin1(png.toBase64()) + "\"/>\n<h1>" + title.toHtmlEscaped() + "</h1>\n<p>by " + author.toHtmlEscaped() + "</p>\n";
        } else if (format == ManuscriptFormat::Docx) {
            output += docxParagraph("<p style=\"text-align:center\">" + title.toHtmlEscaped() + "</p>", "Title", !anthologyTitle.isEmpty())
                + docxParagraph("<p style=\"text-align:center\">" + author.toHtmlEscaped() + "</p>");
        } else output = (format == ManuscriptFormat::Markdown ? "# " + markdownText(title) : title) + "\n\nby " + (format == ManuscriptFormat::Markdown ? markdownText(author) : author) + "\n\n";
        const QString subtitle = book.value("subtitle").toString();
        if (!subtitle.isEmpty() && format == ManuscriptFormat::Docx) output += docxParagraph("<p style=\"text-align:center\"><i>" + subtitle.toHtmlEscaped() + "</i></p>");
        else if (!subtitle.isEmpty() && format != ManuscriptFormat::Epub) output += format == ManuscriptFormat::Html ? "<p>" + subtitle.toHtmlEscaped() + "</p>\n" : (format == ManuscriptFormat::Markdown ? markdownText(subtitle) : subtitle) + "\n\n";
        QSet<QString> seen;
        for (int i = 0; i < order.size(); ++i) {
            const QString chapter = order.at(i).toString();
            if (chapter.isEmpty() || seen.contains(chapter)) return {false, "Invalid chapter order."};
            seen.insert(chapter);
            QByteArray source;
            if (!LibraryPersistence::readLibraryFile(root, id + "/chapters/" + chapter + ".html", &source, &error)) return {false, error};
            QVector<Paragraph> paragraphs;
            if (!parseManuscriptParagraphs(source, paragraphs, error)) return {false, error};
            if (format == ManuscriptFormat::Pdf) {
                PdfChapter pdfChapter;
                pdfChapter.heading = order.size() == 1 ? title : QStringLiteral("Chapter %1").arg(i + 1)
                    + (book.value("chapterTitles").toObject().value(chapter).toString().isEmpty() ? QString()
                       : " — " + book.value("chapterTitles").toObject().value(chapter).toString());
                for (const auto &p : paragraphs) pdfChapter.paragraphs.append({p.runs, p.align, p.sceneBreak});
                pdf.chapters.append(pdfChapter);
                continue;
            }
            if (format == ManuscriptFormat::Epub) {
                const QString heading = order.size() == 1 ? title : QStringLiteral("Chapter %1").arg(i + 1)
                    + (book.value("chapterTitles").toObject().value(chapter).toString().isEmpty() ? QString()
                       : " — " + book.value("chapterTitles").toObject().value(chapter).toString());
                const QString pageId = prefix + QStringLiteral("-chapter%1").arg(i + 1);
                QString body = "<h2>" + heading.toHtmlEscaped() + "</h2>";
                for (const auto &p : paragraphs) body += p.sceneBreak
                    ? "<p class=\"brk\">***</p>" : p.html;
                epub.page(pageId, heading, body);
                epub.navigation += "<li><a href=\"" + pageId + ".xhtml\">" + heading.toHtmlEscaped() + "</a></li>";
                continue;
            }
            if (order.size() > 1) {
                QString heading = QStringLiteral("Chapter %1").arg(i + 1);
                const QString name = book.value("chapterTitles").toObject().value(chapter).toString();
                if (!name.isEmpty()) heading += QStringLiteral(" — ") + name;
                output += format == ManuscriptFormat::Docx ? docxParagraph("<p style=\"text-align:center\">" + heading.toHtmlEscaped() + "</p>", "Heading1", true) : format == ManuscriptFormat::Html ? "<h2>" + heading.toHtmlEscaped() + "</h2>\n" : (format == ManuscriptFormat::Markdown ? "## " + markdownText(heading) : heading) + "\n\n";
            }
            if (order.size() == 1 && format == ManuscriptFormat::Docx) output += docxParagraph("", {}, true);
            for (const auto &p : paragraphs) output += (format == ManuscriptFormat::Docx ? docxParagraph(p.html) : format == ManuscriptFormat::Html ? p.html : format == ManuscriptFormat::Markdown ? p.markdown : p.text) + (format == ManuscriptFormat::Docx ? "" : format == ManuscriptFormat::Html ? "\n" : "\n\n");
        }
        if (format == ManuscriptFormat::Epub) epub.navigation += "</ol></li>";
    }
    if (format == ManuscriptFormat::Html) {
        output += "</body></html>\n";
        QXmlStreamReader reader(output); while (!reader.atEnd()) reader.readNext();
        if (reader.hasError()) return {false, "Generated HTML failed validation."};
    }
    QByteArray bytes;
    if (format == ManuscriptFormat::Pdf) {
        const PdfOutput rendered = PdfRenderer::render(pdf, PdfRenderer::localePaper());
        if (rendered.bytes.isEmpty()) return {false, rendered.error};
        bytes = rendered.bytes;
    } else bytes = format == ManuscriptFormat::Epub ? epub.archive(error) : format == ManuscriptFormat::Docx ? docxArchive(output, error) : output.toUtf8();
    if (bytes.isEmpty()) return {false, error};
    if (format != ManuscriptFormat::Docx && format != ManuscriptFormat::Epub && format != ManuscriptFormat::Pdf) {
        QStringDecoder utf8(QStringDecoder::Utf8); utf8(bytes);
        if (utf8.hasError()) return {false, "Generated text failed validation."};
    }
    QSaveFile file(destination); file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.flush()) return {false, file.errorString()};
    if (!file.commit()) return {false, file.errorString()};
    return {true, {}};
}

ManuscriptExportResult ManuscriptExport::write(const QString &root, const QString &id,
                                               ManuscriptFormat format, const QString &destination)
{
    return writeSelection(root, {id}, format, destination);
}

ManuscriptExportResult ManuscriptExport::writeShelf(const QString &root, const QString &shelfId,
                                                    const QString &destination, ManuscriptFormat format)
{
    if (format != ManuscriptFormat::Docx && format != ManuscriptFormat::Epub) return {false, "Unsupported anthology format."};
    QByteArray bytes; QString error;
    if (!LibraryPersistence::readLibraryFile(root, "library.json", &bytes, &error)) return {false, error};
    QJsonParseError parse;
    const auto metadata = QJsonDocument::fromJson(bytes, &parse);
    if (parse.error != QJsonParseError::NoError || !metadata.isObject()
        || !metadata.object().value("shelves").isArray()) return {false, "Invalid Library metadata."};
    for (const auto &value : metadata.object().value("shelves").toArray()) {
        const auto shelf = value.toObject();
        if (shelf.value("id").toString() != shelfId) continue;
        if (!shelf.value("bookIds").isArray()) return {false, "Invalid shelf book order."};
        QStringList ids;
        for (const auto &book : shelf.value("bookIds").toArray()) {
            if (!book.isString() || book.toString().isEmpty()) return {false, "Invalid shelf book ID."};
            if (!ids.contains(book.toString())) ids.append(book.toString());
        }
        if (ids.isEmpty()) return {false, "Shelf has no books."};
        return writeSelection(root, ids, format, destination,
                              shelf.value("name").toString("Anthology"));
    }
    return {false, "Shelf not found."};
}
