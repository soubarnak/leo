#include "pdf_renderer.h"
#include <QLocale>
#include <algorithm>
#include <cairo-pdf.h>
#include <pango/pangocairo.h>

namespace {
constexpr double Margin = 72; // one inch
// DejaVu first: its Arabic glyphs carry text mappings that PDF readers extract in logical order, where some
// installed Noto Arabic builds extract as broken characters. Other scripts fall back through fontconfig.
// NEO 0.7.9 body copy: 13pt, line-height 1.7, text column capped at 620 CSS px (465pt).
constexpr double BodySize = 13, BodyLine = 1.7 * BodySize, MaxColumn = 465;
#define SERIF "DejaVu Serif,DejaVu Sans,serif"

QByteArray markup(const QVector<PdfRun> &runs, bool initial)
{
    QByteArray result;
    for (const auto &run : runs) {
        QString text = run.text, head;
        if (initial && result.isEmpty() && !text.isEmpty()) {
            // The opening character keeps its surrogate pair and combining marks.
            int length = text.at(0).isHighSurrogate() && text.size() > 1 ? 2 : 1;
            while (length < text.size() && text.at(length).category() == QChar::Mark_NonSpacing) ++length;
            head = text.left(length); text = text.mid(length);
        }
        auto escape = [](const QString &part) {
            const QByteArray utf8 = QString(part).replace(QChar(0x2028), '\n').toUtf8();
            gchar *escaped = g_markup_escape_text(utf8.constData(), utf8.size());
            const QByteArray out(escaped); g_free(escaped); return out;
        };
        QByteArray part = escape(text);
        if (!head.isEmpty()) part = "<span size=\"23962\">" + escape(head) + "</span>" + part;
        if (run.italic) part = "<i>" + part + "</i>";
        if (run.bold) part = "<b>" + part + "</b>";
        result += part;
    }
    return result;
}

cairo_status_t writeChunk(void *closure, const unsigned char *data, unsigned int length)
{
    static_cast<QByteArray *>(closure)->append(reinterpret_cast<const char *>(data), length);
    return CAIRO_STATUS_SUCCESS;
}

class Canvas {
public:
    Canvas(QByteArray *sink, PdfPaper paper)
        : width_(paper == PdfPaper::Letter ? 612 : 595.92), height_(paper == PdfPaper::Letter ? 792 : 842.88)
    {
        surface_ = cairo_pdf_surface_create_for_stream(writeChunk, sink, width_, height_);
        cr_ = cairo_create(surface_);
        fontMap_ = pango_cairo_font_map_new();
        pango_cairo_font_map_set_resolution(PANGO_CAIRO_FONT_MAP(fontMap_), 72);
        context_ = pango_font_map_create_context(fontMap_);
        pango_cairo_update_context(cr_, context_);
        layout_ = pango_layout_new(context_);
    }
    Canvas(const Canvas &) = delete;
    Canvas &operator=(const Canvas &) = delete;
    ~Canvas()
    {
        g_object_unref(layout_); g_object_unref(context_); g_object_unref(fontMap_);
        cairo_destroy(cr_); cairo_surface_destroy(surface_);
    }
    bool ok() const { return cairo_surface_status(surface_) == CAIRO_STATUS_SUCCESS && cairo_status(cr_) == CAIRO_STATUS_SUCCESS; }
    bool finish()
    {
        cairo_surface_finish(surface_);
        return cairo_surface_status(surface_) == CAIRO_STATUS_SUCCESS && cairo_status(cr_) == CAIRO_STATUS_SUCCESS;
    }
    void skip(double points) { y_ += points; }
    void newPage(double top = Margin) { cairo_show_page(cr_); ++pages; y_ = top; }
    double contentHeight() const { return height_ - 2 * Margin; }
    int unknownGlyphs = 0;
    int pages = 1;

    void image(const QImage &source)
    {
        if (source.isNull()) return;
        QImage argb = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        cairo_surface_t *picture = cairo_image_surface_create_for_data(argb.bits(), CAIRO_FORMAT_ARGB32,
                                                                       argb.width(), argb.height(), argb.bytesPerLine());
        if (cairo_surface_status(picture) != CAIRO_STATUS_SUCCESS) { cairo_surface_destroy(picture); return; }
        const double boxWidth = columnWidth(), boxHeight = 0.95 * (height_ - 2 * Margin);
        const double scale = std::min(boxWidth / argb.width(), boxHeight / argb.height());
        cairo_save(cr_);
        cairo_translate(cr_, (width_ - argb.width() * scale) / 2, Margin);
        cairo_scale(cr_, scale, scale);
        cairo_set_source_surface(cr_, picture, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr_), CAIRO_FILTER_BEST);
        cairo_paint(cr_);
        cairo_restore(cr_);
        cairo_surface_destroy(picture);
    }

