#pragma once

#include <QImage>
#include <QString>

struct CoverResult {
    bool ok = false;
    QString error;
};

enum class CoverMode { Image, Abstract, Painted };

class BookCovers final {
public:
    static CoverResult importImage(const QString &libraryPath, const QString &bookId,
                                   const QString &sourcePath);
    static CoverResult setMode(const QString &libraryPath, const QString &bookId,
                               CoverMode mode);
    static CoverResult removeImage(const QString &libraryPath, const QString &bookId);
    static CoverResult repaint(const QString &libraryPath, const QString &bookId);
    static CoverResult savePainting(const QString &libraryPath, const QString &bookId,
                                    const QByteArray &imageBytes);
    static QImage exportCover(const QString &libraryPath, const QString &bookId);
    static QImage render(const QString &libraryPath, const QString &bookId,
                         const QSize &size, bool ignorePainting = false);
};
