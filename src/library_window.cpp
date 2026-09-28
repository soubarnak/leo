#include "library_window.h"

#include "app_paths.h"
#include "legacy_chapter_codec.h"
#include "library_persistence.h"
#include "library_reader.h"
#include "release_check_dialog.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QCursor>
#include <QDesktopServices>
#include <QDir>
#include <QDropEvent>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTextCursor>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QUuid>
#include <QVector>
#include <QVBoxLayout>

#include <functional>
#include <utility>

namespace {

constexpr int ItemKindRole = Qt::UserRole + 1;
constexpr int BookIdRole = Qt::UserRole + 2;
constexpr int ChapterIdRole = Qt::UserRole + 3;
constexpr int ChapterItemKind = 1;

struct ClipboardProvenance {
    QString context;
    QString text;
    int region = -1;

    void clear()
    {
        context.clear();
        text.clear();
        region = -1;
    }
};

struct ProtectedSpan {
    qsizetype start;
    qsizetype end;
};

void addBook(QTreeWidgetItem *parent, const Book &book)
{
    auto *bookItem = new QTreeWidgetItem(parent, {book.title, book.author});
    bookItem->setToolTip(0, book.title);
    bookItem->setToolTip(1, book.author);
    bookItem->setData(0, BookIdRole, book.id);

    for (int index = 0; index < book.chapters.size(); ++index) {
        const Chapter &chapter = book.chapters.at(index);
        QString label = QStringLiteral("Chapter %1").arg(index + 1);
        if (!chapter.title.isEmpty()) {
            label += QStringLiteral(" — ") + chapter.title;
        }
        auto *chapterItem = new QTreeWidgetItem(bookItem, {label});
        chapterItem->setToolTip(0, label);
        chapterItem->setData(0, ItemKindRole, ChapterItemKind);
        chapterItem->setData(0, BookIdRole, book.id);
        chapterItem->setData(0, ChapterIdRole, chapter.id);
    }
}

QString canonicalOrCleanPath(const QString &path)
{
    const QFileInfo info(path);
    const QString canonicalPath = info.canonicalFilePath();
    if (!canonicalPath.isEmpty()) {
        return QDir::cleanPath(canonicalPath);
    }
    const QFileInfo parentInfo(info.absolutePath());
    const QString parentPath = parentInfo.canonicalFilePath().isEmpty()
                                   ? QDir::cleanPath(info.absolutePath())
                                   : parentInfo.canonicalFilePath();
    return QDir::cleanPath(QDir(parentPath).filePath(info.fileName()));
}

bool readJsonArray(const QString &libraryPath,
                   const QString &relativePath,
                   QSet<QString> *ids,
                   QHash<QString, QString> *chapterIds,
                   QString *error)
{
    const QString absolutePath = QDir(libraryPath).filePath(relativePath);
    if (!QFileInfo::exists(absolutePath)) {
        *error = QStringLiteral("%1 is missing.").arg(relativePath);
        return false;
    }

    QByteArray bytes;
    QString readError;
    if (!LibraryPersistence::readLibraryFile(libraryPath, relativePath, &bytes, &readError)) {
        *error = QStringLiteral("%1 could not be read: %2").arg(relativePath, readError);
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
        *error = QStringLiteral("%1 is not a valid JSON array.").arg(relativePath);
        return false;
    }

    const QJsonArray records = document.array();
    for (int index = 0; index < records.size(); ++index) {
        if (!records.at(index).isObject()) {
            *error = QStringLiteral("%1 has an invalid record at position %2.")
                         .arg(relativePath)
                         .arg(index + 1);
            return false;
        }
        const QJsonObject record = records.at(index).toObject();
        const QJsonValue idValue = record.value(QStringLiteral("id"));
        if (!idValue.isString() || idValue.toString().isEmpty()) {
            *error = QStringLiteral("%1 has a record without a valid ID.").arg(relativePath);
            return false;
        }
        const QString id = idValue.toString();
        if (ids->contains(id)) {
            *error = QStringLiteral("%1 contains duplicate ID '%2'.").arg(relativePath, id);
            return false;
        }
        ids->insert(id);
        if (chapterIds) {
            const QJsonValue chapterValue = record.value(QStringLiteral("chapterId"));
            if (!chapterValue.isUndefined() && !chapterValue.isNull() &&
                !chapterValue.isString()) {
                *error = QStringLiteral("%1 has an invalid chapterId for '%2'.")
                             .arg(relativePath, id);
                return false;
            }
            if (chapterValue.isString()) {
                chapterIds->insert(id, chapterValue.toString());
            }
        }
    }
    return true;
}

LegacyChapterLinkContext loadChapterLinks(const QString &libraryPath,
                                          const QString &bookId,
                                          const QString &chapterId)
{
    LegacyChapterLinkContext links;
    links.chapterId = chapterId;
    const QString bookDirectory = bookId + QLatin1Char('/');
    readJsonArray(libraryPath, bookDirectory + QStringLiteral("stickies.json"),
                  &links.stickies.ids, &links.stickies.chapterIds, &links.stickies.readError);
    readJsonArray(libraryPath, bookDirectory + QStringLiteral("darlings.json"),
                  &links.darlings.ids, &links.darlings.chapterIds, &links.darlings.readError);

    const QString metadataPath = bookDirectory + QStringLiteral("book.json");
    QByteArray metadataBytes;
    QString readError;
    if (!LibraryPersistence::readLibraryFile(libraryPath, metadataPath,
                                             &metadataBytes, &readError)) {
        links.sectionReadError = QStringLiteral("%1 could not be read: %2")
                                     .arg(metadataPath, readError);
        return links;
    }
    QJsonParseError parseError;
    const QJsonDocument metadataDocument = QJsonDocument::fromJson(metadataBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !metadataDocument.isObject()) {
        links.sectionReadError = QStringLiteral("%1 is not a valid JSON object.")
                                     .arg(metadataPath);
        return links;
    }

    const QJsonValue sectionNotesValue =
        metadataDocument.object().value(QStringLiteral("sectionNotes"));
    if (sectionNotesValue.isUndefined()) {
        return links;
    }
    if (!sectionNotesValue.isObject()) {
        links.sectionReadError = QStringLiteral("%1 has invalid sectionNotes metadata.")
                                     .arg(metadataPath);
        return links;
    }

    const QJsonValue chapterSections =
        sectionNotesValue.toObject().value(chapterId);
    if (chapterSections.isUndefined()) {
        return links;
    }
    if (!chapterSections.isArray()) {
        links.sectionReadError = QStringLiteral("%1 has invalid section notes for chapter '%2'.")
                                     .arg(metadataPath, chapterId);
        return links;
    }
    const QJsonArray sections = chapterSections.toArray();
    for (int index = 0; index < sections.size(); ++index) {
        if (!sections.at(index).isObject()) {
            links.sectionReadError = QStringLiteral(
                "%1 has an invalid section at position %2 for chapter '%3'.")
                                         .arg(metadataPath)
                                         .arg(index + 1)
                                         .arg(chapterId);
            return links;
        }
        const QJsonValue idValue = sections.at(index).toObject().value(QStringLiteral("id"));
        if (!idValue.isString() || idValue.toString().isEmpty()) {
            links.sectionReadError = QStringLiteral(
                "%1 has a section without a valid ID for chapter '%2'.")
                                         .arg(metadataPath, chapterId);
            return links;
        }
        const QString id = idValue.toString();
        if (links.sectionIds.contains(id)) {
            links.sectionReadError = QStringLiteral(
                "%1 contains duplicate section ID '%2' for chapter '%3'.")
                                         .arg(metadataPath, id, chapterId);
            return links;
        }
        links.sectionIds.insert(id);
    }
    return links;
}

QStringList protectedTokens(const LegacyChapterDocument &document)
{
    QStringList tokens;
    for (const LegacyChapterFragment &fragment : document.fragments) {
        if (fragment.kind == LegacyChapterContentKind::Protected) {
            tokens.append(fragment.token);
        }
    }
    return tokens;
}

}

