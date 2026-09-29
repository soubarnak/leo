#include "ai_covers.h"

#include "book_covers.h"
#include "library_persistence.h"
#include "writing_progress.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QSet>
#include <QTextDocument>
#include <QTimer>

namespace {
QSet<QString> activeBooks;
const QString secretService = QStringLiteral("io.github.soubarnak.LeoWriter");

QProcess *runSecretTool(const QStringList &arguments, const QByteArray &input,
                        std::function<void(bool, QByteArray)> done)
{
    auto *process = new QProcess;
    process->setProgram(QStringLiteral("secret-tool"));
    process->setArguments(arguments);
    process->setProcessChannelMode(QProcess::SeparateChannels);
    QObject::connect(process, &QProcess::started, process, [process, input] {
        if (!input.isEmpty()) process->write(input);
        process->closeWriteChannel();
    });
    QObject::connect(process, &QProcess::finished, process,
        [process, done](int code, QProcess::ExitStatus status) {
            const QByteArray result = process->readAllStandardOutput();
            process->deleteLater();
            done(status == QProcess::NormalExit && code == 0, result.trimmed());
        });
    QObject::connect(process, &QProcess::errorOccurred, process,
        [process, done](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                process->deleteLater();
                done(false, {});
            }
        });
    process->start();
    QTimer::singleShot(30000, process, [process] {
        if (process->state() != QProcess::NotRunning) process->kill();
    });
    return process;
}

QString manuscript(const QString &root, const QString &id, QString *error)
{
    QByteArray bytes;
    if (!LibraryPersistence::readLibraryFile(root, id + QStringLiteral("/book.json"), &bytes, error)) return {};
    const QJsonArray order = QJsonDocument::fromJson(bytes).object().value(QStringLiteral("chapterOrder")).toArray();
    QStringList parts;
    for (const QJsonValue &chapter : order) {
        const QString chapterId = chapter.toString();
        if (chapterId.isEmpty() || chapterId.contains(QLatin1Char('/')) ||
            chapterId.contains(QLatin1Char('\\'))) {
            *error = QStringLiteral("Invalid chapter ID.");
            return {};
        }
        if (!LibraryPersistence::readLibraryFile(root, id + QStringLiteral("/chapters/") +
            chapterId + QStringLiteral(".html"), &bytes, error)) return {};
        QTextDocument document;
        document.setHtml(QString::fromUtf8(bytes));
        parts.append(document.toPlainText());
    }
    return parts.join(QStringLiteral("\n\n"));
}

QByteArray requestBody(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}
}

AiCovers::AiCovers(QObject *parent)
    : QObject(parent), network_(new QNetworkAccessManager(this)) {}

void AiCovers::cancel()
{
    cancelled_ = true;
    if (secretProcess_) secretProcess_->kill();
    if (reply_) reply_->abort();
}

void AiCovers::storeKey(const QString &key, Done done)
{
    if (key.isEmpty() || key.contains(QLatin1Char('\n'))) {
        done(false, QStringLiteral("Enter a valid API key."));
        return;
    }
    runSecretTool({QStringLiteral("store"), QStringLiteral("--label=LEO cover art API key"),
                   QStringLiteral("application"), secretService,
                   QStringLiteral("provider"), QStringLiteral("openai")}, key.toUtf8(),
        [done = std::move(done)](bool ok, QByteArray) {
            done(ok, ok ? QStringLiteral("API key saved in the system secret store.")
                        : QStringLiteral("System secret store is unavailable or declined the key."));
        });
}

