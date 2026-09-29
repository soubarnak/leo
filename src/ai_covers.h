#pragma once

#include <QObject>
#include <QString>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QProcess;

class AiCovers final : public QObject {
public:
    using Done = std::function<void(bool, const QString &)>;
    explicit AiCovers(QObject *parent = nullptr);
    void storeKey(const QString &key, Done done);
    void generate(const QString &libraryPath, const QString &bookId, Done done);
    void cancel();

private:
    QNetworkAccessManager *network_;
    QNetworkReply *reply_ = nullptr;
    QProcess *secretProcess_ = nullptr;
    bool cancelled_ = false;
};
