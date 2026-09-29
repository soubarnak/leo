#include "library_window.h"

#include "app_paths.h"
#include "font_preferences.h"
#include "legacy_chapter_codec.h"
#include "library_creator.h"
#include "library_persistence.h"
#include "library_reader.h"
#include "release_check_dialog.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
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
#include <QFont>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QInputMethodEvent>
#include <QInputDialog>
#include <QKeyEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStackedWidget>
#include <QStatusBar>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QUuid>
#include <QVector>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <utility>

namespace {

constexpr int ItemKindRole = Qt::UserRole + 1;
constexpr int BookIdRole = Qt::UserRole + 2;
constexpr int ChapterIdRole = Qt::UserRole + 3;
constexpr int AuthorIdRole = Qt::UserRole + 4;
constexpr int ShelfIdRole = Qt::UserRole + 5;
constexpr int ChapterItemKind = 1;
constexpr int OutlineItemKind = 2;

class LibraryTreeWidget final : public QTreeWidget {
public:
    using DropHandler = std::function<void(
        QTreeWidgetItem *, QTreeWidgetItem *, QAbstractItemView::DropIndicatorPosition)>;

    explicit LibraryTreeWidget(QWidget *parent = nullptr) : QTreeWidget(parent) {}

    void setDropHandler(DropHandler handler)
    {
        dropHandler_ = std::move(handler);
    }

protected:
    void startDrag(Qt::DropActions supportedActions) override
    {
        draggedItem_ = currentItem();
        QTreeWidget::startDrag(supportedActions);
        draggedItem_ = nullptr;
    }

    void dropEvent(QDropEvent *event) override
    {
        if (!dropHandler_ || event->source() != this) {
            event->ignore();
            return;
        }
        QTreeWidgetItem *source = draggedItem_ ? draggedItem_ : currentItem();
        QTreeWidgetItem *target = itemAt(event->position().toPoint());
        if (!source || !target) {
            event->ignore();
            return;
        }
        dropHandler_(source, target, dropIndicatorPosition());
        event->acceptProposedAction();
    }

private:
    DropHandler dropHandler_;
    QTreeWidgetItem *draggedItem_ = nullptr;
};

QString deviceHandoffGuidance()
{
    return QStringLiteral(
        "Device handoff: after LEO closes, wait for Syncthing to report Up to Date before opening "
        "this Library "
        "in NEO desktop or Pocket. "
        "Edit this Library on only one device at a time. If LEO detects a competing edit, "
        "it pauses shared saves, preserves your draft, and keeps the other version in the shared Library. "
        "If LEO verifies "
        "a separate Recovered Library, use Switch to Recovered Library to inspect your draft. "
        "LEO cannot detect every legacy write race or prevent sync software from exposing an "
        "intermediate multi-file save.");
}

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

void addBook(QTreeWidgetItem *parent, const Book &book, bool hasOutline)
{
    auto *bookItem = new QTreeWidgetItem(parent, {book.title, book.author});
    bookItem->setToolTip(0, book.title);
    bookItem->setToolTip(1, book.author);
    bookItem->setData(0, BookIdRole, book.id);
    bookItem->setFlags(bookItem->flags() | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);

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
        chapterItem->setFlags(chapterItem->flags() &
                              ~(Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled));
    }
    if (hasOutline) {
        auto *outlineItem = new QTreeWidgetItem(bookItem, {QStringLiteral("Outline")});
        outlineItem->setToolTip(0, QStringLiteral("Open the book outline"));
        outlineItem->setData(0, ItemKindRole, OutlineItemKind);
        outlineItem->setData(0, BookIdRole, book.id);
        outlineItem->setFlags(outlineItem->flags() &
                              ~(Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled));
    }
}

QTreeWidgetItem *findBookItem(QTreeWidgetItem *item, const QString &bookId)
{
    if (item->data(0, ItemKindRole).toInt() == 0 &&
        item->data(0, BookIdRole).toString() == bookId) {
        return item;
    }
    for (int childIndex = 0; childIndex < item->childCount(); ++childIndex) {
        if (QTreeWidgetItem *match = findBookItem(item->child(childIndex), bookId)) {
            return match;
        }
    }
    return nullptr;
}

QTreeWidgetItem *ancestorWithIdRole(QTreeWidgetItem *item, int role)
{
    for (QTreeWidgetItem *current = item; current; current = current->parent()) {
        if (!current->data(0, role).toString().isEmpty()) {
            return current;
        }
    }
    return nullptr;
}

QTreeWidgetItem *shelfAncestor(QTreeWidgetItem *item)
{
    return ancestorWithIdRole(item, ShelfIdRole);
}

QTreeWidgetItem *authorAncestor(QTreeWidgetItem *item)
{
    return ancestorWithIdRole(item, AuthorIdRole);
}