class ProtectedChapterEditor final : public QPlainTextEdit {
public:
    explicit ProtectedChapterEditor(QWidget *parent = nullptr)
        : QPlainTextEdit(parent)
    {
        connect(this, &QPlainTextEdit::selectionChanged, this, [this] {
            rememberPrimarySelection();
        });
    }

    void setProtectedTokens(const QStringList &tokens)
    {
        protectedTokens_ = tokens;
        protectionContext_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
        copiedSource_.clear();
        primarySelection_.clear();
    }

    void setRefusalHandler(std::function<void()> handler)
    {
        refusalHandler_ = std::move(handler);
    }

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        const QTextCursor cursor = textCursor();
        const bool selectionTouches = selectionTouchesProtected(cursor);
        bool refuse = false;

        if (event->matches(QKeySequence::Copy)) {
            refuse = selectionTouches;
            if (!refuse && cursor.hasSelection() && !protectedTokens_.isEmpty()) {
                copySelectionForProtectedContent();
                event->accept();
                return;
            }
        } else if (event->matches(QKeySequence::Cut)) {
            refuse = selectionTouches;
            if (!refuse && cursor.hasSelection() && !protectedTokens_.isEmpty()) {
                cutSelectionForProtectedContent();
                event->accept();
                return;
            }
        } else if (event->matches(QKeySequence::Paste)) {
            refuse = selectionTouches || cursorInsideProtected(cursor.position());
        } else if (!cursor.hasSelection() && event->key() == Qt::Key_Backspace) {
            refuse = deletesProtected(cursor.position(), true);
        } else if (!cursor.hasSelection() && event->key() == Qt::Key_Delete) {
            refuse = deletesProtected(cursor.position(), false);
        } else if (event->key() == Qt::Key_Backspace || event->key() == Qt::Key_Delete ||
                   (!event->text().isEmpty() &&
                    !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier |
                                             Qt::MetaModifier)))) {
            refuse = selectionTouches || cursorInsideProtected(cursor.position());
        }

        if (refuse) {
            reportRefusal();
            event->accept();
            return;
        }
        QPlainTextEdit::keyPressEvent(event);
    }

    void inputMethodEvent(QInputMethodEvent *event) override
    {
        const QTextCursor cursor = textCursor();
        if (selectionTouchesProtected(cursor) || cursorInsideProtected(cursor.position())) {
            reportRefusal();
            event->accept();
            return;
        }
        QPlainTextEdit::inputMethodEvent(event);
    }

    void insertFromMimeData(const QMimeData *source) override
    {
        const QTextCursor cursor = textCursor();
        const int destination = cursor.hasSelection() ? cursor.selectionStart()
                                                      : cursor.position();
        if (selectionTouchesProtected(cursor) || cursorInsideProtected(cursor.position()) ||
            pasteCrossesProtectedContent(source, destination)) {
            reportRefusal();
            return;
        }
        QPlainTextEdit::insertFromMimeData(source);
    }

    void dropEvent(QDropEvent *event) override
    {
        const QTextCursor selection = textCursor();
        const int dropPosition = cursorForPosition(event->position().toPoint()).position();
        bool refuse = cursorInsideProtected(dropPosition);
        if (event->source() == this) {
            refuse = refuse || selectionTouchesProtected(selection) ||
                     movesSelectionAcrossProtected(selection, dropPosition);
        }
        if (refuse) {
            reportRefusal();
            event->setDropAction(Qt::IgnoreAction);
            event->accept();
            return;
        }
        QPlainTextEdit::dropEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        QMenu *menu = createStandardContextMenu(event->pos());
        if (!protectedTokens_.isEmpty()) {
            for (QAction *action : menu->actions()) {
                if (action->shortcut() == QKeySequence::Cut) {
                    QObject::disconnect(action, nullptr, this, nullptr);
                    connect(action, &QAction::triggered, this, [this] {
                        cutSelectionForProtectedContent();
                    });
                } else if (action->shortcut() == QKeySequence::Copy) {
                    QObject::disconnect(action, nullptr, this, nullptr);
                    connect(action, &QAction::triggered, this, [this] {
                        copySelectionForProtectedContent();
                    });
                }
            }
        }
        menu->exec(event->globalPos());
        delete menu;
    }

