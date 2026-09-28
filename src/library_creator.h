#pragma once

#include <QString>

struct NewLibraryOptions {
    QString path;
    QString authorName;
    QString writingStyle;
    QString bodyFont;
    QString dropCapStyle;
};

struct NewLibraryResult {
    bool ok = false;
    QString path;
    QString error;
    bool usedPreferenceFallback = false;
};

class LibraryCreator final {
public:
    static NewLibraryResult create(const NewLibraryOptions &options);
};
