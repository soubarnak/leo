// THROWAWAY Pango/Cairo layout probe. Reads only synthetic fixture.json.
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <algorithm>
#include <cstdio>
#include <pango/pangocairo.h>
#include <cairo-pdf.h>

static QByteArray markup(const QJsonArray &runs) {
    QByteArray result;
    for (const auto &value : runs) {
        const QJsonObject run = value.toObject();
        const QByteArray text = run.value("text").toString().toUtf8();
        gchar *escaped = g_markup_escape_text(text.constData(), text.size());
        QByteArray part(escaped);
        g_free(escaped);
        if (run.value("i").toBool()) part = "<i>" + part + "</i>";
        if (run.value("b").toBool()) part = "<b>" + part + "</b>";
        result += part;
    }
    return result;
}

struct Page {
    cairo_surface_t *surface;
    cairo_t *cr;
    PangoLayout *layout;
    double width;
    double height;
    double y = 72;
    int pages = 1;
    int unknownGlyphs = 0;

    Page(const char *path, const QString &size) {
        width = size == "Letter" ? 612 : 595.92;
        height = size == "Letter" ? 792 : 842.88;
        surface = cairo_pdf_surface_create(path, width, height);
        cr = cairo_create(surface);
        pango_cairo_font_map_set_resolution(PANGO_CAIRO_FONT_MAP(pango_cairo_font_map_get_default()), 72);
        layout = pango_cairo_create_layout(cr);
    }
    void next() { cairo_show_page(cr); ++pages; y = 72; }
    void font(const char *name) {
        PangoFontDescription *desc = pango_font_description_from_string(name);
        pango_layout_set_font_description(layout, desc);
        pango_font_description_free(desc);
    }
    void line(const QByteArray &content, const char *face, PangoAlignment alignment,
              double gap = 0, double indent = 0) {
        font(face);
        pango_layout_set_width(layout, PANGO_SCALE * (width - 144));
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        pango_layout_set_alignment(layout, alignment);
        pango_layout_set_indent(layout, PANGO_SCALE * indent);
        pango_layout_set_markup(layout, content.constData(), content.size());
        unknownGlyphs += pango_layout_get_unknown_glyphs_count(layout);
        int inkWidth = 0, textHeight = 0;
        pango_layout_get_pixel_size(layout, &inkWidth, &textHeight);
        if (y + textHeight > height - 72) next();
        cairo_move_to(cr, 72, y);
        pango_cairo_show_layout(cr, layout);
        y += textHeight + gap;
    }
    void cover() {
        const double coverTop = 102;
        const double coverWidth = std::min(width - 152, (height - 2 * coverTop) * 2.0 / 3.0);
        const double coverHeight = coverWidth * 1.5;
        const double x = (width - coverWidth) / 2.0;
        cairo_rectangle(cr, x, coverTop, coverWidth, coverHeight);
        cairo_set_source_rgb(cr, 0.129, 0.216, 0.278);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 0.725, 0.784, 0.690);
        cairo_set_line_width(cr, 8);
        for (int i = 0; i < 2; ++i) {
            const double riverY = coverTop + coverHeight * (0.66 + i * 0.06);
            cairo_move_to(cr, x + 45, riverY);
            cairo_curve_to(cr, x + coverWidth * 0.35, riverY - 60,
                           x + coverWidth * 0.65, riverY - 60,
                           x + coverWidth - 45, riverY);
            cairo_stroke(cr);
        }
        cairo_set_source_rgb(cr, 1, 1, 1);
        y = coverTop + coverHeight * 0.29;
        line("THE RIVER", "Noto Serif 30", PANGO_ALIGN_CENTER, 0);
        line("LEDGER", "Noto Serif 30", PANGO_ALIGN_CENTER, 0);
        y = coverTop + coverHeight * 0.85;
        line("ADA EXAMPLE", "Noto Serif 16", PANGO_ALIGN_CENTER, 0);
        cairo_set_source_rgb(cr, 0, 0, 0);
    }
    bool finish() {
        g_object_unref(layout);
        cairo_destroy(cr);
        cairo_surface_finish(surface);
        const bool good = cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS;
        cairo_surface_destroy(surface);
        return good;
    }
};

int main(int argc, char **argv) {
    if (argc != 4) {
        std::fprintf(stderr, "Usage: pango-print fixture.json output.pdf A4|Letter\n");
        return 2;
    }
    QFile input(argv[1]);
    if (!input.open(QIODevice::ReadOnly)) return 3;
    QJsonParseError parse;
    const QJsonDocument fixture = QJsonDocument::fromJson(input.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !fixture.isObject()) return 4;
    const QJsonObject book = fixture.object();
    if (book.value("id").toString() != "pdf-PROTOTYPE") return 5;
    const QString paper = QString::fromLocal8Bit(argv[3]);
    if (paper != "A4" && paper != "Letter") return 6;
    Page page(argv[2], paper);
    page.cover();
    page.next();
    page.y = 72 + 215;
    const auto title = book.value("title").toString().toHtmlEscaped().toUtf8();
    const auto subtitle = book.value("subtitle").toString().toHtmlEscaped().toUtf8();
    const auto author = book.value("author").toString().toUpper().toHtmlEscaped().toUtf8();
    page.line(title, "Noto Serif Bold 30", PANGO_ALIGN_CENTER, 4);
    page.line("<i>" + subtitle + "</i>", "Noto Serif 13", PANGO_ALIGN_CENTER, 30);
    page.line(author, "Noto Serif 11", PANGO_ALIGN_CENTER);

    const QJsonArray sections = book.value("sections").toArray();
    for (const auto &sectionValue : sections) {
        const QJsonObject section = sectionValue.toObject();
        page.next();
        page.y = 72 + 45;
        page.line("<span letter_spacing=\"3000\">" + section.value("heading").toString().toUpper().toHtmlEscaped().toUtf8() + "</span>",
                  "Noto Serif 12", PANGO_ALIGN_CENTER, 40);
        bool first = true;
        bool afterBreak = false;
        for (const auto &paragraphValue : section.value("paras").toArray()) {
            const QJsonObject paragraph = paragraphValue.toObject();
            if (paragraph.value("sceneBreak").toBool()) {
                page.y += 14;
                page.line("* * *", "Noto Serif 13", PANGO_ALIGN_CENTER, 14);
                afterBreak = true;
                continue;
            }
            const QString alignment = paragraph.value("align").toString();
            const PangoAlignment pangoAlignment = alignment == "center" ? PANGO_ALIGN_CENTER
                : alignment == "right" ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT;
            const double indent = first || afterBreak || alignment == "center" || alignment == "right" ? 0 : 26;
            QByteArray content = markup(paragraph.value("runs").toArray());
            if (first && !content.isEmpty() && content.at(0) != '<') {
                content = "<span size=\"xx-large\">" + content.left(1) + "</span>" + content.mid(1);
            }
            page.line(content, "Noto Serif 12", pangoAlignment, 2, indent);
            first = false;
            afterBreak = false;
        }
    }
    const bool good = page.finish();
    std::printf("%d pages, %d unknown glyphs\n", page.pages, page.unknownGlyphs);
    return good ? (page.unknownGlyphs ? 8 : 0) : 7;
}
