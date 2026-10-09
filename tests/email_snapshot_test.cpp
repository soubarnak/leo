#include "email_snapshot.h"
#include "mail_draft.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QTemporaryDir>
#include <QUrl>
#include <QUrlQuery>
#include <QtTest>

namespace {

QDateTime stamp() { return QDateTime::fromString(QStringLiteral("2026-10-09T12:34:56Z"), Qt::ISODate); }

QByteArray sha256HexOf(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex();
}

// Synthetic book: one chapter with visible prose and a ghost (outline) paragraph the exports must omit.
void writeBook(const QString &library, const QString &title, const QString &prose, const QString &ghost) {
    QDir().mkpath(library + QStringLiteral("/b/chapters"));
    QFile book(library + QStringLiteral("/b/book.json"));
    QVERIFY(book.open(QIODevice::WriteOnly));
    book.write(QStringLiteral(R"({"title":"%1","author":"Ada Example","chapterOrder":["one"],"chapterTitles":{"one":"Arrival"}})")
                   .arg(title).toUtf8());
    book.close();
    QFile chapter(library + QStringLiteral("/b/chapters/one.html"));
    QVERIFY(chapter.open(QIODevice::WriteOnly));
    chapter.write(QStringLiteral("<p>%1</p><p class=\"ghost\" data-sec-id=\"g\">%2</p>").arg(prose, ghost).toUtf8());
    chapter.close();
}

// Every stub records what it was asked to do, so tests can prove nothing was sent or launched unexpectedly.
struct Recorder {
    QStringList programs;
    QList<QUrl> urls;
    QStringList arguments;
};

DraftProcessStarter recordingStarter(Recorder &recorder, bool result) {
    return [&recorder, result](const QString &program, const QStringList &arguments) {
        recorder.programs << program;
        recorder.arguments = arguments;
        return result;
    };
}

DraftUrlOpener recordingOpener(Recorder &recorder, bool result) {
    return [&recorder, result](const QUrl &url) {
        recorder.urls << url;
        return result;
    };
}

DraftMessage sampleMessage() {
    return {QStringLiteral("Manuscript snapshot"), QStringLiteral("Attach the PDF. Review it and send it yourself."),
            QStringLiteral("/synthetic/River Ledger-20261009T123456Z.pdf")};
}

} // namespace

class EmailSnapshotTest : public QObject {
    Q_OBJECT
private slots:
    void snapshotIsVerifiable();
    void fingerprintIgnoresGhostText();
    void fingerprintChangesWhenProseChanges();
    void snapshotSurvivesDraftFailure();
    void failedPdfWritesNothing();
    void existingSnapshotIsNotReplaced();
    void snapshotInsideLibraryIsRefused();
    void titleCannotEscapeSnapshotFolder();
    void draftMessageNeverClaimsSent();
    void desktopDraftUsesAttachmentAndNeverSends();
    void desktopDraftFallsBackToMailtoAndFolder();
    void desktopDraftFailureIsReported();
    void gmailDraftOpensComposeWithoutSending();
    void gmailDraftFailureIsReported();
};

void EmailSnapshotTest::snapshotIsVerifiable() {
    QTemporaryDir library, snapshots;
    writeBook(library.path(), QStringLiteral("River Ledger"), QStringLiteral("Arrival prose."), QStringLiteral("Outline note"));
    const auto result = EmailSnapshot::create(library.path(), QStringLiteral("b"), snapshots.path(), stamp());
    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(result.stamp, QStringLiteral("20261009T123456Z"));
    QCOMPARE(QFileInfo(result.pdfPath).fileName(), QStringLiteral("River Ledger-20261009T123456Z.pdf"));
    QCOMPARE(QFileInfo(result.textPath).fileName(), QStringLiteral("River Ledger-20261009T123456Z.txt"));
    QCOMPARE(QFileInfo(result.fingerprintPath).fileName(), QStringLiteral("River Ledger-20261009T123456Z.sha256"));

    QFile pdf(result.pdfPath);
    QVERIFY(pdf.open(QIODevice::ReadOnly));
    QCOMPARE(pdf.read(5), QByteArray("%PDF-"));

    QFile text(result.textPath);
    QVERIFY(text.open(QIODevice::ReadOnly));
    const QString textBody = QString::fromUtf8(text.readAll());
    QVERIFY(textBody.contains(QStringLiteral("Arrival prose.")));
    QVERIFY(!textBody.contains(QStringLiteral("Outline note")));

    // Independent recomputation must match the reported fingerprint and the sha256sum sidecar.
    QCOMPARE(QString::fromLatin1(sha256HexOf(result.textPath)), result.fingerprint);
    QVERIFY(result.fingerprint.size() == 64);
    QFile sidecar(result.fingerprintPath);
    QVERIFY(sidecar.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromUtf8(sidecar.readAll()),
             result.fingerprint + QStringLiteral("  ") + QFileInfo(result.textPath).fileName() + QStringLiteral("\n"));

    // Draft text must not claim the mail was sent.
    QVERIFY(!EmailSnapshot::draftMessage(result).body.contains(QStringLiteral("sent"), Qt::CaseInsensitive));
    QCOMPARE(EmailSnapshot::draftMessage(result).attachmentPath, result.pdfPath);
}

