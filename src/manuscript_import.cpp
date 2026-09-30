#include "manuscript_import.h"
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QTextDocument>
#include <QTextBlock>
#include <QTextFragment>
#include <QXmlStreamReader>
#include <QtEndian>
#include <zlib.h>

namespace {
constexpr int limit = 16 * 1024 * 1024;
struct Paragraph { QString text; QString html; QString style; bool page = false; };

// Read only the main document, never extract archive paths or follow relationships.
QByteArray documentXml(const QByteArray &zip)
{
    const auto u16 = [&](int p) { return qFromLittleEndian<quint16>(zip.constData() + p); };
    const auto u32 = [&](int p) { return qFromLittleEndian<quint32>(zip.constData() + p); };
    int end = -1;
    for (int p = zip.size() - 22; p >= qMax(0, int(zip.size()) - 65557); --p) {
        if (u32(p) == 0x06054b50 && p + 22 + u16(p + 20) == zip.size()) { end = p; break; }
    }
    if (end < 0 || u16(end + 4) || u16(end + 6)) return {};
    quint32 offset = u32(end + 16);
    for (int n = 0; n < u16(end + 10); ++n) {
        if (offset > quint32(zip.size()) || zip.size() - offset < 46) return {};
        int p = int(offset);
        if (u32(p) != 0x02014b50) return {};
        const quint32 next = offset + 46u + u16(p + 28) + u16(p + 30) + u16(p + 32);
        if (next < offset || next > quint32(zip.size())) return {};
        if (zip.mid(p + 46, u16(p + 28)) == "word/document.xml") {
            const quint32 packed = u32(p + 20), size = u32(p + 24), local = u32(p + 42);
            if ((u16(p + 8) & 1) || size > limit || packed > limit || local > quint32(zip.size()) || zip.size() - local < 30) return {};
            int l = int(local);
            if (u32(l) != 0x04034b50) return {};
            quint32 start = local + 30u + u16(l + 26) + u16(l + 28);
            if (start > quint32(zip.size()) || packed > quint32(zip.size()) - start) return {};
            QByteArray data = zip.mid(start, packed), out;
            if (u16(p + 10) == 0) out = data;
            else if (u16(p + 10) == 8) {
                out.resize(size);
                z_stream stream{};
                stream.next_in = reinterpret_cast<Bytef *>(data.data()); stream.avail_in = data.size();
                stream.next_out = reinterpret_cast<Bytef *>(out.data()); stream.avail_out = out.size();
                if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) return {};
                int status = inflate(&stream, Z_FINISH);
                inflateEnd(&stream);
                if (status != Z_STREAM_END || stream.total_out != size || stream.total_in != packed) return {};
            } else return {};
            if (out.size() != size || crc32(0, reinterpret_cast<const Bytef *>(out.constData()), out.size()) != u32(p + 16)) return {};
            return out;
        }
        offset = next;
    }
    return {};
}

QString inlineMarkdown(const QString &text)
{
    QTextDocument doc;
    doc.setMarkdown(text, QTextDocument::MarkdownDialectCommonMark);
    QString html;
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
        if (!html.isEmpty()) html += QLatin1Char('\n');
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid()) continue;
            QString run = fragment.text().toHtmlEscaped();
            const auto format = fragment.charFormat();
            if (format.fontItalic()) run = "<i>" + run + "</i>";
            if (format.fontWeight() >= QFont::Bold) run = "<b>" + run + "</b>";
            html += run;
        }
    }
    return html;
}
}