QTreeWidgetItem *bookAncestor(QTreeWidgetItem *item)
{
    for (QTreeWidgetItem *current = item; current; current = current->parent()) {
        if (current->data(0, ItemKindRole).toInt() == 0 &&
            !current->data(0, BookIdRole).toString().isEmpty()) {
            return current;
        }
    }
    return nullptr;
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

class DropCapHighlighter final : public QSyntaxHighlighter {
public:
    explicit DropCapHighlighter(QTextDocument *document)
        : QSyntaxHighlighter(document)
    {
    }

    void setPreferences(const QString &style, const QString &bodyFont)
    {
        style_ = style;
        QFont font(bodyFont);
        font.setPointSizeF(std::max(30.0, font.pointSizeF() * 2.1));
        if (style_ == QStringLiteral("fantasy")) {
            font.setItalic(true);
        } else if (style_ == QStringLiteral("scifi")) {
            font.setWeight(QFont::DemiBold);
        }
        const QString family = FontPreferences::dropCapFamily(style_);
        if (!family.isEmpty()) {
            font.setFamily(family);
        }
        format_.setFont(font);
        rehighlight();
    }

protected:
    void highlightBlock(const QString &text) override
    {
        for (QTextBlock previous = currentBlock().previous(); previous.isValid();
             previous = previous.previous()) {
            if (!previous.text().trimmed().isEmpty()) {
                return;
            }
        }
        qsizetype firstLetter = 0;
        while (firstLetter < text.size() && text.at(firstLetter).isSpace()) {
            ++firstLetter;
        }
        if (firstLetter < text.size()) {
            setFormat(static_cast<int>(firstLetter), 1, format_);
        }
    }

private:
    QString style_ = QStringLiteral("literary");
    QTextCharFormat format_;
};

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

    auto *centralPage = new QWidget(this);
    auto *centralLayout = new QVBoxLayout(centralPage);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    recoveryNotice_ = new QLabel(centralPage);
    recoveryNotice_->setObjectName(QStringLiteral("library-recovery-notice"));
    recoveryNotice_->setWordWrap(true);
    recoveryNotice_->hide();
    centralLayout->addWidget(recoveryNotice_);
    pages_ = new QStackedWidget(centralPage);
    centralLayout->addWidget(pages_, 1);

    welcomePage_ = new QWidget(pages_);
    welcomePage_->setObjectName(QStringLiteral("library-welcome"));
    auto *welcomeLayout = new QVBoxLayout(welcomePage_);
    welcomeLayout->addStretch();
    auto *welcomeTitle = new QLabel(QStringLiteral("Welcome to LEO"), welcomePage_);
    welcomeTitle->setAlignment(Qt::AlignCenter);
    QFont welcomeTitleFont = welcomeTitle->font();
    welcomeTitleFont.setPointSize(welcomeTitleFont.pointSize() + 8);
    welcomeTitle->setFont(welcomeTitleFont);
    welcomeLayout->addWidget(welcomeTitle);
    auto *welcomeCopy = new QLabel(
        QStringLiteral("Create a Library for a new writing project, or open one you already use."),
        welcomePage_);
    welcomeCopy->setAlignment(Qt::AlignCenter);
    welcomeCopy->setWordWrap(true);
    welcomeLayout->addWidget(welcomeCopy);
    auto *newLibraryButton = new QPushButton(QStringLiteral("Create a new Library…"), welcomePage_);
    newLibraryButton->setObjectName(QStringLiteral("new-library-button"));
    newLibraryButton->setDefault(true);
    welcomeLayout->addWidget(newLibraryButton, 0, Qt::AlignHCenter);
    auto *openExistingButton = new QPushButton(QStringLiteral("Open an existing Library…"),
                                               welcomePage_);
    openExistingButton->setObjectName(QStringLiteral("open-existing-library-button"));
    welcomeLayout->addWidget(openExistingButton, 0, Qt::AlignHCenter);
    welcomeLayout->addStretch();
    connect(newLibraryButton, &QPushButton::clicked, this, &LibraryWindow::beginNewLibrary);
    connect(openExistingButton, &QPushButton::clicked, this, &LibraryWindow::chooseLibrary);

    onboardingPage_ = new QWidget(pages_);
    onboardingPage_->setObjectName(QStringLiteral("library-onboarding"));
    auto *onboardingLayout = new QVBoxLayout(onboardingPage_);
    auto *onboardingTitle = new QLabel(QStringLiteral("Set up your writing Library"),
                                       onboardingPage_);
    onboardingTitle->setObjectName(QStringLiteral("onboarding-title"));
    onboardingLayout->addWidget(onboardingTitle);
    auto *onboardingCopy = new QLabel(
        QStringLiteral("These choices are saved in the Library. You can change them later."),
        onboardingPage_);
    onboardingCopy->setWordWrap(true);
    onboardingLayout->addWidget(onboardingCopy);
    auto *onboardingForm = new QFormLayout;
    onboardingAuthor_ = new QLineEdit(onboardingPage_);
    onboardingAuthor_->setObjectName(QStringLiteral("onboarding-author"));
    onboardingAuthor_->setPlaceholderText(QStringLiteral("Anonymous"));
    onboardingForm->addRow(QStringLiteral("Author name"), onboardingAuthor_);
    onboardingMode_ = new QComboBox(onboardingPage_);
    onboardingMode_->setObjectName(QStringLiteral("onboarding-mode"));
    onboardingMode_->addItem(QStringLiteral("Pantser — start with a blank chapter"),
                             QStringLiteral("pantser"));
    onboardingMode_->addItem(QStringLiteral("Plotter — start with an outline"),
                             QStringLiteral("plotter"));
    onboardingForm->addRow(QStringLiteral("Writing mode"), onboardingMode_);
    onboardingBodyFont_ = new QComboBox(onboardingPage_);
    onboardingBodyFont_->setObjectName(QStringLiteral("onboarding-body-font"));
    const QStringList installedFonts = QFontDatabase::families();
    const QStringList preferredFonts{
        QStringLiteral("Georgia"), QStringLiteral("Palatino"), QStringLiteral("Baskerville"),
        QStringLiteral("Cambria"), QStringLiteral("Constantia"),
        QStringLiteral("DejaVu Serif"), QStringLiteral("Liberation Serif"),
        QStringLiteral("Noto Serif")};
    for (const QString &preferredFont : preferredFonts) {
        const auto found = std::find_if(installedFonts.cbegin(), installedFonts.cend(),
                                        [&preferredFont](const QString &installedFont) {
                                            return installedFont.compare(preferredFont,
                                                                         Qt::CaseInsensitive) == 0;
                                        });
        if (found != installedFonts.cend()) {
            onboardingBodyFont_->addItem(*found, *found);
        }
    }
    if (onboardingBodyFont_->count() == 0) {
        const QString systemSerif = FontPreferences::systemSerifFamily();
        onboardingBodyFont_->addItem(systemSerif, systemSerif);
    }
    onboardingForm->addRow(QStringLiteral("Body typeface"), onboardingBodyFont_);
    onboardingDropCap_ = new QComboBox(onboardingPage_);
    onboardingDropCap_->setObjectName(QStringLiteral("onboarding-drop-cap"));
    onboardingDropCap_->addItem(QStringLiteral("Literary"), QStringLiteral("literary"));
    onboardingDropCap_->addItem(QStringLiteral("Fantasy"), QStringLiteral("fantasy"));
    onboardingDropCap_->addItem(QStringLiteral("Sci-Fi"), QStringLiteral("scifi"));
    onboardingForm->addRow(QStringLiteral("Drop-cap style"), onboardingDropCap_);

    onboardingLocation_ = new QLineEdit(defaultPath_, onboardingPage_);
    onboardingLocation_->setObjectName(QStringLiteral("onboarding-location"));
    auto *locationRow = new QWidget(onboardingPage_);
    auto *locationLayout = new QHBoxLayout(locationRow);
    locationLayout->setContentsMargins(0, 0, 0, 0);
    locationLayout->addWidget(onboardingLocation_, 1);
    auto *browseLocation = new QPushButton(QStringLiteral("Choose…"), locationRow);
    browseLocation->setObjectName(QStringLiteral("onboarding-browse"));
    locationLayout->addWidget(browseLocation);
    onboardingForm->addRow(QStringLiteral("New Library location"), locationRow);
    connect(browseLocation, &QPushButton::clicked, this, [this] {
        const QString parent = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Choose a folder for the new Library"),
            QFileInfo(onboardingLocation_->text()).absolutePath(), QFileDialog::ShowDirsOnly);
        if (!parent.isEmpty()) {
            onboardingLocation_->setText(QDir(parent).filePath(QStringLiteral("NEO Library")));
        }
    });
    onboardingLayout->addLayout(onboardingForm);
    onboardingError_ = new QLabel(onboardingPage_);
    onboardingError_->setObjectName(QStringLiteral("onboarding-error"));
    onboardingError_->setWordWrap(true);
    onboardingError_->setStyleSheet(QStringLiteral("color: #a33;"));
    onboardingLayout->addWidget(onboardingError_);
    onboardingLayout->addStretch();
    auto *onboardingButtons = new QHBoxLayout;
    onboardingButtons->addStretch();
    auto *cancelOnboarding = new QPushButton(QStringLiteral("Cancel"), onboardingPage_);
    cancelOnboarding->setObjectName(QStringLiteral("onboarding-cancel"));
    connect(cancelOnboarding, &QPushButton::clicked, this, [this] {
        onboardingError_->clear();
        pages_->setCurrentWidget(onboardingReturnPage_ ? onboardingReturnPage_ : welcomePage_);
    });
    onboardingButtons->addWidget(cancelOnboarding);
    auto *submitOnboarding = new QPushButton(QStringLiteral("Create Library"), onboardingPage_);
    submitOnboarding->setObjectName(QStringLiteral("onboarding-submit"));
    submitOnboarding->setDefault(true);
    connect(submitOnboarding, &QPushButton::clicked, this, &LibraryWindow::createNewLibrary);
    onboardingButtons->addWidget(submitOnboarding);
    onboardingLayout->addLayout(onboardingButtons);

    libraryPage_ = new QWidget(pages_);
    auto *libraryLayout = new QVBoxLayout(libraryPage_);
    auto *libraryTree = new LibraryTreeWidget(libraryPage_);
    tree_ = libraryTree;
    tree_->setObjectName(QStringLiteral("library-tree"));
    tree_->setAccessibleName(QStringLiteral("Library shelves, books, and chapters"));
    tree_->setColumnCount(2);
    tree_->setHeaderLabels({QStringLiteral("Library"), QStringLiteral("Book author")});
    tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree_->setDragEnabled(true);
    tree_->setAcceptDrops(true);
    tree_->viewport()->setAcceptDrops(true);
    tree_->setDragDropMode(QAbstractItemView::InternalMove);
    tree_->setDefaultDropAction(Qt::MoveAction);
    tree_->setDropIndicatorShown(true);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_, &QWidget::customContextMenuRequested,
            this, &LibraryWindow::showOrganizationContextMenu);
    libraryTree->setDropHandler([this](QTreeWidgetItem *source, QTreeWidgetItem *target,
                                       QAbstractItemView::DropIndicatorPosition position) {
        handleLibraryDrop(source, target, position);
    });
    libraryLayout->addWidget(tree_, 1);
    connect(tree_, &QTreeWidget::itemActivated, this,
            [this](QTreeWidgetItem *item, int) {
                if (item && item->data(0, ItemKindRole).toInt() == OutlineItemKind) {
                    openOutline(item);
                } else {
                    openChapter(item);
                }
            });

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
            pages_->setCurrentWidget(libraryPage_);
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
    openRecoveredLibraryButton_ = new QPushButton(
        QStringLiteral("Switch to Recovered Library"), editorChrome_);
    openRecoveredLibraryButton_->setObjectName(QStringLiteral("chapter-open-recovered"));
    openRecoveredLibraryButton_->setVisible(false);
    connect(openRecoveredLibraryButton_, &QPushButton::clicked,
            this, &LibraryWindow::switchToRecoveredLibrary);
    editorToolbar->addWidget(openRecoveredLibraryButton_);
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
    dropCapHighlighter_ = new DropCapHighlighter(chapterEditor_->document());
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
    openRecoveredFromRefusalButton_ = new QPushButton(
        QStringLiteral("Switch to Recovered Library"), refusalPage_);
    openRecoveredFromRefusalButton_->setObjectName(QStringLiteral("library-open-recovered"));
    openRecoveredFromRefusalButton_->setVisible(false);
    connect(openRecoveredFromRefusalButton_, &QPushButton::clicked,
            this, &LibraryWindow::switchToRecoveredLibrary);
    refusalLayout->addWidget(openRecoveredFromRefusalButton_, 0, Qt::AlignHCenter);
    auto *chooseAgain = new QPushButton(QStringLiteral("Choose another Library…"), refusalPage_);
    connect(chooseAgain, &QPushButton::clicked, this, &LibraryWindow::chooseLibrary);
    refusalLayout->addWidget(chooseAgain, 0, Qt::AlignHCenter);
    refusalLayout->addStretch();

    pages_->addWidget(welcomePage_);
    pages_->addWidget(onboardingPage_);
    pages_->addWidget(libraryPage_);
    pages_->addWidget(editorPage_);
    pages_->addWidget(refusalPage_);
    setCentralWidget(centralPage);
    pages_->setCurrentWidget(welcomePage_);
    statusBar()->showMessage(QStringLiteral("No Library open"));

    QMenu *fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    QAction *openAction = fileMenu->addAction(QStringLiteral("&Open Library…"));
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &LibraryWindow::chooseLibrary);
    QAction *newLibraryAction = fileMenu->addAction(QStringLiteral("New Library…"));
    connect(newLibraryAction, &QAction::triggered, this, &LibraryWindow::beginNewLibrary);
    fileMenu->addSeparator();
    QAction *handoffAction = fileMenu->addAction(
        QStringLiteral("Prepare Device Handoff…"));
    connect(handoffAction, &QAction::triggered, this, [this] {
        if (activeLibraryPath_.isEmpty()) {
            QMessageBox::information(
                this, QStringLiteral("No Library open"),
                QStringLiteral("Open a Library in LEO before preparing a device handoff."));
            return;
        }

        if (!savePendingEdits()) {
            QMessageBox::warning(
                this, QStringLiteral("Device handoff is not ready"),
                QStringLiteral("LEO could not finish saving this chapter. Keep LEO open and "
                               "resolve the save problem before handoff.\n\n%1")
                    .arg(deviceHandoffGuidance()));
            return;
        }

        QString saveStatus = QStringLiteral("LEO finished pending chapter saves.");
        if (chapterConflict_) {
            saveStatus = QStringLiteral(
                "LEO saved your draft outside the shared Library. The shared Library remains "
                "unchanged. %1")
                .arg(recoveredLibraryPath_.isEmpty()
                         ? QStringLiteral("No verified Recovered Library is available.")
                         : QStringLiteral("A separate Recovered Library is ready for inspection."));
        }
        saveStatus += QStringLiteral("\n\n");
        QMessageBox handoffDialog(this);
        handoffDialog.setIcon(QMessageBox::Information);
        handoffDialog.setWindowTitle(QStringLiteral("Device handoff"));
        handoffDialog.setText(saveStatus + deviceHandoffGuidance());
        QPushButton *closeButton = handoffDialog.addButton(
            QStringLiteral("Close LEO"), QMessageBox::AcceptRole);
        handoffDialog.addButton(QStringLiteral("Keep LEO Open"), QMessageBox::RejectRole);
        handoffDialog.setDefaultButton(closeButton);
        handoffDialog.exec();
        if (handoffDialog.clickedButton() == closeButton) {
            close();
        }
    });
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