private:
    QVector<ProtectedSpan> protectedSpans() const
    {
        QVector<ProtectedSpan> spans;
        const QString text = toPlainText();
        spans.reserve(protectedTokens_.size());
        for (const QString &token : protectedTokens_) {
            const qsizetype start = text.indexOf(token);
            if (start >= 0) {
                spans.append({start, start + token.size()});
            }
        }
        return spans;
    }

    static int protectedRegionAt(qsizetype position, const QVector<ProtectedSpan> &spans)
    {
        int region = 0;
        for (const auto &span : spans) {
            if (span.end <= position) {
                ++region;
            }
        }
        return region;
    }

    void copySelectionForProtectedContent()
    {
        const QTextCursor selection = textCursor();
        if (!selection.hasSelection()) {
            return;
        }
        if (selectionTouchesProtected(selection)) {
            reportRefusal();
            return;
        }
        QMimeData *mime = QPlainTextEdit::createMimeDataFromSelection();
        copiedSource_.text = mime->text();
        copiedSource_.region = protectedRegionAt(selection.selectionStart(), protectedSpans());
        copiedSource_.context = protectionContext_;
        QApplication::clipboard()->setMimeData(mime);
    }

    void rememberPrimarySelection()
    {
        if (protectedTokens_.isEmpty()) {
            return;
        }
        const QTextCursor selection = textCursor();
        if (!selection.hasSelection()) {
            return;
        }
        QMimeData *mime = QPlainTextEdit::createMimeDataFromSelection();
        primarySelection_.context = protectionContext_;
        primarySelection_.text = mime->text();
        primarySelection_.region = selectionTouchesProtected(selection)
                                      ? -1
                                      : protectedRegionAt(selection.selectionStart(),
                                                          protectedSpans());
        delete mime;
    }

    void cutSelectionForProtectedContent()
    {
        const QTextCursor selection = textCursor();
        if (selectionTouchesProtected(selection)) {
            reportRefusal();
            return;
        }
        const int sourceRegion = protectedRegionAt(selection.selectionStart(), protectedSpans());
        QPlainTextEdit::cut();
        copiedSource_.text = QApplication::clipboard()->text();
        copiedSource_.region = sourceRegion;
        copiedSource_.context = protectionContext_;
    }

    bool pasteCrossesProtectedContent(const QMimeData *source, int destination) const
    {
        const int destinationRegion = protectedRegionAt(destination, protectedSpans());
        if (copiedSource_.context == protectionContext_ && !copiedSource_.context.isEmpty() &&
            source->text() == copiedSource_.text && copiedSource_.region >= 0 &&
            copiedSource_.region != destinationRegion) {
            return true;
        }
        QClipboard *clipboard = QApplication::clipboard();
        if (!clipboard->supportsSelection()) {
            return false;
        }
        const QMimeData *primary = clipboard->mimeData(QClipboard::Selection);
        if (!primary || primary->text() != source->text() ||
            primarySelection_.context != protectionContext_ ||
            primarySelection_.text != source->text()) {
            return false;
        }
        return primarySelection_.region < 0 ||
               primarySelection_.region != destinationRegion;
    }

    bool selectionTouchesProtected(const QTextCursor &cursor) const
    {
        if (!cursor.hasSelection()) {
            return false;
        }
        const int selectionStart = cursor.selectionStart();
        const int selectionEnd = cursor.selectionEnd();
        for (const ProtectedSpan &span : protectedSpans()) {
            if (selectionStart < span.end && selectionEnd > span.start) {
                return true;
            }
        }
        return false;
    }

    bool cursorInsideProtected(int position) const
    {
        for (const ProtectedSpan &span : protectedSpans()) {
            if (position > span.start && position < span.end) {
                return true;
            }
        }
        return false;
    }

    bool deletesProtected(int position, bool backward) const
    {
        for (const ProtectedSpan &span : protectedSpans()) {
            if (backward ? position > span.start && position <= span.end
                         : position >= span.start && position < span.end) {
                return true;
            }
        }
        return false;
    }

    bool movesSelectionAcrossProtected(const QTextCursor &selection, int dropPosition) const
    {
        if (!selection.hasSelection()) {
            return false;
        }
        const int selectionStart = selection.selectionStart();
        const int selectionEnd = selection.selectionEnd();
        for (const ProtectedSpan &span : protectedSpans()) {
            if ((selectionEnd <= span.start && dropPosition >= span.end) ||
                (selectionStart >= span.end && dropPosition <= span.start)) {
                return true;
            }
        }
        return false;
    }

    void reportRefusal()
    {
        if (refusalHandler_) {
            refusalHandler_();
        }
    }

    QStringList protectedTokens_;
    QString protectionContext_;
    ClipboardProvenance copiedSource_;
    ClipboardProvenance primarySelection_;
    std::function<void()> refusalHandler_;
};

