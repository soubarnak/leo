#pragma once
#include <QString>
enum class ManuscriptFormat { Text, Markdown, Html, Docx };
struct ManuscriptExportResult { bool ok = false; QString error; };
class ManuscriptExport final {
public:
    static ManuscriptExportResult writeShelf(const QString &libraryPath, const QString &shelfId,
                                             const QString &destination);
    static ManuscriptExportResult write(const QString &libraryPath, const QString &bookId,
                                        ManuscriptFormat format, const QString &destination);
};