void LibraryWindow::beginNewLibrary()
{
    if (!savePendingEdits()) {
        return;
    }
    if (pages_->currentWidget() != onboardingPage_) {
        onboardingReturnPage_ = pages_->currentWidget();
    }
    onboardingError_->clear();
    onboardingAuthor_->clear();
    onboardingLocation_->setText(defaultPath_);
    onboardingMode_->setCurrentIndex(onboardingMode_->findData(QStringLiteral("pantser")));
    onboardingDropCap_->setCurrentIndex(onboardingDropCap_->findData(QStringLiteral("literary")));
    pages_->setCurrentWidget(onboardingPage_);
    onboardingAuthor_->setFocus();
}

void LibraryWindow::createNewLibrary()
{
    const NewLibraryOptions options{
        onboardingLocation_->text(),
        onboardingAuthor_->text(),
        onboardingMode_->currentData().toString(),
        onboardingBodyFont_->currentData().toString(),
        onboardingDropCap_->currentData().toString()};
    const NewLibraryResult result = LibraryCreator::create(options);
    if (!result.ok) {
        onboardingError_->setText(result.error);
        return;
    }

    if (!openLibrary(result.path)) {
        return;
    }
    if (result.usedPreferenceFallback) {
        const QString fallback = QStringLiteral(
            "One choice was unavailable. LEO saved safe defaults in the new Library.");
        editorState_->setText(editorState_->text() + QStringLiteral(" ") + fallback);
        statusBar()->showMessage(fallback);
    }
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
    organization_.reset();
    activeLibraryPath_.clear();
    if (canonicalOrCleanPath(path) != canonicalOrCleanPath(recoveredLibraryPath_)) {
        recoveredLibraryPath_.clear();
        openRecoveredLibraryButton_->hide();
        openRecoveredFromRefusalButton_->hide();
    }

    const PersistenceResult recovery = LibraryPersistence::recoverPendingSaves(path);
    recoveredLibraryPath_ = recovery.recoveredLibraryPath;
    openRecoveredLibraryButton_->hide();
    openRecoveredFromRefusalButton_->setVisible(!recoveredLibraryPath_.isEmpty());
    recoveryNotice_->hide();
    tree_->clear();
    if (!recovery.ok) {
        qWarning().noquote() << "Library save recovery paused:" << recovery.error;
        activeLibraryPath_.clear();
        const QString recoveredMessage = recovery.recovered
            ? QStringLiteral("LEO recovered an earlier interrupted save, then paused this Library.")
            : QStringLiteral("LEO paused this Library while recovering an interrupted save.");
        refusal_->setText(QStringLiteral(
            "%1\n\n%2\n\n%3\n\nThe unresolved save journal and shared Library bytes remain available for inspection.")
                              .arg(recoveredMessage, recovery.error, deviceHandoffGuidance()));
        pages_->setCurrentWidget(refusalPage_);
        statusBar()->showMessage(QStringLiteral("Library save recovery paused for inspection"));
        return false;
    }

    const LibraryReadResult beforeOrganizationRecovery = LibraryReader::read(path);
    if (!beforeOrganizationRecovery.ok()) {
        qWarning().noquote() << "Library open refused:" << beforeOrganizationRecovery.error;
        activeLibraryPath_.clear();
        refusal_->setText(QStringLiteral(
            "LEO refused to open this Library. It made no Library changes.\n\n%1\n\n%2")
                              .arg(beforeOrganizationRecovery.error, deviceHandoffGuidance()));
        pages_->setCurrentWidget(refusalPage_);
        statusBar()->showMessage(QStringLiteral("No Library open"));
        return false;
    }

    auto organization = std::make_unique<LibraryOrganization>(
        beforeOrganizationRecovery.library.path);
    QString organizationError;
    if (!organization->load(&organizationError)) {
        qWarning().noquote() << "Library organization recovery paused:" << organizationError;
        activeLibraryPath_.clear();
        refusal_->setText(QStringLiteral(
            "LEO could not recover or prepare safe Library organization changes. It made no "
            "partial Library change.\n\n%1\n\n%2")
                              .arg(organizationError, deviceHandoffGuidance()));
        pages_->setCurrentWidget(refusalPage_);
        statusBar()->showMessage(QStringLiteral("Library organization recovery paused"));
        return false;
    }

    const LibraryReadResult result = LibraryReader::read(path);
    if (!result.ok()) {
        qWarning().noquote() << "Library open refused:" << result.error;
        activeLibraryPath_.clear();
        refusal_->setText(QStringLiteral(
            "LEO refused to open this Library. It made no Library changes.\n\n%1\n\n%2")
                              .arg(result.error, deviceHandoffGuidance()));
        pages_->setCurrentWidget(refusalPage_);
        statusBar()->showMessage(QStringLiteral("No Library open"));
        return false;
    }

    if (recovery.recovered) {
        recoveryNotice_->setText(QStringLiteral(
            "LEO recovered an interrupted save. Review the recovered chapter before continuing."));
        recoveryNotice_->show();
    }

    activeLibraryPath_ = result.library.path;
    organization_ = std::move(organization);
    applyPreferences(result.library.preferences);
    activeDocumentIsOutline_ = false;
    populateLibraryTree(result.library);
    pages_->setCurrentWidget(libraryPage_);
    QString status = recovery.recovered
                         ? QStringLiteral("Interrupted save recovered; review chapter")
                         : QStringLiteral("Library open: %1").arg(result.library.path);
    if (!preferenceNotice_.isEmpty()) {
        status += QStringLiteral(" — ") + preferenceNotice_;
    }
    statusBar()->showMessage(status);

    if (!result.library.preferences.initialBookId.isEmpty()) {
        QTreeWidgetItem *initialBook = nullptr;
        for (int itemIndex = 0; itemIndex < tree_->topLevelItemCount() && !initialBook;
             ++itemIndex) {
            initialBook = findBookItem(tree_->topLevelItem(itemIndex),
                                       result.library.preferences.initialBookId);
        }
        if (initialBook) {
            const int preferredKind = activePreferences_.writingStyle ==
                                              QStringLiteral("plotter")
                                          ? OutlineItemKind
                                          : ChapterItemKind;
            QTreeWidgetItem *startingDocument = nullptr;
            for (int childIndex = 0; childIndex < initialBook->childCount(); ++childIndex) {
                QTreeWidgetItem *child = initialBook->child(childIndex);
                if (child->data(0, ItemKindRole).toInt() == preferredKind) {
                    startingDocument = child;
                    break;
                }
            }
            if (!startingDocument && initialBook->childCount() > 0) {
                startingDocument = initialBook->child(0);
            }
            if (startingDocument) {
                if (startingDocument->data(0, ItemKindRole).toInt() == OutlineItemKind) {
                    openOutline(startingDocument);
                } else {
                    openChapter(startingDocument);
                }
            }
        }
    }
    return true;
}