ManuscriptPreview ManuscriptImport::read(const QString &path)
{
    ManuscriptPreview result;
    result.title = QFileInfo(path).completeBaseName();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > limit) {
        result.error = "Cannot read manuscript (maximum size is 16 MiB)."; return result;
    }
    const QByteArray bytes = file.read(limit + 1);
    if (file.error() != QFile::NoError || bytes.size() > limit) {
        result.error = "Could not read the complete manuscript."; return result;
    }
    const QString ext = QFileInfo(path).suffix().toLower();
    QVector<Paragraph> paragraphs;
    if (ext == "docx") {
        const QByteArray xml = documentXml(bytes);
        if (xml.isEmpty()) { result.error = "Malformed, encrypted or unsupported DOCX archive; nothing imported."; return result; }
        QXmlStreamReader reader(xml);
        Paragraph paragraph;
        bool inParagraph = false, bold = false, italic = false, body = false;
        const QString ns = "http://schemas.openxmlformats.org/wordprocessingml/2006/main";
        while (!reader.atEnd()) {
            reader.readNext();
            if (reader.isDTD() || reader.isEntityReference()) { result.error = "DOCX entities are refused."; return result; }
            if (reader.isStartElement() && reader.name() == QStringLiteral("AlternateContent")) {
                result.warnings.append(QStringLiteral("Unsupported DOCX element: AlternateContent"));
                result.error = QStringLiteral("DOCX contains unsupported content; nothing imported.");
            }
            if (reader.namespaceUri() != ns) continue;
            const QString name = reader.name().toString();
            if (reader.isStartElement()) {
                if (name == "body") body = true;
                if (name == "p") { paragraph = {}; inParagraph = true; }
                if (name == "r") bold = italic = false;
                if (name == "b" || name == "i") {
                    const QString value = reader.attributes().value(ns, "val").toString();
                    const bool enabled = value != "0" && value != "false" && value != "off";
                    if (name == "b") bold = enabled; else italic = enabled;
                }
                if (name == "pStyle") paragraph.style = reader.attributes().value(ns, "val").toString();
                if (name == "pageBreakBefore") paragraph.page = true;
                if (inParagraph && (name == "t" || name == "tab" || name == "br" || name == "cr")) {
                    QString text;
                    if (name == "t") text = reader.readElementText();
                    else if (name == "tab") text = "\t";
                    else if (reader.attributes().value(ns, "type") == "page") {
                        if (!paragraph.text.isEmpty()) {
                            paragraphs.append(paragraph);
                            const QString style = paragraph.style;
                            paragraph = {}; paragraph.style = style;
                        }
                        paragraph.page = true;
                    }
                    else text = "\n";
                    paragraph.text += text;
                    QStringList lines;
                    for (const QString &line : text.split(QLatin1Char('\n'))) {
                        QString escaped = line.toHtmlEscaped();
                        if (!escaped.isEmpty()) {
                            if (italic) escaped = "<i>" + escaped + "</i>";
                            if (bold) escaped = "<b>" + escaped + "</b>";
                        }
                        lines.append(escaped);
                    }
                    paragraph.html += lines.join(QLatin1Char('\n'));
                }
                if (name == "drawing" || name == "object" || name == "tbl" || name == "footnoteReference" || name == "endnoteReference" || name == "del" || name == "instrText" || name == "altChunk" || name == "fldSimple" || name == "sym") {
                    result.warnings.append("Unsupported DOCX element: " + name);
                    // Refuse rather than silently discard or reorder manuscript content.
                    result.error = "DOCX contains unsupported content; nothing imported.";
                }
            } else if (reader.isEndElement() && name == "p") {
                if (inParagraph) paragraphs.append(paragraph);
                inParagraph = false;
            }
        }
        if (reader.hasError() || !body) result.error = "Malformed DOCX document; nothing imported.";
        result.warnings.append("DOCX: bold and italic preserved; other styling, headers and footers are omitted.");
        if (!result.error.isEmpty()) return result;
    } else if (ext == "txt" || ext == "md" || ext == "markdown") {
        QStringDecoder decoder(QStringDecoder::Utf8);
        QString text = decoder(bytes);
        if (decoder.hasError() || text.contains(QChar::Null)) { result.error = "Manuscripts must contain valid UTF-8 text without NUL characters."; return result; }
        text.replace("\r\n", "\n"); text.replace('\r', '\n');
        if (ext != "txt" && text.contains(QRegularExpression("<[^>]+>"))) {
            result.error = "External HTML and semantic IDs in Markdown are refused; nothing imported."; return result;
        }
        for (const QString &line : text.split('\n')) {
            Paragraph p;
            p.text = line;
            if (ext != "txt") {
                const auto heading = QRegularExpression("^(#{1,6}) +(.+)$").match(line);
                if (heading.hasMatch()) { p.style = heading.captured(1).size() == 1 ? "Title" : "Heading1"; p.text = heading.captured(2); }
                p.html = inlineMarkdown(p.text);
            } else p.html = p.text.toHtmlEscaped();
            paragraphs.append(p);
        }
        if (ext != "txt") result.warnings.append("Markdown: bold and italic preserved; links, images, lists and other styling reduced to text.");
    } else { result.error = "Supported manuscript formats: DOCX, UTF-8 TXT and Markdown."; return result; }

    const QRegularExpression heading("^(chapter|part|prologue|epilogue)\\b.{0,80}$", QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression scene("^\\s*([*#•~⁂—–-]\\s*){1,7}$");
    ImportedChapter current{QStringLiteral("Chapter 1"), {}};
    bool seenText = false;
    for (const auto &p : paragraphs) {
        const QString text = p.text.trimmed();
        if (!seenText && p.style == "Title") { result.title = text; seenText = true; continue; }
        if (p.style == "Heading1" || heading.match(text).hasMatch() || p.page) {
            if (!current.html.isEmpty()) { result.chapters.append(current); current = {QStringLiteral("Chapter %1").arg(result.chapters.size() + 1), {}}; }
            if (p.style == "Heading1" || heading.match(text).hasMatch()) { current.title = text; seenText = true; continue; }
        }
        if (text.isEmpty()) continue;
        seenText = true;
        if (scene.match(text).hasMatch()) { current.html += "<p class=\"scene-break\">***</p>\n"; ++result.sceneBreaks; }
        else current.html += ("<p>" + QString(p.html).replace(QLatin1Char('\n'), QStringLiteral("</p>\n<p>")) + "</p>\n").toUtf8();
    }
    if (!current.html.isEmpty()) result.chapters.append(current);
    if (result.chapters.isEmpty()) result.error = "No manuscript prose found; nothing imported.";
    return result;
}