    // Lays out one paragraph and breaks it between lines so a long paragraph can cross pages.
    void block(const QByteArray &content, const char *face, const QString &align, double lineHeight,
               double before = 0, double after = 0, double indent = 0)
    {
        y_ += before;
        PangoFontDescription *description = pango_font_description_from_string(face);
        pango_layout_set_font_description(layout_, description);
        pango_font_description_free(description);
        pango_layout_set_width(layout_, PANGO_SCALE * columnWidth());
        pango_layout_set_wrap(layout_, PANGO_WRAP_WORD_CHAR);
        pango_layout_set_alignment(layout_, align == "center" ? PANGO_ALIGN_CENTER : align == "right" ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT);
        pango_layout_set_justify(layout_, align == "justify");
        pango_layout_set_indent(layout_, PANGO_SCALE * indent);
        PangoAttrList *attributes = pango_attr_list_new();
        pango_attr_list_insert(attributes, pango_attr_line_height_new_absolute(int(lineHeight * PANGO_SCALE)));
        pango_layout_set_attributes(layout_, attributes);
        pango_attr_list_unref(attributes);
        pango_layout_set_markup(layout_, content.constData(), content.size());
        unknownGlyphs += pango_layout_get_unknown_glyphs_count(layout_);
        PangoLayoutIter *it = pango_layout_get_iter(layout_);
        do {
            PangoRectangle logical;
            pango_layout_iter_get_line_extents(it, nullptr, &logical);
            const double top = double(logical.y) / PANGO_SCALE, lineHeight = double(logical.height) / PANGO_SCALE;
            if (y_ + lineHeight > height_ - Margin && y_ > Margin) newPage();
            const double baseline = double(pango_layout_iter_get_baseline(it)) / PANGO_SCALE;
            cairo_move_to(cr_, (width_ - columnWidth()) / 2 + double(logical.x) / PANGO_SCALE, y_ + (baseline - top));
            pango_cairo_show_layout_line(cr_, pango_layout_iter_get_line_readonly(it));
            y_ += lineHeight;
        } while (pango_layout_iter_next_line(it));
        pango_layout_iter_free(it);
        y_ += after;
    }

private:
    double columnWidth() const { return std::min(width_ - 2 * Margin, MaxColumn); }
    double width_, height_, y_ = Margin;
    cairo_surface_t *surface_ = nullptr;
    cairo_t *cr_ = nullptr;
    PangoFontMap *fontMap_ = nullptr;
    PangoContext *context_ = nullptr;
    PangoLayout *layout_ = nullptr;
};
}

PdfPaper PdfRenderer::localePaper()
{
    switch (QLocale().territory()) {
    case QLocale::UnitedStates: case QLocale::Canada: case QLocale::Mexico: case QLocale::Philippines: return PdfPaper::Letter;
    default: return PdfPaper::A4;
    }
}

PdfOutput PdfRenderer::render(const QVector<PdfBook> &books, PdfPaper paper, const QString &anthologyTitle)
{
    PdfOutput out;
    if (books.isEmpty()) return {{}, "There is no book to export."};
    qsizetype expectedPages = anthologyTitle.isEmpty() ? 0 : 1;
    {
        Canvas page(&out.bytes, paper);
        bool firstPage = true;
        if (!anthologyTitle.isEmpty()) {
            page.skip(0.30 * page.contentHeight());
            page.block(anthologyTitle.toHtmlEscaped().toUtf8(), SERIF " Bold 30", "center", 51);
            firstPage = false;
        }
        for (const auto &book : books) {
            expectedPages += 2 + book.chapters.size();
            if (!firstPage) page.newPage();
            firstPage = false;
            page.image(book.cover);
            page.newPage(Margin + 0.30 * page.contentHeight());
            page.block(book.title.toHtmlEscaped().toUtf8(), SERIF " Bold 30", "center", 51);
            if (!book.subtitle.isEmpty()) page.block("<i>" + book.subtitle.toHtmlEscaped().toUtf8() + "</i>", SERIF " 13", "center", BodyLine);
            page.block(book.author.toUpper().toHtmlEscaped().toUtf8(), SERIF " 11", "center", 1.7 * 11, 30);
            for (const auto &chapter : book.chapters) {
                page.newPage(Margin);
                page.block(chapter.heading.toHtmlEscaped().toUtf8(), SERIF " 12", "center", 1.7 * 12, 45, 30);
                bool first = true, afterBreak = false;
                for (const auto &paragraph : chapter.paragraphs) {
                    if (paragraph.sceneBreak) {
                        page.block("* * *", SERIF " 13", "center", BodyLine, 1.0 * 32.5, 32.5);
                        afterBreak = true;
                        continue;
                    }
                    const bool flush = first || afterBreak || paragraph.align == "center" || paragraph.align == "right";
                    page.block(markup(paragraph.runs, first), SERIF " 13", paragraph.align, BodyLine, 0, 0, flush ? 0 : 2 * BodySize);
                    first = false; afterBreak = false;
                }
            }
        }
        if (!page.ok()) return {{}, "Could not start the PDF."};
        if (!page.finish()) return {{}, "Could not render the PDF."};
        // Every cover, title page and chapter starts a page; with the anthology title page that is the minimum; a shortfall means a page was lost.
        if (page.pages < expectedPages) return {{}, "The PDF is missing pages, so it was not created."};
        if (page.unknownGlyphs > 0) return {{}, "No installed font covers every character in this book, so the PDF was not created."};
    }
    const QByteArray tail = out.bytes.right(1024);
    if (!out.bytes.startsWith("%PDF-") || !tail.contains("startxref") || !tail.contains("%%EOF"))
        return {{}, "The PDF is incomplete, so it was not created."};
    return out;
}