void LibraryWindow::populateLibraryTree(const Library &library)
{
    tree_->clear();
    const auto addBooks = [&library](QTreeWidgetItem *parent, const QVector<Book> &books) {
        for (const Book &book : books) {
            const QString outlinePath = QDir(library.path)
                                            .filePath(book.id + QStringLiteral("/outline.html"));
            addBook(parent, book, QFileInfo::exists(outlinePath));
        }
    };
    for (const Author &author : library.authors) {
        auto *authorItem = new QTreeWidgetItem(tree_, {author.name});
        authorItem->setData(0, AuthorIdRole, author.id);
        authorItem->setFlags((authorItem->flags() & ~Qt::ItemIsDragEnabled) |
                             Qt::ItemIsDropEnabled);
        for (const Shelf &shelf : author.shelves) {
            auto *shelfItem = new QTreeWidgetItem(authorItem, {shelf.name});
            shelfItem->setData(0, ShelfIdRole, shelf.id);
            shelfItem->setFlags(shelfItem->flags() | Qt::ItemIsDragEnabled |
                                Qt::ItemIsDropEnabled);
            addBooks(shelfItem, shelf.books);
        }
    }

    if (!library.unfiledBooks.isEmpty()) {
        auto *unfiled = new QTreeWidgetItem(tree_, {QStringLiteral("Unfiled books")});
        unfiled->setFlags(unfiled->flags() &
                          ~(Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled));
        addBooks(unfiled, library.unfiledBooks);
    }
    tree_->expandAll();
}