void EmailSnapshotTest::fingerprintIgnoresGhostText() {
    QTemporaryDir library, snapshots;
    writeBook(library.path(), QStringLiteral("River Ledger"), QStringLiteral("Arrival prose."), QStringLiteral("Outline note"));
    const auto first = EmailSnapshot::create(library.path(), QStringLiteral("b"), snapshots.path(), stamp());
    QVERIFY2(first.ok, qPrintable(first.error));

    // The PDF and text omit ghost paragraphs, so changing only ghost text must leave the snapshot unchanged.
    writeBook(library.path(), QStringLiteral("River Ledger"), QStringLiteral("Arrival prose."), QStringLiteral("Rewritten private outline"));
    const auto second = EmailSnapshot::create(library.path(), QStringLiteral("b"), snapshots.path(), stamp().addSecs(1));
    QVERIFY2(second.ok, qPrintable(second.error));
    QCOMPARE(second.fingerprint, first.fingerprint);
}

void EmailSnapshotTest::fingerprintChangesWhenProseChanges() {
    QTemporaryDir library, snapshots;
    writeBook(library.path(), QStringLiteral("River Ledger"), QStringLiteral("Arrival prose."), QStringLiteral("Outline note"));
    const auto first = EmailSnapshot::create(library.path(), QStringLiteral("b"), snapshots.path(), stamp());
    QVERIFY2(first.ok, qPrintable(first.error));

    writeBook(library.path(), QStringLiteral("River Ledger"), QStringLiteral("Arrival prose, revised."), QStringLiteral("Outline note"));
    const auto second = EmailSnapshot::create(library.path(), QStringLiteral("b"), snapshots.path(), stamp().addSecs(1));
    QVERIFY2(second.ok, qPrintable(second.error));
    QVERIFY(second.fingerprint != first.fingerprint);
}

void EmailSnapshotTest::snapshotSurvivesDraftFailure() {
    QTemporaryDir library, snapshots;
    writeBook(library.path(), QStringLiteral("River Ledger"), QStringLiteral("Arrival prose."), QStringLiteral("Outline note"));
    const auto snapshot = EmailSnapshot::create(library.path(), QStringLiteral("b"), snapshots.path(), stamp());
    QVERIFY2(snapshot.ok, qPrintable(snapshot.error));

    Recorder recorder;
    const auto draft = MailDraft::openDesktopDraft(EmailSnapshot::draftMessage(snapshot),
                                                   recordingStarter(recorder, false), recordingOpener(recorder, false));
    QVERIFY(draft.status == DraftStatus::Failed);
    QVERIFY(!draft.error.isEmpty());

    QVERIFY(QFileInfo::exists(snapshot.pdfPath));
    QVERIFY(QFileInfo::exists(snapshot.textPath));
    QVERIFY(QFileInfo::exists(snapshot.fingerprintPath));
    QCOMPARE(QString::fromLatin1(sha256HexOf(snapshot.textPath)), snapshot.fingerprint);
}

void EmailSnapshotTest::failedPdfWritesNothing() {
    QTemporaryDir library, snapshots;
    // U+10FFFD has no font coverage, so the PDF export fails (same fixture as the manuscript export test).
    writeBook(library.path(), QStringLiteral("River Ledger"),
              QString::fromUtf8("No font has U+10FFFD: \xF4\x8F\xBF\xBD"), QStringLiteral("Outline note"));
    const auto result = EmailSnapshot::create(library.path(), QStringLiteral("b"), snapshots.path(), stamp());
    QVERIFY(!result.ok);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(QDir(snapshots.path()).entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty());
}

void EmailSnapshotTest::existingSnapshotIsNotReplaced() {
    QTemporaryDir library, snapshots;
    writeBook(library.path(), QStringLiteral("River Ledger"), QStringLiteral("Arrival prose."), QStringLiteral("Outline note"));
    const auto first = EmailSnapshot::create(library.path(), QStringLiteral("b"), snapshots.path(), stamp());
    QVERIFY2(first.ok, qPrintable(first.error));

    writeBook(library.path(), QStringLiteral("River Ledger"), QStringLiteral("Arrival prose, revised."), QStringLiteral("Outline note"));
    const auto second = EmailSnapshot::create(library.path(), QStringLiteral("b"), snapshots.path(), stamp());
    QVERIFY(!second.ok);
    QCOMPARE(QString::fromLatin1(sha256HexOf(first.textPath)), first.fingerprint);
}

void EmailSnapshotTest::snapshotInsideLibraryIsRefused() {
    QTemporaryDir library;
    writeBook(library.path(), QStringLiteral("River Ledger"), QStringLiteral("Arrival prose."), QStringLiteral("Outline note"));
    const auto result = EmailSnapshot::create(library.path(), QStringLiteral("b"), library.filePath(QStringLiteral("Snapshots")), stamp());
    QVERIFY(!result.ok);
    QVERIFY(!QFileInfo::exists(library.filePath(QStringLiteral("Snapshots"))));
}

