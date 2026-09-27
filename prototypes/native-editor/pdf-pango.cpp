// THROWAWAY export probe. Synthetic text only; no application files or manuscripts.
#include <pango/pangocairo.h>
#include <cairo-pdf.h>
#include <cstdio>

constexpr double pageWidth = 595.28, pageHeight = 841.89, margin = 72;

int main(int argc, char **argv) {
    if (argc != 2) { std::fprintf(stderr,"Usage: pdf-pango OUTPUT.pdf\n"); return 2; }
    auto surface = cairo_pdf_surface_create(argv[1],pageWidth,pageHeight);
    auto painter = cairo_create(surface);
    pango_cairo_font_map_set_resolution(PANGO_CAIRO_FONT_MAP(pango_cairo_font_map_get_default()),72);
    auto layout = pango_cairo_create_layout(painter);
    pango_layout_set_width(layout,PANGO_SCALE*(pageWidth-2*margin));
    pango_layout_set_wrap(layout,PANGO_WRAP_WORD_CHAR);
    int page = 0;
    double y = margin;
    auto footer = [&] {
        char number[24]; std::snprintf(number,sizeof number,"%d",page);
        auto font = pango_font_description_from_string("Noto Sans 9");
        pango_layout_set_font_description(layout,font); pango_font_description_free(font);
        pango_layout_set_text(layout,number,-1);
        cairo_move_to(painter,pageWidth-margin-15,pageHeight-margin/2);
        pango_cairo_show_layout(painter,layout);
    };
    auto nextPage = [&] {
        if(page) { footer(); cairo_show_page(painter); }
        ++page; y=margin;
    };
    auto paragraph = [&](const char *markup, const char *face, double gap) {
        auto font = pango_font_description_from_string(face);
        pango_layout_set_font_description(layout,font); pango_font_description_free(font);
        pango_layout_set_markup(layout,markup,-1);
        int width=0,height=0; pango_layout_get_pixel_size(layout,&width,&height);
        if(y+height>pageHeight-margin) nextPage();
        cairo_move_to(painter,margin,y);
        pango_cairo_show_layout(painter,layout);
        y += height+gap;
    };
    nextPage();
    paragraph("Synthetic native export probe","Noto Serif Bold 22",28);
    paragraph("Chapter 1","Noto Serif Bold 16",22);
    paragraph("Before <b>bold</b> and <i>italic</i>; বাংলা; العربية; é; 👩🏽‍💻.","Noto Serif 12",14);
    paragraph("Written section after marker. The placeholder and ghost prompt stay out of exports.","Noto Serif 12",14);
    paragraph("***","Noto Serif 12",14);
    paragraph("Centered prose.","Noto Serif 12",14);
    nextPage();
    paragraph("Pagination sample","Noto Serif Bold 16",20);
    for(int i=1;i<=80;++i) {
        char text[512];
        std::snprintf(text,sizeof text,"%d. A synthetic paragraph crosses pages, with <b>emphasis</b>, <i>italics</i> and বাংলা العربية é 👩🏽‍💻.",i);
        paragraph(text,"Noto Serif 12",12);
    }
    footer();
    g_object_unref(layout);
    cairo_destroy(painter);
    cairo_surface_finish(surface);
    auto status=cairo_surface_status(surface);
    cairo_surface_destroy(surface);
    return status==CAIRO_STATUS_SUCCESS ? 0 : 1;
}