bool LibraryWindow::refreshOrganizationView()
{
    const QString path = activeLibraryPath_;
    if (path.isEmpty() || !openLibrary(path)) {
        return false;
    }
    pages_->setCurrentWidget(libraryPage_);
    tree_->setFocus();
    return true;
}

void LibraryWindow::finishOrganizationChange(const LibraryOrganizationResult &result,
                                            const QString &successMessage)
{
    const bool refreshed = refreshOrganizationView();
    if (!result.ok) {
        if (!refreshed) {
            organization_.reset();
        }
        QMessageBox::warning(
            this, QStringLiteral("Library change paused"),
            QStringLiteral("%1\n\nLEO reopened the Library to recover or inspect its saved state.")
                .arg(result.error));
        return;
    }
    if (!refreshed) {
        QMessageBox::warning(
            this, QStringLiteral("Library needs inspection"),
            QStringLiteral("The change was saved, but LEO could not reopen the Library. Open it again to inspect the saved state."));
        return;
    }
    statusBar()->showMessage(successMessage, 5000);
}

void LibraryWindow::createBookFromSelection()
{
    if (!organization_) {
        return;
    }

    QTreeWidgetItem *selected = tree_->currentItem();
    QTreeWidgetItem *shelf = shelfAncestor(selected);
    if (!shelf && selected && selected->data(0, AuthorIdRole).isValid() &&
        selected->childCount() > 0) {
        shelf = selected->child(0);
    }
    if (!shelf) {
        const QJsonArray shelves = organization_->metadata()
                                       .value(QStringLiteral("shelves")).toArray();
        if (!shelves.isEmpty()) {
            const QString shelfId = shelves.first().toObject()
                                        .value(QStringLiteral("id")).toString();
            for (int authorIndex = 0; authorIndex < tree_->topLevelItemCount() && !shelf;
                 ++authorIndex) {
                QTreeWidgetItem *author = tree_->topLevelItem(authorIndex);
                for (int shelfIndex = 0; shelfIndex < author->childCount(); ++shelfIndex) {
                    QTreeWidgetItem *candidate = author->child(shelfIndex);
                    if (candidate->data(0, ShelfIdRole).toString() == shelfId) {
                        shelf = candidate;
                        break;
                    }
                }
            }
        }
    }
    if (!shelf) {
        QMessageBox::information(this, QStringLiteral("Add a shelf first"),
                                 QStringLiteral("Create a shelf before adding a book."));
        return;
    }

    bool accepted = false;
    const QString title = QInputDialog::getText(
        this, QStringLiteral("New book"), QStringLiteral("Book title:"),
        QLineEdit::Normal, QStringLiteral("Untitled"), &accepted);
    if (!accepted) {
        return;
    }
    finishOrganizationChange(
        organization_->createBook(shelf->data(0, ShelfIdRole).toString(), title),
        QStringLiteral("Book created."));
}

