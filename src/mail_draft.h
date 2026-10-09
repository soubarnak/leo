#pragma once

#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>

// Opening a mail draft never sends mail. There is deliberately no "Sent" status.
enum class DraftStatus {
    // Desktop mail client was asked to compose with the attachment. Leo cannot verify that the attachment was added.
    Opened,
    // Compose window opened (or mailto fallback) without the attachment; the writer attaches the file by hand.
    OpenedWithoutAttachment,
    Failed,
};

struct DraftResult {
    DraftStatus status = DraftStatus::Failed;
    QString error;
};

struct DraftMessage {
    QString subject;
    QString body;
    QString attachmentPath;
};

// Injected so tests never launch a real mail client or file manager.
using DraftProcessStarter = std::function<bool(const QString &program, const QStringList &arguments)>;
using DraftUrlOpener = std::function<bool(const QUrl &url)>;

class MailDraft final {
public:
    // Debian: xdg-email compose with --attach. Fallback: mailto (no attachment) plus the snapshot folder.
    static DraftResult openDesktopDraft(const DraftMessage &message,
                                        const DraftProcessStarter &start,
                                        const DraftUrlOpener &open);
    // Gmail: web compose window (no API, no credentials) plus the snapshot folder for manual attachment.
    static DraftResult openGmailDraft(const DraftMessage &message, const DraftUrlOpener &open);
};
