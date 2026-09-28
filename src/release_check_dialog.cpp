#include "release_check_dialog.h"

#include <QDesktopServices>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QVersionNumber>
#include <QVBoxLayout>

ReleaseCheckDialog::ReleaseCheckDialog(QString currentVersion,
                                       QWidget *parent,
                                       QNetworkAccessManager *network)
    : QDialog(parent)
    , currentVersion_(std::move(currentVersion))
    , network_(network ? network : new QNetworkAccessManager(this))
{
    setWindowTitle(QStringLiteral("Check for Updates"));
    setMinimumWidth(420);

    auto *layout = new QVBoxLayout(this);
    status_ = new QLabel(QStringLiteral("Checking GitHub Releases…"), this);
    status_->setObjectName(QStringLiteral("release-check-status"));
    status_->setWordWrap(true);
    status_->setAccessibleName(QStringLiteral("Release check status"));
    layout->addWidget(status_);

    auto *buttons = new QHBoxLayout;
    auto *openReleases = new QPushButton(QStringLiteral("Open GitHub Releases"), this);
    connect(openReleases, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(releasePageUrl());
    });
    buttons->addWidget(openReleases);

    auto *close = new QPushButton(QStringLiteral("Close"), this);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    QNetworkRequest request(latestReleaseApiUrl());
    request.setRawHeader(QByteArrayLiteral("Accept"), QByteArrayLiteral("application/vnd.github+json"));
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("LEO/%1").arg(currentVersion_));
    request.setTransferTimeout(10000);
    QNetworkReply *reply = network_->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] { finishCheck(reply); });
}

QUrl ReleaseCheckDialog::latestReleaseApiUrl()
{
    return QUrl(QStringLiteral("https://api.github.com/repos/soubarnak/leo/releases/latest"));
}

QUrl ReleaseCheckDialog::releasePageUrl()
{
    return QUrl(QStringLiteral("https://github.com/soubarnak/leo/releases"));
}

void ReleaseCheckDialog::finishCheck(QNetworkReply *reply)
{
    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (statusCode == 404) {
        status_->setText(QStringLiteral(
            "No LEO release is published yet. Debian packages receive updates through APT."));
        reply->deleteLater();
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        qWarning("Could not check GitHub Releases: %s", qPrintable(reply->errorString()));
        status_->setText(QStringLiteral(
            "Could not check GitHub Releases. Check your connection. Debian packages receive updates through APT."));
        reply->deleteLater();
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(reply->readAll(), &parseError);
    const QString tag = document.object().value(QStringLiteral("tag_name")).toString();
    QString latestVersion = tag;
    if (latestVersion.startsWith(QLatin1Char('v'))) {
        latestVersion.remove(0, 1);
    }

    qsizetype latestSuffix = 0;
    qsizetype currentSuffix = 0;
    const QVersionNumber latest = QVersionNumber::fromString(latestVersion, &latestSuffix);
    const QVersionNumber current = QVersionNumber::fromString(currentVersion_, &currentSuffix);
    if (parseError.error != QJsonParseError::NoError || latest.isNull() || current.isNull() ||
        latestSuffix != latestVersion.size() || currentSuffix != currentVersion_.size()) {
        status_->setText(QStringLiteral(
            "GitHub returned an invalid release version. Debian packages receive updates through APT."));
        reply->deleteLater();
        return;
    }

    if (QVersionNumber::compare(latest, current) > 0) {
        status_->setText(QStringLiteral(
            "LEO %1 is available. Open GitHub Releases for details. Install Debian package updates through APT.")
                            .arg(latestVersion));
    } else {
        status_->setText(QStringLiteral(
            "LEO is up to date. Install Debian package updates through APT."));
    }
    reply->deleteLater();
}
