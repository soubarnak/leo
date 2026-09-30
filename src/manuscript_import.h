#pragma once
#include <QByteArray>
#include <QStringList>
#include <QVector>

struct ImportedChapter {
    QString title;
    QByteArray html;
};
struct ManuscriptPreview {
    QString title;
    QVector<ImportedChapter> chapters;
    QStringList warnings;
    QString error;
    int sceneBreaks = 0;
    bool ok() const { return error.isEmpty() && !chapters.isEmpty(); }
};
class ManuscriptImport final {
public:
    static ManuscriptPreview read(const QString &sourcePath);
};
