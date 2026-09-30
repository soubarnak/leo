#pragma once
#include <QString>
enum class ManuscriptFormat { Text, Markdown, Html, Docx, Epub };
struct ManuscriptExportResult { bool ok = false; QString error; };
class ManuscriptExport final {
public:
    static ManuscriptExportResult writeShelf(const QString &libraryPath, const QString &shelfId,
                                             const QString &destination, ManuscriptFormat format = ManuscriptFormat::Docx);
    static ManuscriptExportResult write(const QString &libraryPath, const QString &bookId,
                                        ManuscriptFormat format, const QString &destination);
};