void AiCovers::generate(const QString &root, const QString &id, Done done)
{
    const QString job = root + QLatin1Char('/') + id;
    if (activeBooks.contains(job)) {
        done(false, QStringLiteral("A cover request for this book is already running."));
        return;
    }
    QString error;
    const QString text = manuscript(root, id, &error);
    if (!error.isEmpty()) { done(false, error); return; }
    if (WritingProgress::countWords(text) < 1000) {
        done(false, QStringLiteral("Write at least 1,000 words before generating cover art."));
        return;
    }
    activeBooks.insert(job);
    auto finish = [job, done = std::move(done)](bool ok, const QString &message) {
        activeBooks.remove(job);
        done(ok, message);
    };
    secretProcess_ = runSecretTool({QStringLiteral("lookup"), QStringLiteral("application"), secretService,
                   QStringLiteral("provider"), QStringLiteral("openai")}, {},
        [this, root, id, text, finish](bool found, QByteArray key) {
            secretProcess_ = nullptr;
            if (cancelled_) {
                finish(false, QStringLiteral("Cover generation cancelled; cover unchanged."));
                return;
            }
            if (!found || key.isEmpty()) {
                finish(false, QStringLiteral("No API key is available from the system secret store."));
                return;
            }
            QNetworkRequest request(QUrl(QStringLiteral("https://api.openai.com/v1/chat/completions")));
            request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
            request.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + key);
            request.setTransferTimeout(60000);
            QStringList words = text.simplified().split(QLatin1Char(' '));
            if (words.size() > 6000) {
                words = words.mid(0, 4500) + QStringList{QStringLiteral("[…]")} + words.mid(words.size() - 1500);
            }
            const QJsonArray messages{
                QJsonObject{{QStringLiteral("role"), QStringLiteral("system")},
                    {QStringLiteral("content"), QStringLiteral("Write one concise image-only book cover illustration brief based on the manuscript. Describe a concrete scene, mood, palette and composition. Request no lettering, logos, or title.")}},
                QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                    {QStringLiteral("content"), words.join(QLatin1Char(' '))}}};
            QNetworkReply *brief = network_->post(request, requestBody({
                {QStringLiteral("model"), QStringLiteral("gpt-5-mini")},
                {QStringLiteral("messages"), messages},
                {QStringLiteral("max_completion_tokens"), 500}}));
            reply_ = brief;
            connect(brief, &QNetworkReply::finished, this,
                [this, brief, key, root, id, finish] {
                    reply_ = nullptr;
                    const bool ok = brief->error() == QNetworkReply::NoError;
                    const QJsonObject response = QJsonDocument::fromJson(brief->readAll()).object();
                    brief->deleteLater();
                    const QString prompt = response.value(QStringLiteral("choices")).toArray()
                        .at(0).toObject().value(QStringLiteral("message")).toObject()
                        .value(QStringLiteral("content")).toString().trimmed();
                    if (cancelled_) {
                        finish(false, QStringLiteral("Cover generation cancelled; cover unchanged."));
                        return;
                    }
                    if (!ok || prompt.isEmpty()) {
                        finish(false, QStringLiteral("The cover brief request failed. Existing cover unchanged."));
                        return;
                    }
                    QNetworkRequest imageRequest(QUrl(QStringLiteral("https://api.openai.com/v1/images/generations")));
                    imageRequest.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
                    imageRequest.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + key);
                    imageRequest.setTransferTimeout(120000);
                    QNetworkReply *painting = network_->post(imageRequest, requestBody({
                        {QStringLiteral("model"), QStringLiteral("gpt-image-1-mini")},
                        {QStringLiteral("prompt"), prompt + QStringLiteral(" Full-bleed portrait book cover illustration. No text, letters, numbers, title, logo or watermark.")},
                        {QStringLiteral("size"), QStringLiteral("1024x1536")},
                        {QStringLiteral("quality"), QStringLiteral("medium")},
                        {QStringLiteral("output_format"), QStringLiteral("png")},
                        {QStringLiteral("n"), 1}}));
                    reply_ = painting;
                    connect(painting, &QNetworkReply::finished, this,
                        [this, painting, root, id, finish] {
                            reply_ = nullptr;
                            const bool ok = painting->error() == QNetworkReply::NoError;
                            const QJsonObject response = QJsonDocument::fromJson(painting->readAll()).object();
                            painting->deleteLater();
                            if (cancelled_) {
                                finish(false, QStringLiteral("Cover generation cancelled; cover unchanged."));
                                return;
                            }
                            if (!ok) {
                                finish(false, QStringLiteral("The image request failed. Existing cover unchanged."));
                                return;
                            }
                            const QByteArray image = QByteArray::fromBase64(response.value(QStringLiteral("data"))
                                .toArray().at(0).toObject().value(QStringLiteral("b64_json")).toString().toLatin1());
                            const CoverResult saved = BookCovers::savePainting(root, id, image);
                            finish(saved.ok, saved.ok ? QStringLiteral("Generated cover saved.") : saved.error);
                        });
                });
        });
}
