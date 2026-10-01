#include "pdf_renderer.h"
#include <QLocale>
#include <algorithm>
#include <cairo-pdf.h>
#include <pango/pangocairo.h>

namespace {
constexpr double Margin = 72; // one inch
// DejaVu first: its Arabic glyphs carry text mappings that PDF readers extract in logical order, where some
// installed Noto Arabic builds extract as broken characters. Other scripts fall back through fontconfig.
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
        if (!head.isEmpty()) part = "<span size=\"xx-large\">" + escape(head) + "</span>" + part;
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
    void newPage(double top = Margin) { cairo_show_page(cr_); ++pages; y_ = top; }
    void skip(double points) { y_ += points; }
    double width() const { return width_; }
    double height() const { return height_; }
    int unknownGlyphs = 0;
    int pages = 1;

    void image(const QImage &source)
    {
        if (source.isNull()) return;
        QImage argb = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        cairo_surface_t *picture = cairo_image_surface_create_for_data(argb.bits(), CAIRO_FORMAT_ARGB32,
                                                                       argb.width(), argb.height(), argb.bytesPerLine());
        if (cairo_surface_status(picture) != CAIRO_STATUS_SUCCESS) { cairo_surface_destroy(picture); return; }
        const double boxWidth = width_ - 2 * (Margin + 8), boxHeight = height_ - 2 * (Margin + 30);
        const double scale = std::min(boxWidth / argb.width(), boxHeight / argb.height());
        cairo_save(cr_);
        cairo_translate(cr_, (width_ - argb.width() * scale) / 2, (height_ - argb.height() * scale) / 2);
        cairo_scale(cr_, scale, scale);
        cairo_set_source_surface(cr_, picture, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr_), CAIRO_FILTER_BEST);
        cairo_paint(cr_);
        cairo_restore(cr_);
        cairo_surface_destroy(picture);
    }

    // Lays out one paragraph and breaks it between lines so a long paragraph can cross pages.
    void block(const QByteArray &content, const char *face, const QString &align, double gap = 0, double indent = 0)
    {
        PangoFontDescription *description = pango_font_description_from_string(face);
        pango_layout_set_font_description(layout_, description);
        pango_font_description_free(description);
        pango_layout_set_width(layout_, PANGO_SCALE * (width_ - 2 * Margin));
        pango_layout_set_wrap(layout_, PANGO_WRAP_WORD_CHAR);
        pango_layout_set_alignment(layout_, align == "center" ? PANGO_ALIGN_CENTER : align == "right" ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT);
        pango_layout_set_justify(layout_, align == "justify");
        pango_layout_set_indent(layout_, PANGO_SCALE * indent);
        pango_layout_set_line_spacing(layout_, 1.3f);
        pango_layout_set_markup(layout_, content.constData(), content.size());
        unknownGlyphs += pango_layout_get_unknown_glyphs_count(layout_);
        PangoLayoutIter *it = pango_layout_get_iter(layout_);
        do {
            PangoRectangle logical;
            pango_layout_iter_get_line_extents(it, nullptr, &logical);
            const double top = double(logical.y) / PANGO_SCALE, lineHeight = double(logical.height) / PANGO_SCALE;
            if (y_ + lineHeight > height_ - Margin && y_ > Margin) newPage();
            const double baseline = double(pango_layout_iter_get_baseline(it)) / PANGO_SCALE;
            cairo_move_to(cr_, Margin + double(logical.x) / PANGO_SCALE, y_ + (baseline - top));
            pango_cairo_show_layout_line(cr_, pango_layout_iter_get_line_readonly(it));
            y_ += lineHeight;
        } while (pango_layout_iter_next_line(it));
        pango_layout_iter_free(it);
        y_ += gap;
    }

private:
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

PdfOutput PdfRenderer::render(const PdfBook &book, PdfPaper paper)
{
    PdfOutput out;
    {
        Canvas page(&out.bytes, paper);
        page.image(book.cover);
        page.newPage(Margin + 215);
        page.block(book.title.toHtmlEscaped().toUtf8(), SERIF " Bold 30", "center", 4);
        if (!book.subtitle.isEmpty()) page.block("<i>" + book.subtitle.toHtmlEscaped().toUtf8() + "</i>", SERIF " 13", "center", 30);
        page.block(book.author.toUpper().toHtmlEscaped().toUtf8(), SERIF " 11", "center");
        for (const auto &chapter : book.chapters) {
            page.newPage(Margin + 45);
            page.block(chapter.heading.toHtmlEscaped().toUtf8(), SERIF " 12", "center", 40);
            bool first = true, afterBreak = false;
            for (const auto &paragraph : chapter.paragraphs) {
                if (paragraph.sceneBreak) {
                    page.skip(14);
                    page.block("* * *", SERIF " 13", "center", 14);
                    afterBreak = true;
                    continue;
                }
                const bool flush = first || afterBreak || paragraph.align == "center" || paragraph.align == "right";
                page.block(markup(paragraph.runs, first), SERIF " 12", paragraph.align, 2, flush ? 0 : 26);
                first = false; afterBreak = false;
            }
        }
        if (!page.ok()) return {{}, "Could not start the PDF."};
        if (!page.finish()) return {{}, "Could not render the PDF."};
        if (page.unknownGlyphs > 0) return {{}, "No installed font covers every character in this book, so the PDF was not created."};
    }
    return out;
}
