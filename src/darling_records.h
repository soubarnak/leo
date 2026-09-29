#pragma once

#include <QJsonArray>
#include <QString>

struct DarlingResult {
    bool ok = false;
    bool review = false;
    QString error;
    QString chapterId;
    int position = -1;
};

class DarlingRecords final {
public:
    DarlingRecords(QString libraryPath, QString bookId);
    bool list(QJsonArray *records, QString *error) const;
    DarlingResult cut(const QString &chapterId, int start, int end) const;
    DarlingResult restore(const QString &id, bool allowFallback = false) const;

private:
    QString libraryPath_;
    QString bookId_;
};
