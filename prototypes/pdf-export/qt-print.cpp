// THROWAWAY negative control: Qt rich text print of NEO's actual export HTML.
#include <QApplication>
#include <QFile>
#include <QPageLayout>
#include <QPageSize>
#include <QPrinter>
#include <QTextDocument>

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    if (argc != 4) return 2;
    QFile input(argv[1]);
    if (!input.open(QIODevice::ReadOnly)) return 3;
    QTextDocument document;
    document.setHtml(QString::fromUtf8(input.readAll()));
    QPrinter printer(QPrinter::HighResolution);
    printer.setOutputFormat(QPrinter::PdfFormat);
    printer.setOutputFileName(QString::fromLocal8Bit(argv[2]));
    const QPageSize size(QString::fromLocal8Bit(argv[3]) == "Letter" ? QPageSize::Letter : QPageSize::A4);
    printer.setPageSize(size);
    printer.setPageMargins(QMarginsF(25.4, 25.4, 25.4, 25.4), QPageLayout::Millimeter);
    document.print(&printer);
    return printer.printerState() == QPrinter::Error ? 4 : 0;
}
