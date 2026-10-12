#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>

#include <functional>

// File-tree helpers shared by library_persistence.cpp (safety snapshots, Recovered libraries)
// and library_snapshot.cpp (daily snapshots). Definitions live in library_persistence.cpp so
// the verified-copy behaviour has a single implementation.
namespace LibrarySnapshotInternal {

// "D:<relative dir>" -> empty, "F:<relative file>" -> hex SHA-256.
using Manifest = QMap<QString, QByteArray>;

bool scanTree(const QString &root, Manifest *manifest, QString *error,
              bool includeLocalArtifacts);
QByteArray manifestHash(const Manifest &manifest);
// Copies root into destination (created), re-scans the source and the copy, and requires both
// manifests to equal the first scan. Removes destination on failure. afterCopy is a test seam
// invoked once the copy finished and before verification.
bool copyVerifiedTree(const QString &root, const QString &destination, Manifest *sourceManifest,
                      QString *error, bool includeLocalArtifacts,
                      const std::function<void()> &afterCopy = {});
// Forced (no marker short-circuit) verified safety snapshot under the safety-snapshots store.
bool createSafetySnapshot(const QString &libraryRoot, QString *snapshotPath, QString *error);

bool ensureOutsideLibrary(const QString &path, const QString &libraryRoot, QString *error);
bool ensurePrivateDirectory(const QString &path, QString *error);
bool syncDirectory(const QString &path, QString *error);
// Writes bytes to path via a same-directory staged file, fsync and rename (mode 0600).
bool writeFileAtomic(const QString &path, const QByteArray &bytes, QString *error);
QString newOperationId();
QString canonicalDirectory(const QString &path);
bool isWithin(const QString &parent, const QString &child);

}
