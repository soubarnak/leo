#pragma once

#include <QString>

class AppPaths final {
public:
    static QString configDirectory();
    static QString dataDirectory();
    static QString stateDirectory();
    static QString cacheDirectory();
    static QString runtimeDirectory();
    static QString logFilePath();
    static QString instanceLockFilePath();
    static QString instanceServerName();
};
