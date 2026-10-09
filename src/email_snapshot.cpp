#include "email_snapshot.h"

#include "library_persistence.h"
#include "manuscript_export.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace {

QString stampFor(const QDateTime &now)
{
    return now.toUTC().toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'"));
}

// Keeps letters, digits, space, dash and underscore. Dots and slashes become underscores, so no path escape.
QString safeFileStem(const QString &title)
{
    QString out;
    for (const QChar c : title)
        out += (c.isLetterOrNumber() || c == QLatin1Char(' ') || c == QLatin1Char('-') || c == QLatin1Char('_'))
                   ? c : QLatin1Char('_');
    out = out.simplified().left(80);
    return out.isEmpty() ? QStringLiteral("manuscript") : out;
}

SnapshotResult failed(const QString &error)
{
    SnapshotResult result;
    result.error = error;
    return result;
}

void removeFiles(const QStringList &paths)
{
    for (const auto &path : paths) QFile::remove(path);
}

bool readWholeFile(const QString &path, QByteArray *bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    *bytes = file.readAll();
    return !bytes->isEmpty();
}

} // namespace

SnapshotResult EmailSnapshot::create(const QString &libraryPath, const QString &bookId,
                                     const QString &snapshotDir, const QDateTime &now)
{
    const QString library = QFileInfo(libraryPath).canonicalFilePath();
    const QString libraryAbsolute = QDir::cleanPath(QFileInfo(libraryPath).absoluteFilePath());
    const QString folder = QDir::cleanPath(QFileInfo(snapshotDir).absoluteFilePath());
    if (library.isEmpty()) return failed(QStringLiteral("Library not found."));
    for (const QString &root : {library, libraryAbsolute}) {
        if (folder == root || folder.startsWith(root + QLatin1Char('/')))
            return failed(QStringLiteral("Choose a snapshot folder outside the Library."));
    }

    QByteArray metadata; QString error;
    if (!LibraryPersistence::readLibraryFile(libraryPath, bookId + QStringLiteral("/book.json"), &metadata, &error))
        return failed(error);
    const auto json = QJsonDocument::fromJson(metadata);
    if (!json.isObject()) return failed(QStringLiteral("Invalid book metadata."));
    const QString title = json.object().value(QStringLiteral("title")).toString(QStringLiteral("Untitled"));

    SnapshotResult result;
    result.title = title;
    result.stamp = stampFor(now);
    const QString stem = QDir(folder).filePath(safeFileStem(title) + QLatin1Char('-') + result.stamp);
    result.pdfPath = stem + QStringLiteral(".pdf");
    result.textPath = stem + QStringLiteral(".txt");
    result.fingerprintPath = stem + QStringLiteral(".sha256");
    if (QFileInfo::exists(result.pdfPath) || QFileInfo::exists(result.textPath) || QFileInfo::exists(result.fingerprintPath))
        return failed(QStringLiteral("A snapshot with this timestamp already exists. Wait a second and try again."));
    if (!QDir().mkpath(folder)) return failed(QStringLiteral("Could not create the snapshot folder."));

    // PDF first. ManuscriptExport writes atomically, so a failed PDF leaves no PDF behind (the empty folder may remain).
    const auto pdf = ManuscriptExport::write(libraryPath, bookId, ManuscriptFormat::Pdf, result.pdfPath);
    if (!pdf.ok) return failed(pdf.error);

    const auto text = ManuscriptExport::write(libraryPath, bookId, ManuscriptFormat::Text, result.textPath);
    if (!text.ok) {
        removeFiles({result.pdfPath});
        return failed(text.error);
    }

    // Digest the files as written, so the sidecar describes exactly what the writer will attach and verify.
    QByteArray textBytes, pdfBytes;
    if (!readWholeFile(result.textPath, &textBytes) || !readWholeFile(result.pdfPath, &pdfBytes)) {
        removeFiles({result.pdfPath, result.textPath});
        return failed(QStringLiteral("Could not read the snapshot back to fingerprint it."));
    }
    result.fingerprint = QString::fromLatin1(LibraryPersistence::hash(textBytes));
    const QString pdfDigest = QString::fromLatin1(LibraryPersistence::hash(pdfBytes));

    // sha256sum format: `sha256sum -c <name>.sha256` verifies both files from the snapshot folder.
    const QByteArray sidecar = (result.fingerprint + QStringLiteral("  ") + QFileInfo(result.textPath).fileName() + QStringLiteral("\n")
                                + pdfDigest + QStringLiteral("  ") + QFileInfo(result.pdfPath).fileName() + QStringLiteral("\n")).toUtf8();
    QSaveFile saved(result.fingerprintPath);
    saved.setDirectWriteFallback(false);
    if (!saved.open(QIODevice::WriteOnly) || saved.write(sidecar) != sidecar.size() || !saved.commit()) {
        removeFiles({result.pdfPath, result.textPath});
        return failed(QStringLiteral("Could not save the fingerprint file."));
    }

    result.ok = true;
    return result;
}

DraftMessage EmailSnapshot::draftMessage(const SnapshotResult &snapshot)
{
    DraftMessage message;
    message.subject = QStringLiteral("Manuscript snapshot: %1").arg(snapshot.title);
    message.body = QStringLiteral(
                       "Manuscript snapshot: %1\n"
                       "Saved (UTC): %2\n"
                       "Attachment: %3\n"
                       "SHA-256 of the plain-text export: %4\n"
                       "To verify, run sha256sum -c %5 in the snapshot folder.\n"
                       "This is a mail draft only. Review it and send it yourself.\n")
                       .arg(snapshot.title, snapshot.stamp, QFileInfo(snapshot.pdfPath).fileName(),
                            snapshot.fingerprint, QFileInfo(snapshot.fingerprintPath).fileName());
    message.attachmentPath = snapshot.pdfPath;
    return message;
}
