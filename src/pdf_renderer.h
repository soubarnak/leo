#pragma once
#include <QByteArray>
#include <QImage>
#include <QString>
#include <QVector>
struct PdfRun { QString text; bool bold = false, italic = false; };
struct PdfParagraph { QVector<PdfRun> runs; QString align; bool sceneBreak = false; };
struct PdfChapter { QString heading; QVector<PdfParagraph> paragraphs; };
struct PdfBook { QString title, subtitle, author; QImage cover; QVector<PdfChapter> chapters; };
enum class PdfPaper { A4, Letter };
struct PdfOutput { QByteArray bytes; QString error; };
class PdfRenderer final {
public:
    // Letter is a North American habit; most of the world prints A4.
    static PdfPaper localePaper();
    // Renders into memory so the caller can publish atomically; any failure yields an empty result and an error.
    static PdfOutput render(const PdfBook &book, PdfPaper paper);
};
