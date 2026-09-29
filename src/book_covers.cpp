#include "book_covers.h"

#include "library_persistence.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QUuid>

namespace {
CoverResult change(const QString &root, const QString &id, CoverMode mode,
                   const QString &source = {}, bool newSeed = false, bool removeImage = false)
{
    if (id.isEmpty() || id.contains(QLatin1Char('/')) || id.contains(QLatin1Char('\\')) ||
        id == QStringLiteral(".") || id == QStringLiteral("..")) {
        return {false, QStringLiteral("Invalid book ID.")};
    }
    const QString metadataPath = id + QStringLiteral("/book.json");
    QByteArray original;
    QString error;
    if (!LibraryPersistence::readLibraryFile(root, metadataPath, &original, &error)) {
        return {false, error};
    }
    const QJsonDocument document = QJsonDocument::fromJson(original);
    if (!document.isObject()) {
        return {false, QStringLiteral("Invalid book metadata.")};
    }
    QJsonObject metadata = document.object();
    QVector<PersistenceFileChange> changes;
    if (!source.isEmpty()) {
        QImageReader reader(source);
        reader.setAutoTransform(true);
        const QImage image = reader.read();
        if (image.isNull() || image.width() > 10000 || image.height() > 10000) {
            return {false, QStringLiteral("Choose a valid image no larger than 10000 pixels per side.")};
        }
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        if (!image.save(&buffer, "PNG")) {
            return {false, QStringLiteral("Could not encode the cover image.")};
        }
        const QString file = QStringLiteral("cover-") +
                             QUuid::createUuid().toString(QUuid::Id128) + QStringLiteral(".png");
        changes.append({id + QLatin1Char('/') + file, {}, true, bytes});
        metadata.insert(QStringLiteral("coverImage"), file);
    }
    if (mode == CoverMode::Image && source.isEmpty()) {
        const QString filename = metadata.value(QStringLiteral("coverImage")).toString();
        if (filename.isEmpty() || QFileInfo(filename).fileName() != filename ||
            !QImageReader(QDir(root).filePath(id + QLatin1Char('/') + filename)).canRead()) {
            return {false, QStringLiteral("The imported cover image is unavailable.")};
        }
    }
    if (removeImage) {
        metadata.remove(QStringLiteral("coverImage"));
        metadata.insert(QStringLiteral("coverMode"), QStringLiteral("abstract"));
    } else {
        metadata.insert(QStringLiteral("coverMode"), mode == CoverMode::Image
            ? QStringLiteral("image") : QStringLiteral("abstract"));
    }
    if (newSeed || metadata.value(QStringLiteral("coverSeed")).isUndefined()) {
        metadata.insert(QStringLiteral("coverSeed"), newSeed
            ? QJsonValue(QUuid::createUuid().toString(QUuid::Id128)) : QJsonValue(id));
    }
    changes.append({metadataPath, LibraryPersistence::hash(original), false,
                    QJsonDocument(metadata).toJson(QJsonDocument::Indented)});
    const PersistenceResult saved = LibraryPersistence::saveFiles(root, changes);
    return {saved.ok, saved.error};
}
}

CoverResult BookCovers::importImage(const QString &root, const QString &id, const QString &source)
{
    if (source.isEmpty()) return {false, QStringLiteral("No image selected.")};
    return change(root, id, CoverMode::Image, source);
}

CoverResult BookCovers::setMode(const QString &root, const QString &id, CoverMode mode)
{
    return change(root, id, mode);
}

CoverResult BookCovers::removeImage(const QString &root, const QString &id)
{
    return change(root, id, CoverMode::Abstract, {}, false, true);
}

CoverResult BookCovers::repaint(const QString &root, const QString &id)
{
    return change(root, id, CoverMode::Abstract, {}, true);
}

QImage BookCovers::render(const QString &root, const QString &id, const QSize &size)
{
    if (size.isEmpty()) return {};
    QByteArray bytes;
    QString error;
    if (!LibraryPersistence::readLibraryFile(root, id + QStringLiteral("/book.json"), &bytes, &error)) return {};
    const QJsonObject metadata = QJsonDocument::fromJson(bytes).object();
    const QString filename = metadata.value(QStringLiteral("coverImage")).toString();
    const QString mode = metadata.value(QStringLiteral("coverMode")).toString();
    if (!filename.isEmpty() && (mode.isEmpty() || mode == QStringLiteral("image")) &&
        QFileInfo(filename).fileName() == filename) {
        QImageReader reader(QDir(root).filePath(id + QLatin1Char('/') + filename));
        reader.setAutoTransform(true);
        const QImage image = reader.read();
        if (!image.isNull()) return image.scaled(size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    }
    QImage image(size, QImage::Format_RGB32);
    const QByteArray hash = QCryptographicHash::hash(
        metadata.value(QStringLiteral("coverSeed")).toVariant().toString().toUtf8(),
        QCryptographicHash::Sha256);
    const int hue = static_cast<uchar>(hash.at(0)) * 360 / 256;
    image.fill(QColor::fromHsv(hue, 130, 105));
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    for (int i = 0; i < 8; ++i) {
        const int x = static_cast<uchar>(hash.at(i + 1)) * size.width() / 256;
        const int y = static_cast<uchar>(hash.at(i + 9)) * size.height() / 256;
        const int radius = qMax(10, size.width() / 3);
        painter.setBrush(QColor::fromHsv((hue + i * 31) % 360, 100 + i * 10, 150, 90));
        painter.drawEllipse(QPoint(x, y), radius, radius);
    }
    painter.setPen(Qt::white);
    QFont titleFont = painter.font();
    titleFont.setBold(true);
    titleFont.setPixelSize(qMax(9, size.width() / 10));
    painter.setFont(titleFont);
    painter.drawText(image.rect().adjusted(8, 8, -8, -size.height() / 4),
                     Qt::AlignCenter | Qt::TextWordWrap,
                     metadata.value(QStringLiteral("title")).toString(QStringLiteral("Untitled")));
    titleFont.setBold(false);
    titleFont.setPixelSize(qMax(8, size.width() / 14));
    painter.setFont(titleFont);
    painter.drawText(image.rect().adjusted(8, size.height() * 3 / 4, -8, -8),
                     Qt::AlignCenter | Qt::TextWordWrap,
                     metadata.value(QStringLiteral("author")).toString());
    return image;
}

QImage BookCovers::exportCover(const QString &root, const QString &id)
{
    QByteArray bytes;
    QString error;
    if (!LibraryPersistence::readLibraryFile(root, id + QStringLiteral("/book.json"),
                                             &bytes, &error)) return {};
    const QString filename = QJsonDocument::fromJson(bytes).object()
                                 .value(QStringLiteral("coverImage")).toString();
    if (!filename.isEmpty() && QFileInfo(filename).fileName() == filename) {
        QImageReader reader(QDir(root).filePath(id + QLatin1Char('/') + filename));
        reader.setAutoTransform(true);
        const QImage imported = reader.read();
        if (!imported.isNull()) return imported;
    }
    return render(root, id, QSize(1600, 2560));
}