class HoverFadeFilter final : public QObject {
public:
    explicit HoverFadeFilter(QWidget *container, QObject *parent)
        : QObject(parent), container_(container), effect_(new QGraphicsOpacityEffect(container))
    {
        container_->setGraphicsEffect(effect_);
        effect_->setOpacity(0.0);
        container_->installEventFilter(this);
        for (QWidget *child : container_->findChildren<QWidget *>()) {
            child->installEventFilter(this);
        }
    }

    void setAttention(bool attention)
    {
        attention_ = attention;
        refresh();
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (qobject_cast<QWidget *>(watched)) {
            switch (event->type()) {
            case QEvent::Enter:
            case QEvent::HoverEnter:
            case QEvent::FocusIn:
                refresh();
                break;
            case QEvent::Leave:
            case QEvent::HoverLeave:
            case QEvent::FocusOut:
                QTimer::singleShot(0, this, [this] { refresh(); });
                break;
            default:
                break;
            }
        }
        return QObject::eventFilter(watched, event);
    }

private:
    bool pointerInside() const
    {
        return container_->rect().contains(container_->mapFromGlobal(QCursor::pos()));
    }

    bool focusInside() const
    {
        QWidget *focus = QApplication::focusWidget();
        while (focus) {
            if (focus == container_) {
                return true;
            }
            focus = focus->parentWidget();
        }
        return false;
    }

