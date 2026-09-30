#pragma once
#include <QString>
enum class ManuscriptFormat { Text, Markdown, Html };
struct ManuscriptExportResult { bool ok = false; QString error; };
class ManuscriptExport final {
public:
    static ManuscriptExportResult write(const QString &libraryPath, const QString &bookId,
                                        ManuscriptFormat format, const QString &destination);
};