void EmailSnapshotTest::titleCannotEscapeSnapshotFolder() {
    QTemporaryDir library, snapshots;
    writeBook(library.path(), QStringLiteral("../escape/with slash"), QStringLiteral("Arrival prose."), QStringLiteral("Outline note"));
    const auto result = EmailSnapshot::create(library.path(), QStringLiteral("b"), snapshots.path(), stamp());
    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(QFileInfo(result.pdfPath).canonicalPath(), QDir(snapshots.path()).canonicalPath());
    QVERIFY(!QFileInfo(result.pdfPath).fileName().contains(QLatin1Char('/')));
}

void EmailSnapshotTest::draftMessageNeverClaimsSent() {
    SnapshotResult snapshot;
    snapshot.title = QStringLiteral("River Ledger");
    snapshot.pdfPath = QStringLiteral("/synthetic/River Ledger-20261009T123456Z.pdf");
    snapshot.fingerprint = QStringLiteral("00");
    const auto message = EmailSnapshot::draftMessage(snapshot);
    QVERIFY(!message.subject.contains(QStringLiteral("sent"), Qt::CaseInsensitive));
    QVERIFY(!message.body.contains(QStringLiteral("sent"), Qt::CaseInsensitive));
    QVERIFY(message.body.contains(QStringLiteral("00")));
}

void EmailSnapshotTest::desktopDraftUsesAttachmentAndNeverSends() {
    Recorder recorder;
    const auto draft = MailDraft::openDesktopDraft(sampleMessage(), recordingStarter(recorder, true), recordingOpener(recorder, true));
    QVERIFY(draft.status == DraftStatus::Opened);
    QCOMPARE(recorder.programs, QStringList{QStringLiteral("xdg-email")});
    const int attach = recorder.arguments.indexOf(QStringLiteral("--attach"));
    QVERIFY(attach >= 0);
    QCOMPARE(recorder.arguments.at(attach + 1), QStringLiteral("/synthetic/River Ledger-20261009T123456Z.pdf"));
    QVERIFY(!recorder.arguments.contains(QStringLiteral("--send"), Qt::CaseInsensitive));
    QVERIFY(recorder.urls.isEmpty());
}

void EmailSnapshotTest::desktopDraftFallsBackToMailtoAndFolder() {
    Recorder recorder;
    const auto draft = MailDraft::openDesktopDraft(sampleMessage(), recordingStarter(recorder, false), recordingOpener(recorder, true));
    QVERIFY(draft.status == DraftStatus::OpenedWithoutAttachment);
    QCOMPARE(recorder.urls.size(), qsizetype(2));
    QCOMPARE(recorder.urls.at(0).scheme(), QStringLiteral("mailto"));
    QCOMPARE(QUrlQuery(recorder.urls.at(0)).queryItemValue(QStringLiteral("subject")), sampleMessage().subject);
    QCOMPARE(recorder.urls.at(1).scheme(), QStringLiteral("file"));
    QCOMPARE(recorder.urls.at(1).toLocalFile(), QStringLiteral("/synthetic"));
}

void EmailSnapshotTest::desktopDraftFailureIsReported() {
    Recorder recorder;
    const auto draft = MailDraft::openDesktopDraft(sampleMessage(), recordingStarter(recorder, false), recordingOpener(recorder, false));
    QVERIFY(draft.status == DraftStatus::Failed);
    QVERIFY(!draft.error.isEmpty());
    QVERIFY(!draft.error.contains(QStringLiteral("sent"), Qt::CaseInsensitive));
}

void EmailSnapshotTest::gmailDraftOpensComposeWithoutSending() {
    Recorder recorder;
    const auto draft = MailDraft::openGmailDraft(sampleMessage(), recordingOpener(recorder, true));
    QVERIFY(draft.status == DraftStatus::OpenedWithoutAttachment);
    QCOMPARE(recorder.urls.size(), qsizetype(2));
    const QUrl compose = recorder.urls.at(0);
    QCOMPARE(compose.scheme(), QStringLiteral("https"));
    QCOMPARE(compose.host(), QStringLiteral("mail.google.com"));
    const QUrlQuery query(compose);
    QCOMPARE(query.queryItemValue(QStringLiteral("view")), QStringLiteral("cm"));
    QCOMPARE(query.queryItemValue(QStringLiteral("su")), sampleMessage().subject);
    QCOMPARE(query.queryItemValue(QStringLiteral("body")), sampleMessage().body);
    QVERIFY(!query.hasQueryItem(QStringLiteral("send")));
    QCOMPARE(recorder.urls.at(1).scheme(), QStringLiteral("file"));
    QVERIFY(recorder.programs.isEmpty());
}

void EmailSnapshotTest::gmailDraftFailureIsReported() {
    Recorder recorder;
    const auto draft = MailDraft::openGmailDraft(sampleMessage(), recordingOpener(recorder, false));
    QVERIFY(draft.status == DraftStatus::Failed);
    QVERIFY(!draft.error.isEmpty());
}

QTEST_MAIN(EmailSnapshotTest)
#include "email_snapshot_test.moc"