    void refresh()
    {
        effect_->setOpacity(attention_ || pointerInside() || focusInside() ? 1.0 : 0.0);
    }

    QWidget *container_;
    QGraphicsOpacityEffect *effect_;
    bool attention_ = false;
};

LibraryWindow::LibraryWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("LEO"));
    resize(960, 700);
    defaultPath_ = defaultLibraryPath();

    pages_ = new QStackedWidget(this);
    tree_ = new QTreeWidget(pages_);
    tree_->setObjectName(QStringLiteral("library-tree"));
    tree_->setAccessibleName(QStringLiteral("Library shelves, books, and chapters"));
    tree_->setColumnCount(2);
    tree_->setHeaderLabels({QStringLiteral("Library"), QStringLiteral("Book author")});
    tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree_->setDragEnabled(false);
    tree_->setAcceptDrops(false);
    connect(tree_, &QTreeWidget::itemActivated, this,
            [this](QTreeWidgetItem *item, int) { openChapter(item); });

    editorPage_ = new QWidget(pages_);
    auto *editorLayout = new QVBoxLayout(editorPage_);
    editorChrome_ = new QWidget(editorPage_);
    auto *chromeLayout = new QVBoxLayout(editorChrome_);
    chromeLayout->setContentsMargins(0, 0, 0, 0);
    auto *editorToolbar = new QHBoxLayout;
    auto *backButton = new QPushButton(QStringLiteral("‹ Library"), editorChrome_);
    backButton->setAccessibleName(QStringLiteral("Return to Library"));
    connect(backButton, &QPushButton::clicked, this, [this] {
        if (savePendingEdits()) {
            pages_->setCurrentWidget(tree_);
            statusBar()->showMessage(QStringLiteral("Library open: %1").arg(activeLibraryPath_));
        }
    });
    editorToolbar->addWidget(backButton);
    editorTitle_ = new QLabel(editorChrome_);
    editorTitle_->setObjectName(QStringLiteral("chapter-title"));
    editorTitle_->setAccessibleName(QStringLiteral("Current chapter"));
    editorToolbar->addWidget(editorTitle_, 1);
    saveButton_ = new QPushButton(QStringLiteral("Save"), editorChrome_);
    saveButton_->setObjectName(QStringLiteral("chapter-save"));
    connect(saveButton_, &QPushButton::clicked, this, [this] { saveCurrentChapter(); });
    editorToolbar->addWidget(saveButton_);
    repairCopyButton_ = new QPushButton(QStringLiteral("Save Repair Copy…"), editorChrome_);
    repairCopyButton_->setObjectName(QStringLiteral("chapter-repair-copy"));
    repairCopyButton_->setVisible(false);
    connect(repairCopyButton_, &QPushButton::clicked, this, &LibraryWindow::saveRepairCopy);
    editorToolbar->addWidget(repairCopyButton_);
    chromeLayout->addLayout(editorToolbar);

    editorState_ = new QLabel(editorChrome_);
    editorState_->setObjectName(QStringLiteral("chapter-save-state"));
    editorState_->setWordWrap(true);
    chromeLayout->addWidget(editorState_);
    editorLayout->addWidget(editorChrome_);

    chapterEditor_ = new ProtectedChapterEditor(editorPage_);
    chapterEditor_->setObjectName(QStringLiteral("chapter-editor"));
    chapterEditor_->setAccessibleName(QStringLiteral("Chapter text or read-only source"));
    static_cast<ProtectedChapterEditor *>(chapterEditor_)->setRefusalHandler([this] {
        const QString message = QStringLiteral(
            "Edit refused because protected legacy content could change. Protected source and linked records remain unchanged.");
        updateEditorState(message);
    });
    editorLayout->addWidget(chapterEditor_, 1);
    chromeHoverFilter_ = new HoverFadeFilter(editorChrome_, editorChrome_);
    saveTimer_ = new QTimer(this);
    saveTimer_->setSingleShot(true);
    saveTimer_->setInterval(800);
    connect(saveTimer_, &QTimer::timeout, this, [this] { saveCurrentChapter(); });
    connect(chapterEditor_, &QPlainTextEdit::textChanged, this, [this] {
        if (loadingChapter_ || chapterReadOnly_) {
            return;
        }
        const QString editedText = chapterEditor_->toPlainText();
        QString validationError;
        if (!LegacyChapterCodec::validateEditedText(chapterDocument_, editedText,
                                                    &validationError)) {
            loadingChapter_ = true;
            chapterEditor_->setPlainText(lastValidEditorText_);
            loadingChapter_ = false;
            updateEditorState(validationError);
            return;
        }
        lastValidEditorText_ = editedText;
        chapterDirty_ = true;
        updateEditorState();
        saveTimer_->start();
    });

    refusalPage_ = new QWidget(pages_);
    auto *refusalLayout = new QVBoxLayout(refusalPage_);
    refusalLayout->addStretch();
    refusal_ = new QLabel(refusalPage_);
    refusal_->setObjectName(QStringLiteral("library-refusal"));
    refusal_->setWordWrap(true);
    refusal_->setAlignment(Qt::AlignCenter);
    refusalLayout->addWidget(refusal_);
    auto *chooseAgain = new QPushButton(QStringLiteral("Choose another Library…"), refusalPage_);
    connect(chooseAgain, &QPushButton::clicked, this, &LibraryWindow::chooseLibrary);
    refusalLayout->addWidget(chooseAgain, 0, Qt::AlignHCenter);
    refusalLayout->addStretch();

    pages_->addWidget(tree_);
    pages_->addWidget(editorPage_);
    pages_->addWidget(refusalPage_);
    setCentralWidget(pages_);
    statusBar()->showMessage(QStringLiteral("No Library open"));

    QMenu *fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    QAction *openAction = fileMenu->addAction(QStringLiteral("&Open Library…"));
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &LibraryWindow::chooseLibrary);
    fileMenu->addSeparator();
    QAction *quitAction = fileMenu->addAction(QStringLiteral("E&xit"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    QMenu *helpMenu = menuBar()->addMenu(QStringLiteral("&Help"));
    QAction *updatesAction = helpMenu->addAction(QStringLiteral("Check for Updates…"));
    connect(updatesAction, &QAction::triggered, this, [this] {
        auto *dialog = new ReleaseCheckDialog(QCoreApplication::applicationVersion(), this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
    });
    QAction *errorLogAction = helpMenu->addAction(QStringLiteral("Open Error Log"));
    connect(errorLogAction, &QAction::triggered, this, [] {
        const QString logPath = AppPaths::logFilePath();
        QDir().mkpath(QFileInfo(logPath).absolutePath());
        QFile log(logPath);
        if (!log.exists() && log.open(QIODevice::WriteOnly)) {
            log.close();
        }
        QDesktopServices::openUrl(QUrl::fromLocalFile(logPath));
    });
}

QString LibraryWindow::defaultLibraryPath()
{
    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString documentsPath = documents.isEmpty()
                                      ? QDir::home().filePath(QStringLiteral("Documents"))
                                      : documents;
    return QDir(documentsPath).filePath(QStringLiteral("NEO Library"));
}

QString LibraryWindow::selectLibraryDirectory(QWidget *parent, const QString &startingPath)
{
    return QFileDialog::getExistingDirectory(
        parent, QStringLiteral("Open existing NEO Library"), startingPath,
        QFileDialog::ShowDirsOnly);
}

bool LibraryWindow::openLibrary(const QString &path)
{
    if (!savePendingEdits()) {
        return false;
    }

    const LibraryReadResult result = LibraryReader::read(path);
    tree_->clear();
    if (!result.ok()) {
        qWarning().noquote() << "Library open refused:" << result.error;
        activeLibraryPath_.clear();
        refusal_->setText(QStringLiteral(
            "LEO refused to open this Library.\n\n%1\n\nNo Library files were changed.")
                              .arg(result.error));
        pages_->setCurrentWidget(refusalPage_);
        statusBar()->showMessage(QStringLiteral("No Library open"));
        return false;
    }

    activeLibraryPath_ = result.library.path;
    for (const Author &author : result.library.authors) {
        auto *authorItem = new QTreeWidgetItem(tree_, {author.name});
        for (const Shelf &shelf : author.shelves) {
            auto *shelfItem = new QTreeWidgetItem(authorItem, {shelf.name});
            for (const Book &book : shelf.books) {
                addBook(shelfItem, book);
            }
        }
    }

    if (!result.library.unfiledBooks.isEmpty()) {
        auto *unfiled = new QTreeWidgetItem(tree_, {QStringLiteral("Unfiled books")});
        for (const Book &book : result.library.unfiledBooks) {
            addBook(unfiled, book);
        }
    }

    tree_->expandAll();
    pages_->setCurrentWidget(tree_);
    statusBar()->showMessage(QStringLiteral("Library open: %1").arg(result.library.path));
    return true;
}

bool LibraryWindow::openChapter(QTreeWidgetItem *item)
{
    if (!item || item->data(0, ItemKindRole).toInt() != ChapterItemKind ||
        !savePendingEdits()) {
        return false;
    }

    const QString bookId = item->data(0, BookIdRole).toString();
    const QString chapterId = item->data(0, ChapterIdRole).toString();
    activeChapterRelativePath_ =
        bookId + QStringLiteral("/chapters/") + chapterId + QStringLiteral(".html");
    editorTitle_->setText(item->parent()->text(0) + QStringLiteral(" — ") + item->text(0));
    sourceHash_.clear();
    sourceBytes_.clear();
    sourceAvailable_ = false;
    repairCopyButton_->setVisible(false);
    chapterDirty_ = false;
    saveFailed_ = false;
    chapterReadOnly_ = true;

    QByteArray source;
    QString readError;
    loadingChapter_ = true;
    if (!LibraryPersistence::readLibraryFile(activeLibraryPath_, activeChapterRelativePath_,
                                             &source, &readError)) {
        chapterDocument_ = {};
        chapterLinks_ = {};
        lastValidEditorText_.clear();
        static_cast<ProtectedChapterEditor *>(chapterEditor_)->setProtectedTokens({});
        chapterEditor_->clear();
        editorState_->setText(QStringLiteral("Read-only: %1").arg(readError));
        chapterEditor_->setReadOnly(true);
        saveButton_->setEnabled(false);
        loadingChapter_ = false;
        pages_->setCurrentWidget(editorPage_);
        statusBar()->showMessage(QStringLiteral("Chapter could not be opened safely"));
        return true;
    }

    sourceBytes_ = source;
    sourceAvailable_ = true;
    chapterLinks_ = loadChapterLinks(activeLibraryPath_, bookId, chapterId);
    chapterDocument_ = LegacyChapterCodec::decode(source, chapterLinks_);
    static_cast<ProtectedChapterEditor *>(chapterEditor_)
        ->setProtectedTokens(protectedTokens(chapterDocument_));
    sourceHash_ = LibraryPersistence::hash(source);
    chapterReadOnly_ = !chapterDocument_.editable();
    chapterEditor_->setReadOnly(chapterReadOnly_);
    chapterEditor_->setPlainText(chapterReadOnly_ ? QString::fromUtf8(source)
                                                 : chapterDocument_.text);
    chapterEditor_->moveCursor(QTextCursor::Start);
    loadingChapter_ = false;
    lastValidEditorText_ = chapterEditor_->toPlainText();
    repairCopyButton_->setVisible(chapterReadOnly_);

    if (chapterReadOnly_) {
        editorState_->setText(QStringLiteral("Read-only: %1 The original chapter stays unchanged.")
                                  .arg(chapterDocument_.refusalReason));
    } else if (chapterDocument_.hasProtectedContent()) {
        editorState_->setText(QStringLiteral(
            "Safe prose is editable. Protected legacy content stays unchanged."));
    } else {
        editorState_->setText(QStringLiteral(
            "Plain prose is editable. Changes save after a short pause."));
    }
    saveButton_->setText(QStringLiteral("Save"));
    saveButton_->setEnabled(false);
    pages_->setCurrentWidget(editorPage_);
    statusBar()->showMessage(chapterReadOnly_
                                 ? QStringLiteral("Chapter is read-only")
                                 : QStringLiteral("Chapter open; no Library files changed"));
    if (!chapterReadOnly_) {
        chapterEditor_->setFocus();
    }
    return true;
}

bool LibraryWindow::saveCurrentChapter()
{
    saveTimer_->stop();
    if (!chapterDirty_) {
        return true;
    }
    if (chapterReadOnly_) {
        return false;
    }

    QString encodeError;
    const QByteArray newBytes = LegacyChapterCodec::encode(
        chapterDocument_, chapterEditor_->toPlainText(), &encodeError);
    if (!encodeError.isEmpty()) {
        saveFailed_ = true;
        updateEditorState(encodeError);
        statusBar()->showMessage(QStringLiteral("Unsaved changes — save failed"));
        return false;
    }

    const PersistenceResult result = LibraryPersistence::saveFile(
        activeLibraryPath_, activeChapterRelativePath_, sourceHash_, newBytes);
    if (!result.ok) {
        saveFailed_ = true;
        updateEditorState(result.error);
        statusBar()->showMessage(result.conflict
                                     ? QStringLiteral("Unsaved changes — save paused")
                                     : QStringLiteral("Unsaved changes — save failed"));
        return false;
    }

    sourceHash_ = result.savedHash;
    sourceBytes_ = newBytes;
    chapterDocument_.text = chapterEditor_->toPlainText();
    lastValidEditorText_ = chapterDocument_.text;
    chapterDirty_ = false;
    saveFailed_ = false;
    updateEditorState(chapterDocument_.hasProtectedContent()
                          ? QStringLiteral("Protected legacy content stays unchanged. Changes are saved.")
                          : QStringLiteral("Plain prose is editable. Changes save after a short pause."));
    return true;
}

bool LibraryWindow::savePendingEdits()
{
    if (!chapterDirty_) {
        saveTimer_->stop();
        return true;
    }
    return saveCurrentChapter();
}

void LibraryWindow::saveRepairCopy()
{
    if (!sourceAvailable_ || !chapterReadOnly_) {
        return;
    }

    QFileDialog dialog(this, QStringLiteral("Save a separate repair copy"));
    dialog.setObjectName(QStringLiteral("repair-copy-dialog"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setFileMode(QFileDialog::AnyFile);
    dialog.setNameFilter(QStringLiteral("HTML files (*.html);;All files (*)"));
    dialog.setOption(QFileDialog::DontUseNativeDialog);
    const QString sourceName = QFileInfo(activeChapterRelativePath_).completeBaseName();
    dialog.selectFile(QDir::home().filePath(sourceName + QStringLiteral("-repair.html")));
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) {
        return;
    }

    const QString destinationPath = QFileInfo(dialog.selectedFiles().first()).absoluteFilePath();
    const QString sourcePath = QFileInfo(
        QDir(activeLibraryPath_).filePath(activeChapterRelativePath_)).absoluteFilePath();
    if (canonicalOrCleanPath(destinationPath) == canonicalOrCleanPath(sourcePath)) {
        updateEditorState(QStringLiteral("Choose a separate path. The Library chapter was not changed."));
        return;
    }

    QSaveFile copy(destinationPath);
    if (!copy.open(QIODevice::WriteOnly) || copy.write(sourceBytes_) != sourceBytes_.size() ||
        !copy.commit()) {
        updateEditorState(QStringLiteral("Could not save repair copy: %1").arg(copy.errorString()));
        return;
    }
    updateEditorState(QStringLiteral("Repair copy saved to %1. The Library chapter stays unchanged.")
                          .arg(destinationPath));
    statusBar()->showMessage(QStringLiteral("Separate repair copy saved"));
}

void LibraryWindow::updateEditorState(const QString &message)
{
    chromeHoverFilter_->setAttention(saveFailed_);
    if (!message.isEmpty()) {
        editorState_->setText(chapterDirty_ && saveFailed_
                                  ? QStringLiteral("Unsaved changes — %1 Retry with Save.").arg(message)
                                  : message);
    }
    saveButton_->setEnabled(chapterDirty_ && !chapterReadOnly_);
    saveButton_->setText(saveFailed_ ? QStringLiteral("Retry Save") : QStringLiteral("Save"));
    if (!chapterDirty_) {
        setWindowTitle(QStringLiteral("LEO"));
    } else if (saveFailed_) {
        setWindowTitle(QStringLiteral("LEO — unsaved chapter"));
    }
}

void LibraryWindow::closeEvent(QCloseEvent *event)
{
    if (!savePendingEdits()) {
        event->ignore();
        return;
    }
    event->accept();
}

void LibraryWindow::chooseLibrary()
{
    if (!savePendingEdits()) {
        return;
    }
    const QString path = selectLibraryDirectory(this, defaultPath_);
    if (!path.isEmpty()) {
        openLibrary(path);
    }
}