void LibraryWindow::manageAuthor(const QString &authorId, const QPoint &globalPosition)
{
    if (!organization_ || authorId.isEmpty()) {
        return;
    }
    QTreeWidgetItem *authorItem = nullptr;
    for (int index = 0; index < tree_->topLevelItemCount(); ++index) {
        QTreeWidgetItem *item = tree_->topLevelItem(index);
        if (item->data(0, AuthorIdRole).toString() == authorId) {
            authorItem = item;
            break;
        }
    }
    if (!authorItem) {
        return;
    }

    QMenu menu(this);
    const QString name = authorItem->text(0);
    const QString currentId = organization_->metadata()
                                  .value(QStringLiteral("currentAuthorId")).toString();
    QAction *writeAs = menu.addAction(currentId == authorId
                                          ? QStringLiteral("Currently writing as %1").arg(name)
                                          : QStringLiteral("Write as %1").arg(name));
    writeAs->setEnabled(currentId != authorId);
    QAction *rename = menu.addAction(QStringLiteral("Rename %1…").arg(name));
    QAction *newShelf = menu.addAction(QStringLiteral("Add a shelf…"));
    QAction *add = menu.addAction(QStringLiteral("Add a pen name…"));
    QAction *remove = nullptr;
    if (tree_->topLevelItemCount() > 1) {
        remove = menu.addAction(QStringLiteral("Remove %1…").arg(name));
    }
    QAction *chosen = menu.exec(globalPosition);
    if (!chosen) {
        return;
    }

    if (chosen == writeAs) {
        finishOrganizationChange(organization_->setCurrentAuthor(authorId),
                                 QStringLiteral("Writing as %1.").arg(name));
        return;
    }
    if (chosen == rename) {
        bool accepted = false;
        const QString updatedName = QInputDialog::getText(
            this, QStringLiteral("Rename pen name"), QStringLiteral("Name:"),
            QLineEdit::Normal, name, &accepted);
        if (accepted) {
            finishOrganizationChange(organization_->renameAuthor(authorId, updatedName),
                                     QStringLiteral("Pen name renamed."));
        }
        return;
    }
    if (chosen == newShelf) {
        bool accepted = false;
        const QString shelfName = QInputDialog::getText(
            this, QStringLiteral("New shelf"), QStringLiteral("Shelf name:"),
            QLineEdit::Normal, QStringLiteral("New Shelf"), &accepted);
        if (accepted) {
            finishOrganizationChange(organization_->addShelf(authorId, shelfName),
                                     QStringLiteral("Shelf created."));
        }
        return;
    }
    if (chosen == add) {
        bool accepted = false;
        const QString newName = QInputDialog::getText(
            this, QStringLiteral("Add a pen name"), QStringLiteral("Name:"),
            QLineEdit::Normal, QString(), &accepted);
        if (accepted) {
            finishOrganizationChange(organization_->addAuthor(newName),
                                     QStringLiteral("Pen name added with a new shelf."));
        }
        return;
    }
    if (chosen != remove) {
        return;
    }

    QStringList targetLabels;
    QStringList targetIds;
    for (int index = 0; index < tree_->topLevelItemCount(); ++index) {
        QTreeWidgetItem *candidate = tree_->topLevelItem(index);
        const QString id = candidate->data(0, AuthorIdRole).toString();
        if (id == authorId || id.isEmpty()) {
            continue;
        }
        targetIds.append(id);
        targetLabels.append(QStringLiteral("%1 (%2)").arg(candidate->text(0), id));
    }
    if (targetLabels.isEmpty()) {
        return;
    }
    bool accepted = false;
    const QString target = QInputDialog::getItem(
        this, QStringLiteral("Reassign books"),
        QStringLiteral("Move this pen name’s shelves to:"), targetLabels, 0, false,
        &accepted);
    if (!accepted) {
        return;
    }
    const int targetIndex = targetLabels.indexOf(target);
    if (targetIndex < 0 ||
        QMessageBox::question(
            this, QStringLiteral("Remove pen name"),
            QStringLiteral("Its shelves and books will move to %1. Book folders remain in the Library.")
                .arg(targetLabels.at(targetIndex)),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }
    finishOrganizationChange(organization_->removeAuthor(authorId, targetIds.at(targetIndex)),
                             QStringLiteral("Pen name removed; its books were kept."));
}

void LibraryWindow::showOrganizationContextMenu(const QPoint &position)
{
    if (!organization_) {
        return;
    }
    QTreeWidgetItem *item = tree_->itemAt(position);
    if (!item) {
        return;
    }
    tree_->setCurrentItem(item);

    const QString authorId = item->data(0, AuthorIdRole).toString();
    if (!authorId.isEmpty()) {
        manageAuthor(authorId, tree_->viewport()->mapToGlobal(position));
        return;
    }

    const QString shelfId = item->data(0, ShelfIdRole).toString();
    if (!shelfId.isEmpty()) {
        QMenu menu(this);
        QAction *newBook = menu.addAction(QStringLiteral("Add a book…"));
        QAction *rename = menu.addAction(QStringLiteral("Rename shelf…"));
        QAction *remove = menu.addAction(QStringLiteral("Delete shelf…"));
        QAction *chosen = menu.exec(tree_->viewport()->mapToGlobal(position));
        if (chosen == newBook) {
            createBookFromSelection();
        } else if (chosen == rename) {
            bool accepted = false;
            const QString name = QInputDialog::getText(
                this, QStringLiteral("Rename shelf"), QStringLiteral("Shelf name:"),
                QLineEdit::Normal, item->text(0), &accepted);
            if (accepted) {
                finishOrganizationChange(organization_->renameShelf(shelfId, name),
                                         QStringLiteral("Shelf renamed."));
            }
        } else if (chosen == remove &&
                   QMessageBox::question(
                       this, QStringLiteral("Delete shelf"),
                       QStringLiteral("Books on this shelf will move to another shelf under the same pen name."),
                       QMessageBox::Yes | QMessageBox::Cancel,
                       QMessageBox::Cancel) == QMessageBox::Yes) {
            finishOrganizationChange(organization_->removeShelf(shelfId),
                                     QStringLiteral("Shelf deleted; its books were kept."));
        }
        return;
    }

    if (item->data(0, ItemKindRole).toInt() != 0) {
        return;
    }
    const QString bookId = item->data(0, BookIdRole).toString();
    if (bookId.isEmpty()) {
        return;
    }
    QMenu menu(this);
    QAction *rename = menu.addAction(QStringLiteral("Rename book…"));
    QAction *remove = menu.addAction(QStringLiteral("Remove from shelves"));
    QAction *trash = menu.addAction(QStringLiteral("Move to Trash…"));
    QAction *chosen = menu.exec(tree_->viewport()->mapToGlobal(position));
    if (chosen == rename) {
        bool accepted = false;
        const QString title = QInputDialog::getText(
            this, QStringLiteral("Rename book"), QStringLiteral("Book title:"),
            QLineEdit::Normal, item->text(0), &accepted);
        if (accepted) {
            finishOrganizationChange(organization_->renameBook(bookId, title),
                                     QStringLiteral("Book renamed."));
        }
    } else if (chosen == remove &&
               QMessageBox::question(
                   this, QStringLiteral("Remove from shelves"),
                   QStringLiteral("The book folder will stay in the Library and appear under Unfiled books."),
                   QMessageBox::Yes | QMessageBox::Cancel,
                   QMessageBox::Cancel) == QMessageBox::Yes) {
        finishOrganizationChange(organization_->removeBookFromShelves(bookId),
                                 QStringLiteral("Book removed from shelves."));
    } else if (chosen == trash &&
               QMessageBox::question(
                   this, QStringLiteral("Move book to Trash"),
                   QStringLiteral("Move the complete book folder to the system Trash?"),
                   QMessageBox::Yes | QMessageBox::Cancel,
                   QMessageBox::Cancel) == QMessageBox::Yes) {
        finishOrganizationChange(organization_->moveBookToTrash(bookId),
                                 QStringLiteral("Book moved to Trash."));
    }
}

void LibraryWindow::handleLibraryDrop(
    QTreeWidgetItem *source, QTreeWidgetItem *target,
    QAbstractItemView::DropIndicatorPosition position)
{
    if (!organization_ || !source || !target || source == target ||
        source->data(0, ItemKindRole).toInt() != 0) {
        return;
    }

    const QString shelfId = source->data(0, ShelfIdRole).toString();
    if (!shelfId.isEmpty()) {
        if (shelfAncestor(target) == source) {
            return;
        }
        QTreeWidgetItem *sourceAuthor = authorAncestor(source);
        QTreeWidgetItem *destinationAuthor = authorAncestor(target);
        QTreeWidgetItem *targetShelf = shelfAncestor(target);
        if (!sourceAuthor || !destinationAuthor || sourceAuthor != destinationAuthor) {
            return;
        }
        int index = 0;
        const int childCount = destinationAuthor->childCount();
        if (target == destinationAuthor) {
            for (int childIndex = 0; childIndex < childCount; ++childIndex) {
                if (!destinationAuthor->child(childIndex)->data(0, ShelfIdRole).toString().isEmpty() &&
                    destinationAuthor->child(childIndex) != source) {
                    ++index;
                }
            }
        } else if (targetShelf) {
            for (int childIndex = 0; childIndex < childCount; ++childIndex) {
                QTreeWidgetItem *candidate = destinationAuthor->child(childIndex);
                if (candidate == source || candidate->data(0, ShelfIdRole).toString().isEmpty()) {
                    continue;
                }
                if (candidate == targetShelf) {
                    if (position == QAbstractItemView::AboveItem) {
                        break;
                    }
                    ++index;
                    break;
                }
                ++index;
            }
        } else {
            return;
        }
        finishOrganizationChange(
            organization_->moveShelf(shelfId,
                                     destinationAuthor->data(0, AuthorIdRole).toString(), index),
            QStringLiteral("Shelf moved."));
        return;
    }

    const QString bookId = source->data(0, BookIdRole).toString();
    if (bookId.isEmpty()) {
        return;
    }
    QTreeWidgetItem *destinationShelf = shelfAncestor(target);
    if (!destinationShelf && !authorAncestor(target)) {
        return;
    }
    if (!destinationShelf) {
        QTreeWidgetItem *destinationAuthor = authorAncestor(target);
        for (int index = 0; destinationAuthor && index < destinationAuthor->childCount(); ++index) {
            if (!destinationAuthor->child(index)->data(0, ShelfIdRole).toString().isEmpty()) {
                destinationShelf = destinationAuthor->child(index);
                break;
            }
        }
    }
    if (!destinationShelf) {
        return;
    }

    QTreeWidgetItem *targetBook = bookAncestor(target);
    if (targetBook == source) {
        return;
    }
    int index = 0;
    for (int childIndex = 0; childIndex < destinationShelf->childCount(); ++childIndex) {
        QTreeWidgetItem *candidate = destinationShelf->child(childIndex);
        const QString candidateId = candidate->data(0, BookIdRole).toString();
        if (candidate == source || candidateId.isEmpty() ||
            candidate->data(0, ItemKindRole).toInt() != 0) {
            continue;
        }
        if (candidate == targetBook) {
            if (position == QAbstractItemView::AboveItem) {
                break;
            }
            ++index;
            break;
        }
        ++index;
    }
    finishOrganizationChange(
        organization_->moveBook(bookId,
                                destinationShelf->data(0, ShelfIdRole).toString(), index),
        QStringLiteral("Book moved."));
}

bool LibraryWindow::openChapter(QTreeWidgetItem *item)
{
    if (!item || item->data(0, ItemKindRole).toInt() != ChapterItemKind) {
        return false;
    }
    const QString bookId = item->data(0, BookIdRole).toString();
    const QString chapterId = item->data(0, ChapterIdRole).toString();
    const QString relativePath = bookId + QStringLiteral("/chapters/") + chapterId +
                                 QStringLiteral(".html");
    const QString title = item->parent()->text(0) + QStringLiteral(" — ") + item->text(0);
    return openDocument(relativePath, title,
                        loadChapterLinks(activeLibraryPath_, bookId, chapterId), false);
}

bool LibraryWindow::openOutline(QTreeWidgetItem *item)
{
    if (!item || item->data(0, ItemKindRole).toInt() != OutlineItemKind) {
        return false;
    }
    const QString bookId = item->data(0, BookIdRole).toString();
    const QString relativePath = bookId + QStringLiteral("/outline.html");
    const QString title = item->parent()->text(0) + QStringLiteral(" — Outline");
    return openDocument(relativePath, title, {}, true);
}

bool LibraryWindow::openDocument(const QString &relativePath,
                                 const QString &title,
                                 const LegacyChapterLinkContext &links,
                                 bool outline)
{
    if (!savePendingEdits()) {
        return false;
    }
    recoveryNotice_->hide();
    activeChapterRelativePath_ = relativePath;
    activeDocumentIsOutline_ = outline;
    editorTitle_->setText(title);
    sourceHash_.clear();
    sourceBytes_.clear();
    sourceAvailable_ = false;
    repairCopyButton_->setVisible(false);
    chapterDirty_ = false;
    chapterConflict_ = false;
    conflictDraftPath_.clear();
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
        saveFailed_ = true;
        updateEditorState(QStringLiteral("Read-only: %1 %2")
                              .arg(readError, deviceHandoffGuidance()));
        chapterEditor_->setReadOnly(true);
        saveButton_->setEnabled(false);
        loadingChapter_ = false;
        pages_->setCurrentWidget(editorPage_);
        statusBar()->showMessage(outline ? QStringLiteral("Outline could not be opened safely")
                                         : QStringLiteral("Chapter could not be opened safely"));
        return true;
    }

    sourceBytes_ = source;
    sourceAvailable_ = true;
    chapterLinks_ = links;
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

    QString stateMessage;
    if (chapterReadOnly_) {
        stateMessage = QStringLiteral("Read-only: %1 The original %2 stays unchanged.")
                           .arg(chapterDocument_.refusalReason,
                                outline ? QStringLiteral("outline")
                                        : QStringLiteral("chapter"));
    } else if (chapterDocument_.hasProtectedContent()) {
        stateMessage = QStringLiteral(
            "Safe prose is editable. Protected legacy content stays unchanged.");
    } else {
        stateMessage = outline
                           ? QStringLiteral("Outline text is editable. Changes save after a short pause.")
                           : QStringLiteral("Plain prose is editable. Changes save after a short pause.");
    }
    if (!preferenceNotice_.isEmpty()) {
        stateMessage += QStringLiteral(" ") + preferenceNotice_;
    }
    if (!stateMessage.isEmpty()) {
        editorState_->setText(stateMessage);
    }
    saveButton_->setText(QStringLiteral("Save"));
    saveButton_->setEnabled(false);
    pages_->setCurrentWidget(editorPage_);
    statusBar()->showMessage(chapterReadOnly_
                                 ? (outline ? QStringLiteral("Outline is read-only")
                                            : QStringLiteral("Chapter is read-only"))
                                 : (outline ? QStringLiteral("Outline open; no Library files changed")
                                            : QStringLiteral("Chapter open; no Library files changed")));
    if (!chapterReadOnly_) {
        chapterEditor_->setFocus();
    }
    return true;
}

