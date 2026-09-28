#pragma once

#include <QDialog>
#include <QUrl>

class QLabel;
class QNetworkAccessManager;
class QNetworkReply;

class ReleaseCheckDialog final : public QDialog {
    Q_OBJECT

public:
    explicit ReleaseCheckDialog(QString currentVersion,
                                QWidget *parent = nullptr,
                                QNetworkAccessManager *network = nullptr);

    static QUrl latestReleaseApiUrl();
    static QUrl releasePageUrl();

private:
    void finishCheck(QNetworkReply *reply);

    QString currentVersion_;
    QLabel *status_ = nullptr;
    QNetworkAccessManager *network_ = nullptr;
};
