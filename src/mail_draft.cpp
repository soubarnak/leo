#include "mail_draft.h"

#include <QFileInfo>
#include <QUrlQuery>

namespace {

QUrl snapshotFolderUrl(const DraftMessage &message)
{
    return QUrl::fromLocalFile(QFileInfo(message.attachmentPath).absolutePath());
}

QUrl mailtoUrl(const DraftMessage &message)
{
    QUrl url;
    url.setScheme(QStringLiteral("mailto"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("subject"), message.subject);
    query.addQueryItem(QStringLiteral("body"), message.body);
    url.setQuery(query);
    return url;
}

QUrl gmailComposeUrl(const DraftMessage &message)
{
    QUrl url(QStringLiteral("https://mail.google.com/mail/"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("view"), QStringLiteral("cm"));
    query.addQueryItem(QStringLiteral("fs"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("su"), message.subject);
    query.addQueryItem(QStringLiteral("body"), message.body);
    url.setQuery(query);
    return url;
}

} // namespace

DraftResult MailDraft::openDesktopDraft(const DraftMessage &message,
                                        const DraftProcessStarter &start,
                                        const DraftUrlOpener &open)
{
    // xdg-email composes only. It has no send option, so nothing leaves the machine from here.
    const QStringList arguments{
        QStringLiteral("--subject"), message.subject,
        QStringLiteral("--body"), message.body,
        QStringLiteral("--attach"), message.attachmentPath,
    };
    if (start && start(QStringLiteral("xdg-email"), arguments))
        return {DraftStatus::Opened, {}};

    const bool composed = open && open(mailtoUrl(message));
    const bool revealed = open && open(snapshotFolderUrl(message));
    if (composed)
        return {DraftStatus::OpenedWithoutAttachment, {}};
    return {DraftStatus::Failed,
            QStringLiteral("Could not open a mail draft%1. The snapshot is saved in: %2")
                .arg(revealed ? QString() : QStringLiteral(" or show the folder"),
                     QFileInfo(message.attachmentPath).absolutePath())};
}

DraftResult MailDraft::openGmailDraft(const DraftMessage &message, const DraftUrlOpener &open)
{
    // Compose page only: no Gmail API, no OAuth, no send parameter.
    if (!open || !open(gmailComposeUrl(message)))
        return {DraftStatus::Failed,
                QStringLiteral("Could not open Gmail compose. The snapshot is saved in: %1")
                    .arg(QFileInfo(message.attachmentPath).absolutePath())};
    open(snapshotFolderUrl(message));
    return {DraftStatus::OpenedWithoutAttachment, {}};
}