void LibraryWindow::applyPreferences(const LibraryPreferences &preferences)
{
    activePreferences_ = preferences;
    preferenceNotice_.clear();
    QStringList notices;

    if (activePreferences_.writingStyleInvalid ||
        (activePreferences_.writingStyle != QStringLiteral("pantser") &&
         activePreferences_.writingStyle != QStringLiteral("plotter"))) {
        activePreferences_.writingStyle = QStringLiteral("pantser");
        notices.append(QStringLiteral("Unknown writing mode; using Pantser."));
    }

        const QString systemSerif = FontPreferences::systemSerifFamily();
    QString bodyFont = activePreferences_.bodyFont.trimmed();
    if (activePreferences_.bodyFontInvalid) {
        notices.append(QStringLiteral("Saved typeface choice is invalid; using '%1'.")
                           .arg(systemSerif));
        bodyFont = systemSerif;
    } else if (!bodyFont.isEmpty()) {
        const QString installed = FontPreferences::installedFamily({bodyFont});
        if (installed.isEmpty()) {
            notices.append(QStringLiteral("Saved typeface '%1' is unavailable; using '%2'.")
                               .arg(bodyFont, systemSerif));
            bodyFont = systemSerif;
        } else {
            bodyFont = installed;
        }
    } else {
        bodyFont = systemSerif;
    }
    QFont editorFont = chapterEditor_->font();
    editorFont.setFamily(bodyFont);
    chapterEditor_->setFont(editorFont);

    if (activePreferences_.dropCapStyleInvalid ||
        (activePreferences_.dropCapStyle != QStringLiteral("literary") &&
         activePreferences_.dropCapStyle != QStringLiteral("fantasy") &&
         activePreferences_.dropCapStyle != QStringLiteral("scifi"))) {
        activePreferences_.dropCapStyle = QStringLiteral("literary");
        notices.append(QStringLiteral("Unknown drop-cap choice; using Literary."));
    } else if (FontPreferences::dropCapFamily(activePreferences_.dropCapStyle).isEmpty()) {
        notices.append(QStringLiteral("Drop-cap typeface is unavailable; using the body typeface."));
    }
    static_cast<DropCapHighlighter *>(dropCapHighlighter_)
        ->setPreferences(activePreferences_.dropCapStyle, bodyFont);
    preferenceNotice_ = notices.join(QLatin1Char(' '));
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

    if (chapterConflict_) {
        const PersistenceResult result = LibraryPersistence::updateConflictDraft(
            activeLibraryPath_, activeChapterRelativePath_, conflictDraftPath_,
            recoveredLibraryPath_, newBytes);
        if (!result.ok) {
            saveFailed_ = true;
            conflictDraftPath_ = result.conflictDraftPath;
            recoveredLibraryPath_ = result.recoveredLibraryPath;
            openRecoveredLibraryButton_->setVisible(!recoveredLibraryPath_.isEmpty());
            updateEditorState(result.error + QStringLiteral(" ") + deviceHandoffGuidance());
            statusBar()->showMessage(QStringLiteral("Local draft save failed; shared save remains paused"));
            return false;
        }

        chapterDirty_ = false;
        conflictDraftPath_ = result.conflictDraftPath;
        recoveredLibraryPath_ = result.recoveredLibraryPath;
        saveFailed_ = false;
        openRecoveredLibraryButton_->setVisible(!recoveredLibraryPath_.isEmpty());
        QString message;
        if (!result.error.isEmpty()) {
            message = result.error + QStringLiteral(" ") + deviceHandoffGuidance();
        } else if (recoveredLibraryPath_.isEmpty()) {
            message = QStringLiteral("Local draft saved at %1. No verified Recovered Library is available. %2")
                          .arg(conflictDraftPath_, deviceHandoffGuidance());
        } else {
            message = QStringLiteral("Local draft saved in the Recovered Library. The shared Library remains unchanged. %1")
                          .arg(deviceHandoffGuidance());
        }
        updateEditorState(message);
        statusBar()->showMessage(QStringLiteral("Local draft saved; shared save remains paused"));
        return true;
    }

    const PersistenceResult result = LibraryPersistence::saveFile(
        activeLibraryPath_, activeChapterRelativePath_, sourceHash_, newBytes);
    if (!result.ok) {
        saveFailed_ = true;
        if (result.conflict) {
            chapterConflict_ = true;
            conflictDraftPath_ = result.conflictDraftPath;
            recoveredLibraryPath_ = result.recoveredLibraryPath;
            openRecoveredLibraryButton_->setVisible(!recoveredLibraryPath_.isEmpty());
            openRecoveredFromRefusalButton_->hide();
            recoveryNotice_->hide();
        }
        const QString message = result.conflict
            ? result.error + QStringLiteral(" ") + deviceHandoffGuidance()
            : result.error;
        updateEditorState(message);
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
    chapterConflict_ = false;
    conflictDraftPath_.clear();
    saveFailed_ = false;
    chapterEditor_->setReadOnly(false);
    recoveredLibraryPath_.clear();
    openRecoveredLibraryButton_->hide();
    openRecoveredFromRefusalButton_->hide();
    recoveryNotice_->hide();
    const QString savedMessage = chapterDocument_.hasProtectedContent()
                                     ? QStringLiteral(
                                           "Protected legacy content stays unchanged. Changes are saved.")
                                     : activeDocumentIsOutline_
                                           ? QStringLiteral(
                                                 "Outline text is editable. Changes are saved.")
                                           : QStringLiteral(
                                                 "Plain prose is editable. Changes are saved.");
    updateEditorState(savedMessage);
    return true;
}

void LibraryWindow::switchToRecoveredLibrary()
{
    if (!savePendingEdits()) {
        return;
    }
    if (recoveredLibraryPath_.isEmpty()) {
        return;
    }

    const QString recoveredPath = recoveredLibraryPath_;
    const LibraryReadResult recovered = LibraryReader::read(recoveredPath);
    if (!recovered.ok()) {
        const QString message = QStringLiteral(
            "LEO could not verify the Recovered library. The local draft remains preserved. %1\n%2")
                                   .arg(deviceHandoffGuidance(), recovered.error);
        if (pages_->currentWidget() == editorPage_) {
            updateEditorState(message);
        } else {
            refusal_->setText(message);
        }
        return;
    }

    saveTimer_->stop();
    chapterDirty_ = false;
    chapterConflict_ = false;
    conflictDraftPath_.clear();
    saveFailed_ = false;
    if (openLibrary(recoveredPath)) {
        statusBar()->showMessage(QStringLiteral(
            "Recovered library is active. Original shared Library remains unchanged."));
    }
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
    chromeHoverFilter_->setAttention(saveFailed_ && !chapterConflict_);
    if (!message.isEmpty()) {
        if (chapterDirty_ && saveFailed_ && chapterConflict_) {
            editorState_->setText(QStringLiteral("Unsaved changes — save paused. %1")
                                      .arg(message));
        } else if (chapterDirty_ && chapterConflict_) {
            editorState_->setText(QStringLiteral(
                "New edits stay outside the shared Library while saving is paused. %1")
                                      .arg(message));
        } else if (chapterDirty_ && saveFailed_) {
            editorState_->setText(QStringLiteral("Unsaved changes — %1 Retry with Save.")
                                      .arg(message));
        } else {
            editorState_->setText(message);
        }
    }
    saveButton_->setEnabled(chapterDirty_ && !chapterReadOnly_ && !chapterConflict_);
    saveButton_->setText(chapterConflict_
                             ? QStringLiteral("Save paused")
                             : saveFailed_ && chapterDirty_
                                   ? QStringLiteral("Retry Save")
                                   : QStringLiteral("Save"));
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
