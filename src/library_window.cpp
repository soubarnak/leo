#include "manuscript_export.h"
#include "library_window.h"

#include "app_paths.h"
#include "ai_covers.h"
#include "book_covers.h"
#include "font_preferences.h"
#include "legacy_chapter_codec.h"
#include "library_creator.h"
#include "library_persistence.h"
#include "library_reader.h"
#include "release_check_dialog.h"
#include "spellcheck.h"
#include "chapter_links.h"
#include "darling_records.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QCursor>
#include <QDesktopServices>
#include <QDir>
#include <QDialog>
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
#include <QScopeGuard>
#include <QLabel>
#include <QLineEdit>
#include <QHeaderView>
#include <QMenu>
#include <QScreen>
#include <QMenuBar>
#include <QMouseEvent>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollArea>
#include <QScrollBar>
#include <QStandardPaths>
#include <QStackedWidget>
#include <QStatusBar>
#include <QSpinBox>
#include <QPainter>
#include <QPainterPath>
#include <QDateTime>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTimer>
#include <QListWidget>
#include <QTreeWidget>
#include <QUrl>
#include <QUuid>
#include <QVector>
#include <QVBoxLayout>
#include <QWheelEvent>

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
        QTreeWidgetItem *, QTreeWidgetItem *, LibraryDropPosition)>;

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
        LibraryDropPosition position = LibraryDropPosition::OnViewport;
        switch (dropIndicatorPosition()) {
        case QAbstractItemView::OnItem:
            position = LibraryDropPosition::OnItem;
            break;
        case QAbstractItemView::AboveItem:
            position = LibraryDropPosition::AboveItem;
            break;
        case QAbstractItemView::BelowItem:
            position = LibraryDropPosition::BelowItem;
            break;
        case QAbstractItemView::OnViewport:
            break;
        }
        dropHandler_(source, target, position);
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

void addBook(QTreeWidgetItem *parent, const Book &book, bool hasOutline,
             const QString &libraryPath)
{
    auto *bookItem = new QTreeWidgetItem(parent, {book.title, book.author});
    bookItem->setIcon(0, QPixmap::fromImage(BookCovers::render(
        libraryPath, book.id, QSize(72, 108))));
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

    void setPreferences(const QString &style, const QString &bodyFont, double pointSize)
    {
        style_ = style;
        QFont font(bodyFont);
        font.setPointSizeF(std::max(30.0, pointSize * 2.1));
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
        if (text.startsWith(QStringLiteral("[Protected legacy content"))) {
            QTextCharFormat planning;
            planning.setForeground(QColor(QStringLiteral("#8a8178")));
            planning.setFontItalic(true);
            setFormat(0, text.size(), planning);
            return;
        }
        if (text.trimmed() == QStringLiteral("***")) {
            QTextCharFormat sceneBreak;
            sceneBreak.setForeground(QColor(QStringLiteral("#8a8178")));
            sceneBreak.setFontWeight(QFont::DemiBold);
            setFormat(0, text.size(), sceneBreak);
            return;
        }
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
    using SplitHandler = std::function<void(const QString &, int)>;

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
        enterRun_ = 0;
        protectionContext_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
        copiedSource_.clear();
        primarySelection_.clear();
    }

    void refreshProtectedTokens(const QStringList &tokens)
    {
        if (protectedTokens_ != tokens) {
            setProtectedTokens(tokens);
        }
    }

    void setRefusalHandler(std::function<void()> handler)
    {
        refusalHandler_ = std::move(handler);
    }

    void setPasteRefusalHandler(std::function<void()> handler)
    {
        pasteRefusalHandler_ = std::move(handler);
    }

    void setPasteCleanedHandler(std::function<void()> handler)
    {
        pasteCleanedHandler_ = std::move(handler);
    }

    void setSplitHandler(SplitHandler handler)
    {
        splitHandler_ = std::move(handler);
    }

    bool canFormatSelection() const
    {
        return !isReadOnly() && !selectionTouchesProtected(textCursor()) &&
               !cursorInsideProtected(textCursor().position());
    }

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        const QTextCursor cursor = textCursor();
        const bool selectionTouches = selectionTouchesProtected(cursor);
        const bool plainEnter = (event->key() == Qt::Key_Return ||
                                 event->key() == Qt::Key_Enter) &&
            !(event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier |
                                    Qt::AltModifier | Qt::MetaModifier));
        if (!plainEnter) {
            enterRun_ = 0;
        } else if (selectionTouches || cursorInsideProtected(cursor.position())) {
            enterRun_ = 0;
        } else if (enterRun_ == 1 && cursor.block().text().isEmpty() &&
                   cursor.block().previous().isValid() &&
                   cursor.block().previous().text() != QStringLiteral("***")) {
            QTextCursor marker = cursor;
            marker.insertText(QStringLiteral("***"));
            marker.insertBlock();
            setTextCursor(marker);
            enterRun_ = 2;
            event->accept();
            return;
        } else if (enterRun_ >= 2 && cursor.block().text().isEmpty() &&
                   cursor.block().previous().text() == QStringLiteral("***")) {
            const QTextBlock markerBlock = cursor.block().previous();
            const int markerStart = markerBlock.position();
            const int splitPosition = markerStart > 0 ? markerStart - 1 : 0;
            const int suffixStart = cursor.block().position();
            const QString originalText = toPlainText();
            const QString splitText = originalText.left(splitPosition) +
                                      originalText.mid(suffixStart);
            enterRun_ = 0;
            if (splitHandler_) {
                splitHandler_(splitText, splitPosition);
            }
            event->accept();
            return;
        }
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
            enterRun_ = 0;
            reportRefusal();
            event->accept();
            return;
        }
        if (plainEnter) {
            enterRun_ = (enterRun_ >= 2) ? 0 : enterRun_ + 1;
        }
        if (!cursor.hasSelection() && !(event->modifiers() &
            (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) &&
            !cursorInsideProtected(cursor.position())) {
            const QString typed = event->text();
            const QString before = cursor.block().text().left(cursor.positionInBlock());
            QString replacement;
            int remove = 0;
            if (typed == QStringLiteral("-") && before.endsWith(QLatin1Char('-'))) {
                replacement = QString(QChar(0x2014));
                remove = 1;
            } else if (typed == QStringLiteral(".") && before.endsWith(QStringLiteral(".."))) {
                replacement = QString(QChar(0x2026));
                remove = 2;
            } else if (typed == QStringLiteral("\"") || typed == QStringLiteral("'")) {
                const QChar previous = before.isEmpty() ? QChar() : before.back();
                const bool opening = previous.isNull() || previous.isSpace() ||
                    QStringLiteral("([{—‘“>").contains(previous);
                replacement = typed == QStringLiteral("\"")
                    ? QString(QChar(opening ? 0x201c : 0x201d))
                    : QString(QChar(opening ? 0x2018 : 0x2019));
            }
            if (!replacement.isEmpty()) {
                QTextCursor edit = cursor;
                edit.beginEditBlock();
                for (int index = 0; index < remove; ++index) edit.deletePreviousChar();
                edit.insertText(replacement);
                edit.endEditBlock();
                setTextCursor(edit);
                event->accept();
                return;
            }
        }
        QPlainTextEdit::keyPressEvent(event);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        enterRun_ = 0;
        QPlainTextEdit::mousePressEvent(event);
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
        if (!source->hasHtml()) {
            QPlainTextEdit::insertFromMimeData(source);
            return;
        }
        static const QRegularExpression tags(
            QStringLiteral("<\\s*/?\\s*([a-z][a-z0-9:-]*)\\b[^>]*>"),
            QRegularExpression::CaseInsensitiveOption);
        static const QSet<QString> safeTags = {
            QStringLiteral("html"), QStringLiteral("head"), QStringLiteral("body"),
            QStringLiteral("meta"), QStringLiteral("title"), QStringLiteral("style"),
            QStringLiteral("p"), QStringLiteral("div"), QStringLiteral("span"),
            QStringLiteral("b"), QStringLiteral("strong"), QStringLiteral("i"),
            QStringLiteral("em"), QStringLiteral("br")};
        auto tagsInSource = tags.globalMatch(source->html());
        while (tagsInSource.hasNext()) {
            const auto match = tagsInSource.next();
            if (!safeTags.contains(match.captured(1).toLower())) {
                if (pasteRefusalHandler_) pasteRefusalHandler_();
                return;
            }
        }
        QTextDocument pasted;
        pasted.setHtml(source->html());
        QTextCursor insertion = textCursor();
        insertion.beginEditBlock();
        bool firstBlock = true;
        for (QTextBlock block = pasted.begin(); block.isValid(); block = block.next()) {
            if (!firstBlock) {
                insertion.insertBlock();
            }
            if (!firstBlock || insertion.block().text().isEmpty()) {
                QTextBlockFormat blockFormat;
                blockFormat.setAlignment(block.blockFormat().alignment());
                insertion.mergeBlockFormat(blockFormat);
            }
            firstBlock = false;
            for (auto fragment = block.begin(); !fragment.atEnd(); ++fragment) {
                const QTextFragment part = fragment.fragment();
                if (!part.isValid()) continue;
                QTextCharFormat format;
                format.setFontWeight(part.charFormat().fontWeight() >= QFont::Bold
                                         ? QFont::Bold : QFont::Normal);
                format.setFontItalic(part.charFormat().fontItalic());
                insertion.insertText(part.text(), format);
            }
        }
        insertion.endEditBlock();
        setTextCursor(insertion);
        if (pasteCleanedHandler_) pasteCleanedHandler_();
    }

    QMimeData *createMimeDataFromSelection() const override
    {
        QMimeData *mime = QPlainTextEdit::createMimeDataFromSelection();
        const QTextCursor selection = textCursor();
        if (selectionTouchesProtected(selection)) return mime;
        mime->setHtml(selection.selection().toHtml());
        return mime;
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
        QMimeData *mime = createMimeDataFromSelection();
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
    int enterRun_ = 0;
    QString protectionContext_;
    ClipboardProvenance copiedSource_;
    ClipboardProvenance primarySelection_;
    std::function<void()> refusalHandler_;
    std::function<void()> pasteRefusalHandler_;
    std::function<void()> pasteCleanedHandler_;
    SplitHandler splitHandler_;
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

void LibraryWindow::clampToAvailableScreen()
{
    const QScreen *targetScreen = screen();
    if (!targetScreen) {
        return;
    }
    const QSize available = targetScreen->availableGeometry().size();
    if (width() > available.width() || height() > available.height()) {
        resize(qMin(width(), available.width()), qMin(height(), available.height()));
    }
}

LibraryWindow::LibraryWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("LEO"));
    resize(960, 700);
    clampToAvailableScreen();
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
    onboardingAuthor_->setAccessibleName(QStringLiteral("Author name"));
    onboardingForm->addRow(QStringLiteral("Author name"), onboardingAuthor_);
    onboardingMode_ = new QComboBox(onboardingPage_);
    onboardingMode_->setObjectName(QStringLiteral("onboarding-mode"));
    onboardingMode_->addItem(QStringLiteral("Pantser — start with a blank chapter"),
                             QStringLiteral("pantser"));
    onboardingMode_->addItem(QStringLiteral("Plotter — start with an outline"),
                             QStringLiteral("plotter"));
    onboardingMode_->setAccessibleName(QStringLiteral("Writing mode"));
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
        const QString fallbackFont = FontPreferences::bodyFallbackFamily();
        onboardingBodyFont_->addItem(fallbackFont, fallbackFont);
    }
    onboardingBodyFont_->setAccessibleName(QStringLiteral("Body typeface"));
    onboardingForm->addRow(QStringLiteral("Body typeface"), onboardingBodyFont_);
    onboardingDropCap_ = new QComboBox(onboardingPage_);
    onboardingDropCap_->setObjectName(QStringLiteral("onboarding-drop-cap"));
    onboardingDropCap_->addItem(QStringLiteral("Literary"), QStringLiteral("literary"));
    onboardingDropCap_->addItem(QStringLiteral("Fantasy"), QStringLiteral("fantasy"));
    onboardingDropCap_->addItem(QStringLiteral("Sci-Fi"), QStringLiteral("scifi"));
    onboardingDropCap_->setAccessibleName(QStringLiteral("Drop-cap style"));
    onboardingForm->addRow(QStringLiteral("Drop-cap style"), onboardingDropCap_);

    onboardingLocation_ = new QLineEdit(defaultPath_, onboardingPage_);
    onboardingLocation_->setObjectName(QStringLiteral("onboarding-location"));
    onboardingLocation_->setAccessibleName(QStringLiteral("New Library location"));
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
    tree_->setMinimumWidth(tree_->fontMetrics().averageCharWidth() * 24);
    tree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    tree_->header()->setMinimumSectionSize(tree_->fontMetrics().averageCharWidth() * 24);
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
                                       LibraryDropPosition position) {
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
    editorChrome_->setObjectName(QStringLiteral("writing-controls"));
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
    editorTitle_->setMinimumWidth(0);
    editorTitle_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    editorToolbar->addWidget(editorTitle_, 1);
    previousChapterButton_ = new QPushButton(QStringLiteral("‹ Previous"), editorChrome_);
    previousChapterButton_->setObjectName(QStringLiteral("chapter-previous"));
    connect(previousChapterButton_, &QPushButton::clicked, this,
            [this] { navigateChapter(-1); });
    editorToolbar->addWidget(previousChapterButton_);
    nextChapterButton_ = new QPushButton(QStringLiteral("Next ›"), editorChrome_);
    nextChapterButton_->setObjectName(QStringLiteral("chapter-next"));
    connect(nextChapterButton_, &QPushButton::clicked, this,
            [this] { navigateChapter(1); });
    editorToolbar->addWidget(nextChapterButton_);
    saveButton_ = new QPushButton(QStringLiteral("Save"), editorChrome_);
    saveButton_->setObjectName(QStringLiteral("chapter-save"));
    connect(saveButton_, &QPushButton::clicked, this, [this] { saveCurrentChapter(); });
    editorToolbar->addWidget(saveButton_);
    chromeLayout->addLayout(editorToolbar);
    // A second row keeps the toolbar from forcing a window wider than a scaled screen.
    auto *progressToolbar = new QHBoxLayout;
    progressToolbar->addStretch();
    progressCount_ = new QLabel(editorChrome_);
    progressCount_->setObjectName(QStringLiteral("writing-word-count"));
    progressCount_->setAccessibleName(QStringLiteral("Word count"));
    progressToolbar->addWidget(progressCount_);
    progressGoal_ = new QLabel(editorChrome_);
    progressGoal_->setObjectName(QStringLiteral("writing-goal-count"));
    progressGoal_->setAccessibleName(QStringLiteral("Daily goal or sprint progress"));
    progressToolbar->addWidget(progressGoal_);
    auto *progressButton = new QPushButton(QStringLiteral("Goals & sprints…"), editorChrome_);
    progressButton->setObjectName(QStringLiteral("writing-progress-button"));
    connect(progressButton, &QPushButton::clicked, this, &LibraryWindow::showProgress);
    progressToolbar->addWidget(progressButton);
    openRecoveredLibraryButton_ = new QPushButton(
        QStringLiteral("Switch to Recovered Library"), editorChrome_);
    openRecoveredLibraryButton_->setObjectName(QStringLiteral("chapter-open-recovered"));
    openRecoveredLibraryButton_->setVisible(false);
    connect(openRecoveredLibraryButton_, &QPushButton::clicked,
            this, &LibraryWindow::switchToRecoveredLibrary);
    progressToolbar->addWidget(openRecoveredLibraryButton_);
    repairCopyButton_ = new QPushButton(QStringLiteral("Save Repair Copy…"), editorChrome_);
    repairCopyButton_->setObjectName(QStringLiteral("chapter-repair-copy"));
    repairCopyButton_->setVisible(false);
    connect(repairCopyButton_, &QPushButton::clicked, this, &LibraryWindow::saveRepairCopy);
    progressToolbar->addWidget(repairCopyButton_);
    chromeLayout->addLayout(progressToolbar);

    editorState_ = new QLabel(editorChrome_);
    editorState_->setObjectName(QStringLiteral("chapter-save-state"));
    editorState_->setWordWrap(true);
    chromeLayout->addWidget(editorState_);
    editorLayout->addWidget(editorChrome_);

    chapterEditor_ = new ProtectedChapterEditor(editorPage_);
    chapterEditor_->setObjectName(QStringLiteral("chapter-editor"));
    chapterEditor_->setAccessibleName(QStringLiteral("Chapter text or read-only source"));
    chapterEditor_->setTabChangesFocus(true);
    chapterEditor_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    chapterEditor_->viewport()->installEventFilter(this);
    dropCapHighlighter_ = new DropCapHighlighter(chapterEditor_->document());
    static_cast<ProtectedChapterEditor *>(chapterEditor_)->setRefusalHandler([this] {
        const QString message = QStringLiteral(
            "Edit refused because protected legacy content could change. Protected source and linked records remain unchanged.");
        updateEditorState(message);
    });
    static_cast<ProtectedChapterEditor *>(chapterEditor_)->setPasteRefusalHandler([this] {
        updateEditorState(QStringLiteral(
            "Paste refused because it contains content this editor cannot safely represent. "
            "The chapter and clipboard remain unchanged."));
    });
    static_cast<ProtectedChapterEditor *>(chapterEditor_)->setPasteCleanedHandler([this] {
        updateEditorState(QStringLiteral(
            "Paste cleaned to prose, bold, italic and alignment. External IDs and other formatting were removed."));
    });
    static_cast<ProtectedChapterEditor *>(chapterEditor_)->setSplitHandler(
        [this](const QString &text, int position) {
            splitActiveChapter(text, position, false);
        });
    bookFlow_ = new QScrollArea(editorPage_);
    bookFlow_->setObjectName(QStringLiteral("continuous-book-pages"));
    bookFlow_->setWidgetResizable(true);
    bookFlow_->setFocusPolicy(Qt::NoFocus);
    bookFlow_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    bookPages_ = new QWidget(bookFlow_);
    bookPages_->setFocusPolicy(Qt::NoFocus);
    bookPagesLayout_ = new QVBoxLayout(bookPages_);
    bookPagesLayout_->setContentsMargins(24, 24, 24, 24);
    bookPagesLayout_->setSpacing(24);
    bookFlow_->setWidget(bookPages_);
    editorLayout->addWidget(bookFlow_, 1);
    bookPagesLayout_->addWidget(chapterEditor_);
    connect(chapterEditor_->document(), &QTextDocument::contentsChanged,
            this, &LibraryWindow::resizeChapterEditorToContents);
    connect(chapterEditor_, &QPlainTextEdit::cursorPositionChanged, this, [this] {
        if (!loadingChapter_ && bookFlow_ && bookPages_) {
            const QPoint cursor = chapterEditor_->mapTo(
                bookPages_, chapterEditor_->cursorRect().center());
            if (activePreferences_.typewriter) {
                QScrollBar *bar = bookFlow_->verticalScrollBar();
                bar->setValue(cursor.y() - bookFlow_->viewport()->height() * 45 / 100);
            } else {
                bookFlow_->ensureVisible(cursor.x(), cursor.y(), 24, 96);
            }
        }
    });
    chromeHoverFilter_ = new HoverFadeFilter(editorChrome_, editorChrome_);
    saveTimer_ = new QTimer(this);
    saveTimer_->setSingleShot(true);
    saveTimer_->setInterval(800);
    connect(saveTimer_, &QTimer::timeout, this, [this] { saveCurrentChapter(); });
    connect(chapterEditor_, &QPlainTextEdit::selectionChanged,
            this, &LibraryWindow::refreshProgress);
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
        if (chapterStructure_) {
            chapterStructure_->invalidateHistoryForChapterEdit(activeChapterRelativePath_);
        }
        chapterDirty_ = true;
        updateEditorState();
        refreshProgress();
        saveTimer_->start();
        updateChapterStructureActions();
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
    QAction *writingHelp = helpMenu->addAction(QStringLiteral("Writing Shortcuts…"));
    writingHelp->setShortcut(QKeySequence::HelpContents);
    connect(writingHelp, &QAction::triggered, this, [this] {
        QMessageBox::information(this, QStringLiteral("Writing shortcuts"),
            QStringLiteral("Tab moves between controls. Ctrl+B and Ctrl+I format prose; "
                           "Ctrl+F finds text. Ctrl+C, Ctrl+X and Ctrl+V use the clipboard. "
                           "Ctrl+Enter toggles fullscreen; Escape exits fullscreen. "
                           "Use View for themes, typefaces, zoom and typewriter scrolling."));
    });

    QMenu *viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
    const auto describeSubmenu = [](QMenu *menu) {
        menu->setAccessibleName(menu->title());
        menu->setAccessibleDescription(QStringLiteral("submenu"));
        return menu;
    };
    QMenu *themeMenu = describeSubmenu(viewMenu->addMenu(QStringLiteral("Page theme")));
    auto *themeGroup = new QActionGroup(this);
    paperAction_ = themeMenu->addAction(QStringLiteral("Paper"));
    nightAction_ = themeMenu->addAction(QStringLiteral("Night"));
    paperAction_->setObjectName(QStringLiteral("view-theme-paper"));
    nightAction_->setObjectName(QStringLiteral("view-theme-night"));
    for (QAction *action : {paperAction_, nightAction_}) {
        action->setCheckable(true);
        themeGroup->addAction(action);
    }
    connect(paperAction_, &QAction::triggered, this, [this] {
        savePresentationPreference(QStringLiteral("pageTheme"), QStringLiteral("paper"));
    });
    connect(nightAction_, &QAction::triggered, this, [this] {
        savePresentationPreference(QStringLiteral("pageTheme"), QStringLiteral("night"));
    });
    brightAction_ = viewMenu->addAction(QStringLiteral("Brighter controls"));
    brightAction_->setObjectName(QStringLiteral("view-brighter-controls"));
    brightAction_->setCheckable(true);
    connect(brightAction_, &QAction::triggered, this, [this](bool checked) {
        savePresentationPreference(QStringLiteral("uiBright"), checked);
    });
    pinControlsAction_ = viewMenu->addAction(QStringLiteral("Pin writing controls"));
    pinControlsAction_->setObjectName(QStringLiteral("view-pin-controls"));
    pinControlsAction_->setCheckable(true);
    connect(pinControlsAction_, &QAction::triggered, this, [this](bool checked) {
        savePresentationPreference(QStringLiteral("chromePinned"), checked);
    });
    typewriterAction_ = viewMenu->addAction(QStringLiteral("Typewriter scrolling"));
    typewriterAction_->setObjectName(QStringLiteral("view-typewriter"));
    typewriterAction_->setCheckable(true);
    connect(typewriterAction_, &QAction::triggered, this, [this](bool checked) {
        savePresentationPreference(QStringLiteral("typewriter"), checked);
    });
    QMenu *fontMenu = describeSubmenu(viewMenu->addMenu(QStringLiteral("Body typeface")));
    for (const QString &family : {QStringLiteral("Georgia"), QStringLiteral("Palatino"),
                                  QStringLiteral("Baskerville"), QStringLiteral("DejaVu Serif"),
                                  QStringLiteral("Liberation Serif"), QStringLiteral("Noto Serif")}) {
        if (FontPreferences::installedFamily({family}).isEmpty()) continue;
        QAction *fontAction = fontMenu->addAction(family);
        fontAction->setObjectName(QStringLiteral("view-body-font-%1").arg(family));
        connect(fontAction, &QAction::triggered, this, [this, family] {
            savePresentationPreference(QStringLiteral("bodyFont"), family);
        });
    }
    QMenu *dropCapMenu = describeSubmenu(viewMenu->addMenu(QStringLiteral("Drop-cap style")));
    const QStringList dropCapNames{QStringLiteral("Literary"), QStringLiteral("Fantasy"),
                                   QStringLiteral("Sci-Fi")};
    const QStringList dropCapIds{QStringLiteral("literary"), QStringLiteral("fantasy"),
                                 QStringLiteral("scifi")};
    for (int index = 0; index < dropCapNames.size(); ++index) {
        const QString id = dropCapIds.at(index);
        QAction *dropCapAction = dropCapMenu->addAction(dropCapNames.at(index));
        dropCapAction->setObjectName(QStringLiteral("view-dropcap-%1").arg(id));
        connect(dropCapAction, &QAction::triggered,
                this, [this, id] { savePresentationPreference(QStringLiteral("dropCapStyle"), id); });
    }
    viewMenu->addSeparator();
    const auto zoomBy = [this](double change) {
        savePresentationPreference(QStringLiteral("pageZoom"),
            qBound(0.75, activePreferences_.pageZoom + change, 1.6));
    };
    QAction *zoomIn = viewMenu->addAction(QStringLiteral("Zoom in"));
    zoomIn->setObjectName(QStringLiteral("view-zoom-in"));
    // Screen readers mangle the "Ctrl++" accelerator, so list the plain Ctrl+= key first.
    zoomIn->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_Equal), QKeySequence(QKeySequence::ZoomIn)});
    connect(zoomIn, &QAction::triggered, this, [zoomBy] { zoomBy(0.1); });
    QAction *zoomOut = viewMenu->addAction(QStringLiteral("Zoom out"));
    zoomOut->setObjectName(QStringLiteral("view-zoom-out"));
    zoomOut->setShortcut(QKeySequence::ZoomOut);
    connect(zoomOut, &QAction::triggered, this, [zoomBy] { zoomBy(-0.1); });
    QAction *resetZoom = viewMenu->addAction(QStringLiteral("Reset zoom"));
    resetZoom->setObjectName(QStringLiteral("view-zoom-reset"));
    resetZoom->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
    connect(resetZoom, &QAction::triggered,
            this, [this] { savePresentationPreference(QStringLiteral("pageZoom"), 1.0); });
    QMenu *sizeMenu = describeSubmenu(viewMenu->addMenu(QStringLiteral("Writing text size")));
    QAction *largerText = sizeMenu->addAction(QStringLiteral("Larger"));
    largerText->setObjectName(QStringLiteral("view-text-larger"));
    connect(largerText, &QAction::triggered, this, [this] {
        savePresentationPreference(QStringLiteral("editorFontSize"),
                                   qMin(22, activePreferences_.editorFontSize + 1));
    });
    QAction *smallerText = sizeMenu->addAction(QStringLiteral("Smaller"));
    smallerText->setObjectName(QStringLiteral("view-text-smaller"));
    connect(smallerText, &QAction::triggered, this, [this] {
        savePresentationPreference(QStringLiteral("editorFontSize"),
                                   qMax(14, activePreferences_.editorFontSize - 1));
    });
    QAction *resetText = sizeMenu->addAction(QStringLiteral("Reset size"));
    resetText->setObjectName(QStringLiteral("view-text-reset"));
    connect(resetText, &QAction::triggered, this, [this] {
        savePresentationPreference(QStringLiteral("editorFontSize"), 17);
    });
    QAction *fullscreen = viewMenu->addAction(QStringLiteral("Toggle fullscreen"));
    fullscreen->setObjectName(QStringLiteral("view-fullscreen"));
    fullscreen->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return));
    connect(fullscreen, &QAction::triggered, this, [this] {
        isFullScreen() ? showNormal() : showFullScreen();
    });
    QAction *exitFullscreen = new QAction(this);
    exitFullscreen->setShortcut(QKeySequence(Qt::Key_Escape));
    exitFullscreen->setShortcutContext(Qt::WindowShortcut);
    addAction(exitFullscreen);
    connect(exitFullscreen, &QAction::triggered, this, [this] {
        if (isFullScreen()) showNormal();
    });

    QMenu *editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));
    undoStructureAction_ = editMenu->addAction(QStringLiteral("Undo chapter structure"));
    undoStructureAction_->setObjectName(QStringLiteral("chapter-structure-undo"));
    connect(undoStructureAction_, &QAction::triggered,
            this, &LibraryWindow::undoChapterStructure);
    redoStructureAction_ = editMenu->addAction(QStringLiteral("Redo chapter structure"));
    redoStructureAction_->setObjectName(QStringLiteral("chapter-structure-redo"));
    connect(redoStructureAction_, &QAction::triggered,
            this, &LibraryWindow::redoChapterStructure);
    QAction *findAction = editMenu->addAction(QStringLiteral("Find and Replace…"));
    findAction->setObjectName(QStringLiteral("book-find-replace"));
    findAction->setShortcut(QKeySequence::Find);
    connect(findAction, &QAction::triggered, this, &LibraryWindow::showFindReplace);
    QAction *undoReplaceAction = editMenu->addAction(QStringLiteral("Undo replacement batch"));
    undoReplaceAction->setObjectName(QStringLiteral("book-replace-undo"));
    connect(undoReplaceAction, &QAction::triggered, this, &LibraryWindow::undoReplacement);
    QAction *cutDarling = editMenu->addAction(QStringLiteral("Save selection to Darlings"));
    cutDarling->setObjectName(QStringLiteral("darling-cut"));
    connect(cutDarling, &QAction::triggered, this, &LibraryWindow::cutSelectionToDarlings);
    QAction *manageDarling = editMenu->addAction(QStringLiteral("Manage Darlings…"));
    manageDarling->setObjectName(QStringLiteral("darling-manage"));
    connect(manageDarling, &QAction::triggered, this, &LibraryWindow::manageDarlings);
    QAction *spellAction = editMenu->addAction(QStringLiteral("Check Spelling…"));
    spellAction->setObjectName(QStringLiteral("chapter-spellcheck"));
    connect(spellAction, &QAction::triggered, this, &LibraryWindow::showSpellcheck);
    auto *planningMenu = menuBar()->addMenu(QStringLiteral("&Planning"));
    connect(planningMenu->addAction(QStringLiteral("Show chapter and section outline…")),
            &QAction::triggered, this, &LibraryWindow::showPlanningOutline);
    connect(planningMenu->addAction(QStringLiteral("Add placeholder and sticky…")),
            &QAction::triggered, this, &LibraryWindow::addPlanningSticky);
    connect(planningMenu->addAction(QStringLiteral("Add section to outline…")),
            &QAction::triggered, this, &LibraryWindow::addPlanningSection);
    connect(planningMenu->addAction(QStringLiteral("Edit chapter note…")),
            &QAction::triggered, this, &LibraryWindow::editChapterNote);
    connect(planningMenu->addAction(QStringLiteral("Manage stickies…")),
            &QAction::triggered, this, [this] { managePlanningRecords(false); });
    connect(planningMenu->addAction(QStringLiteral("Manage sections…")),
            &QAction::triggered, this, [this] { managePlanningRecords(true); });
    connect(planningMenu->addAction(QStringLiteral("Undo planning change")),
            &QAction::triggered, this, &LibraryWindow::undoPlanningChange);
    auto *formatMenu = menuBar()->addMenu(QStringLiteral("&Format"));
    const auto changedFormat = [this] {
        chapterDirty_ = true;
        updateEditorState();
        refreshProgress();
        saveTimer_->start();
    };
    const auto addCharacterAction = [this, formatMenu, changedFormat](
                                        const QString &label, const QKeySequence &shortcut,
                                        bool bold) {
        QAction *action = formatMenu->addAction(label);
        action->setShortcut(shortcut);
        connect(action, &QAction::triggered, this, [this, bold, changedFormat] {
            auto *editor = static_cast<ProtectedChapterEditor *>(chapterEditor_);
            if (!editor->canFormatSelection()) return;
            QTextCursor cursor = editor->textCursor();
            QTextCharFormat format;
            if (bold) format.setFontWeight(cursor.charFormat().fontWeight() >= QFont::Bold
                                               ? QFont::Normal : QFont::Bold);
            else format.setFontItalic(!cursor.charFormat().fontItalic());
            if (cursor.hasSelection()) cursor.mergeCharFormat(format);
            else editor->mergeCurrentCharFormat(format);
            changedFormat();
        });
    };
    addCharacterAction(QStringLiteral("Bold"), QKeySequence::Bold, true);
    addCharacterAction(QStringLiteral("Italic"), QKeySequence::Italic, false);
    const auto addAlignmentAction = [this, formatMenu, changedFormat](
                                        const QString &label, const QKeySequence &shortcut,
                                        Qt::Alignment alignment) {
        QAction *action = formatMenu->addAction(label);
        action->setShortcut(shortcut);
        connect(action, &QAction::triggered, this, [this, alignment, changedFormat] {
            auto *editor = static_cast<ProtectedChapterEditor *>(chapterEditor_);
            if (!editor->canFormatSelection()) return;
            QTextCursor cursor = editor->textCursor();
            QTextBlockFormat format;
            format.setAlignment(alignment);
            cursor.mergeBlockFormat(format);
            changedFormat();
        });
    };
    addAlignmentAction(QStringLiteral("Align Left"), QKeySequence(Qt::CTRL | Qt::Key_L), Qt::AlignLeft);
    addAlignmentAction(QStringLiteral("Align Center"), QKeySequence(Qt::CTRL | Qt::Key_E), Qt::AlignHCenter);
    addAlignmentAction(QStringLiteral("Align Right"), QKeySequence(Qt::CTRL | Qt::Key_R), Qt::AlignRight);
    addAlignmentAction(QStringLiteral("Justify"), QKeySequence(Qt::CTRL | Qt::Key_J), Qt::AlignJustify);
    updateChapterStructureActions();
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
    const bool sameLibrary = !activeLibraryPath_.isEmpty() &&
        canonicalOrCleanPath(path) == canonicalOrCleanPath(activeLibraryPath_);
    if (!sameLibrary) {
        chapterStructure_.reset();
        structureBookId_.clear();
        activeBookId_.clear();
        bookSearch_.reset();
        searchBookId_.clear();
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
    progress_.reset();
    sprintRunning_ = false;
    sprintBookId_.clear();
    planningRecords_.reset();
    planningBookId_.clear();
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
            addBook(parent, book, QFileInfo::exists(outlinePath), library.path);
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
        QAction *import = menu.addAction(QStringLiteral("Import manuscript…"));
        QAction *anthology = menu.addAction(QStringLiteral("Export DOCX anthology…"));
        QAction *epubAnthology = menu.addAction(QStringLiteral("Export EPUB anthology…"));
        QAction *pdfAnthology = menu.addAction(QStringLiteral("Export PDF anthology…"));
        QAction *rename = menu.addAction(QStringLiteral("Rename shelf…"));
        QAction *remove = menu.addAction(QStringLiteral("Delete shelf…"));
        QAction *chosen = menu.exec(tree_->viewport()->mapToGlobal(position));
        if (chosen == newBook) {
            createBookFromSelection();
        } else if (chosen == anthology || chosen == epubAnthology || chosen == pdfAnthology) {
            if (!saveCurrentChapter()) return;
            const QString destination = QFileDialog::getSaveFileName(this, QStringLiteral("Export anthology"), {},
                chosen == pdfAnthology ? QStringLiteral("PDF book (*.pdf)") : chosen == epubAnthology ? QStringLiteral("EPUB book (*.epub)") : QStringLiteral("Word document (*.docx)"));
            if (destination.isEmpty()) return;
            const auto result = ManuscriptExport::writeShelf(activeLibraryPath_, shelfId, destination,
                chosen == pdfAnthology ? ManuscriptFormat::Pdf : chosen == epubAnthology ? ManuscriptFormat::Epub : ManuscriptFormat::Docx);
            if (!result.ok) QMessageBox::warning(this, QStringLiteral("Export failed"), result.error);
            else statusBar()->showMessage(QStringLiteral("Anthology exported."), 5000);
        } else if (chosen == import) {
            const QString source = QFileDialog::getOpenFileName(this, QStringLiteral("Import manuscript"),
                QString(), QStringLiteral("Manuscripts (*.docx *.txt *.md *.markdown)"));
            if (source.isEmpty()) return;
            const auto preview = ManuscriptImport::read(source);
            QString details = preview.warnings.join(QLatin1Char('\n'));
            if (!preview.ok()) {
                QMessageBox::warning(this, QStringLiteral("Import refused"), preview.error + "\n" + details);
                return;
            }
            QStringList headings;
            for (const auto &chapter : preview.chapters) headings.append(chapter.title);
            details = QStringLiteral("Title: %1\nChapters: %2\nScene breaks: %3\n\n%4\n\n%5")
                .arg(preview.title).arg(preview.chapters.size()).arg(preview.sceneBreaks)
                .arg(headings.join(QLatin1Char('\n')), details);
            QMessageBox confirm(QMessageBox::Question, QStringLiteral("Import preview"), details,
                                QMessageBox::Ok | QMessageBox::Cancel, this);
            confirm.setTextFormat(Qt::PlainText);
            if (confirm.exec() != QMessageBox::Ok) return;
            finishOrganizationChange(organization_->importBook(shelfId, preview),
                                     QStringLiteral("Manuscript imported."));
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

    if (item->data(0, ItemKindRole).toInt() == ChapterItemKind) {
        showChapterStructureMenu(item, tree_->viewport()->mapToGlobal(position));
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
    QAction *addChapter = menu.addAction(QStringLiteral("Add chapter…"));
    QMenu *exports = menu.addMenu(QStringLiteral("Export manuscript"));
    QAction *exportText = exports->addAction(QStringLiteral("Plain text…"));
    QAction *exportMarkdown = exports->addAction(QStringLiteral("Markdown…"));
    QAction *exportHtml = exports->addAction(QStringLiteral("HTML…"));
    QAction *exportDocx = exports->addAction(QStringLiteral("Word document…"));
    QAction *exportEpub = exports->addAction(QStringLiteral("EPUB book…"));
    QAction *exportPdf = exports->addAction(QStringLiteral("PDF book…"));
    QMenu *covers = menu.addMenu(QStringLiteral("Cover"));
    QAction *importCover = covers->addAction(QStringLiteral("Import image…"));
    QAction *showImage = covers->addAction(QStringLiteral("Show imported image"));
    QAction *showAbstract = covers->addAction(QStringLiteral("Show seeded artwork"));
    QAction *showPainted = covers->addAction(QStringLiteral("Show generated artwork"));
    QAction *repaintCover = covers->addAction(QStringLiteral("Repaint seeded artwork"));
    QAction *removeCover = covers->addAction(QStringLiteral("Remove imported image"));
    covers->addSeparator();
    QAction *saveApiKey = covers->addAction(QStringLiteral("Save OpenAI API key…"));
    QAction *generateCover = covers->addAction(QStringLiteral("Generate artwork from manuscript…"));
    QAction *rename = menu.addAction(QStringLiteral("Rename book…"));
    QAction *remove = menu.addAction(QStringLiteral("Remove from shelves"));
    QAction *trash = menu.addAction(QStringLiteral("Move to Trash…"));
    QAction *chosen = menu.exec(tree_->viewport()->mapToGlobal(position));
    if (chosen == exportText || chosen == exportMarkdown || chosen == exportHtml || chosen == exportDocx || chosen == exportEpub || chosen == exportPdf) {
        if (!saveCurrentChapter()) return;
        const auto format = chosen == exportPdf ? ManuscriptFormat::Pdf : chosen == exportEpub ? ManuscriptFormat::Epub : chosen == exportText ? ManuscriptFormat::Text
            : chosen == exportMarkdown ? ManuscriptFormat::Markdown : chosen == exportDocx ? ManuscriptFormat::Docx : ManuscriptFormat::Html;
        const QString filter = chosen == exportPdf ? QStringLiteral("PDF book (*.pdf)") : chosen == exportEpub ? QStringLiteral("EPUB book (*.epub)") : chosen == exportText ? QStringLiteral("Plain text (*.txt)")
            : chosen == exportMarkdown ? QStringLiteral("Markdown (*.md)") : chosen == exportDocx ? QStringLiteral("Word document (*.docx)") : QStringLiteral("HTML (*.html)");
        const QString destination = QFileDialog::getSaveFileName(this, QStringLiteral("Export manuscript"), {}, filter);
        if (destination.isEmpty()) return;
        const auto result = ManuscriptExport::write(activeLibraryPath_, bookId, format, destination);
        if (!result.ok) QMessageBox::warning(this, QStringLiteral("Export failed"), result.error);
        else statusBar()->showMessage(QStringLiteral("Manuscript exported."), 5000);
        return;
    }
    if (chosen == saveApiKey) {
        bool accepted = false;
        const QString key = QInputDialog::getText(this, QStringLiteral("OpenAI API key"),
            QStringLiteral("Save key in the system secret store:"), QLineEdit::Password,
            {}, &accepted);
        if (accepted) {
            auto *service = new AiCovers(qApp);
            QPointer<LibraryWindow> window(this);
            service->storeKey(key, [window, service](bool ok, const QString &message) {
                if (window) QMessageBox::information(window, ok ? QStringLiteral("Key saved")
                                                                 : QStringLiteral("Key unavailable"), message);
                service->deleteLater();
            });
        }
        return;
    }
    if (chosen == generateCover) {
        if (!savePendingEdits()) return;
        if (QMessageBox::question(this, QStringLiteral("Generate cover artwork"),
            QStringLiteral("LEO will send an excerpt of this manuscript to OpenAI and make two paid API requests. Generate this cover now?"),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) {
            statusBar()->showMessage(QStringLiteral("Cover generation cancelled; cover unchanged."), 5000);
            return;
        }
        auto *service = new AiCovers(qApp);
        auto *progress = new QProgressDialog(QStringLiteral("Generating cover artwork…"),
            QStringLiteral("Cancel"), 0, 0, this);
        progress->setWindowModality(Qt::NonModal);
        progress->setMinimumDuration(0);
        progress->show();
        connect(progress, &QProgressDialog::canceled, service, &AiCovers::cancel);
        const QString root = activeLibraryPath_;
        QPointer<LibraryWindow> window(this);
        QPointer<QProgressDialog> progressGuard(progress);
        statusBar()->showMessage(QStringLiteral("Generating artwork…"));
        service->generate(root, bookId,
            [window, progressGuard, service, root](bool ok, const QString &message) {
                service->deleteLater();
                if (progressGuard) progressGuard->deleteLater();
                if (!window) return;
                if (ok) {
                    if (window->activeLibraryPath_ == root) window->refreshOrganizationView();
                    window->statusBar()->showMessage(message, 8000);
                } else {
                    QMessageBox::warning(window, QStringLiteral("Cover unchanged"), message);
                }
            });
        return;
    }
    if (chosen == importCover || chosen == showImage || chosen == showAbstract ||
        chosen == showPainted ||
        chosen == repaintCover || chosen == removeCover) {
        CoverResult result;
        if (chosen == importCover) {
            const QString source = QFileDialog::getOpenFileName(
                this, QStringLiteral("Choose cover image"), {},
                QStringLiteral("Images (*.png *.jpg *.jpeg *.webp *.bmp)"));
            if (source.isEmpty()) return;
            result = BookCovers::importImage(activeLibraryPath_, bookId, source);
        } else if (chosen == repaintCover) {
            result = BookCovers::repaint(activeLibraryPath_, bookId);
        } else {
            result = chosen == removeCover
                ? BookCovers::removeImage(activeLibraryPath_, bookId)
                : BookCovers::setMode(activeLibraryPath_, bookId,
                    chosen == showImage ? CoverMode::Image :
                    chosen == showPainted ? CoverMode::Painted : CoverMode::Abstract);
        }
        if (!result.ok) {
            QMessageBox::warning(this, QStringLiteral("Cover unchanged"), result.error);
            return;
        }
        item->setIcon(0, QPixmap::fromImage(BookCovers::render(
            activeLibraryPath_, bookId, QSize(72, 108))));
        statusBar()->showMessage(QStringLiteral("Cover updated."));
    } else if (chosen == addChapter) {
        if (!ensureChapterStructure(bookId) || !savePendingEdits()) {
            return;
        }
        int chapterCount = 0;
        for (int index = 0; index < item->childCount(); ++index) {
            chapterCount += item->child(index)->data(0, ItemKindRole).toInt() ==
                            ChapterItemKind;
        }
        bool accepted = false;
        const QString title = QInputDialog::getText(
            this, QStringLiteral("Add chapter"), QStringLiteral("Chapter title:"),
            QLineEdit::Normal, QStringLiteral("Chapter %1").arg(chapterCount + 1),
            &accepted);
        if (accepted) {
            const ChapterStructureResult result =
                chapterStructure_->addChapter(chapterCount, title);
            applyChapterStructureResult(result, bookId, QStringLiteral("Chapter added."));
        }
    } else if (chosen == rename) {
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

void LibraryWindow::showChapterStructureMenu(QTreeWidgetItem *item,
                                             const QPoint &globalPosition)
{
    if (!item) {
        return;
    }
    const QString bookId = item->data(0, BookIdRole).toString();
    const QString chapterId = item->data(0, ChapterIdRole).toString();
    if (bookId.isEmpty() || chapterId.isEmpty() || !ensureChapterStructure(bookId)) {
        return;
    }

    int row = 0;
    int chapterCount = 0;
    for (int index = 0; index < item->parent()->childCount(); ++index) {
        QTreeWidgetItem *sibling = item->parent()->child(index);
        if (sibling->data(0, ItemKindRole).toInt() != ChapterItemKind) {
            continue;
        }
        if (sibling == item) {
            row = chapterCount;
        }
        ++chapterCount;
    }

    QMenu menu(this);
    QAction *rename = menu.addAction(QStringLiteral("Rename chapter title…"));
    QAction *moveUp = menu.addAction(QStringLiteral("Move chapter up"));
    QAction *moveDown = menu.addAction(QStringLiteral("Move chapter down"));
    moveUp->setEnabled(row > 0);
    moveDown->setEnabled(row + 1 < chapterCount);
    menu.addSeparator();
    QAction *split = menu.addAction(QStringLiteral("Split at cursor"));
    const bool activeChapter = pages_->currentWidget() == editorPage_ &&
        activeBookId_ == bookId &&
        QFileInfo(activeChapterRelativePath_).completeBaseName() == chapterId &&
        !activeDocumentIsOutline_ && !chapterReadOnly_;
    split->setEnabled(activeChapter);
    QAction *joinPrevious = menu.addAction(QStringLiteral("Join with previous chapter"));
    QAction *joinNext = menu.addAction(QStringLiteral("Join with next chapter"));
    joinPrevious->setEnabled(row > 0);
    joinNext->setEnabled(row + 1 < chapterCount);
    QAction *remove = menu.addAction(QStringLiteral("Delete chapter…"));
    menu.addSeparator();
    QAction *undo = menu.addAction(QStringLiteral("Undo chapter structure"));
    QAction *redo = menu.addAction(QStringLiteral("Redo chapter structure"));
    undo->setEnabled(chapterStructure_->canUndo());
    redo->setEnabled(chapterStructure_->canRedo());

    QAction *chosen = menu.exec(globalPosition);
    if (!chosen) {
        return;
    }
    if (chosen == undo) {
        undoChapterStructure();
        return;
    }
    if (chosen == redo) {
        redoChapterStructure();
        return;
    }
    if (chosen == split) {
        if (activeChapter) {
            splitActiveChapter(chapterEditor_->toPlainText(),
                               chapterEditor_->textCursor().position());
        }
        return;
    }
    if (!savePendingEdits()) {
        return;
    }

    const auto readChapterSource = [this, &bookId, &chapterId](
        ChapterEditSource *source, QString *error) {
        const QString relativePath = bookId + QStringLiteral("/chapters/") + chapterId +
                                     QStringLiteral(".html");
        QByteArray bytes;
        if (!LibraryPersistence::readLibraryFile(activeLibraryPath_, relativePath,
                                                 &bytes, error)) {
            return false;
        }
        *source = {bytes, LibraryPersistence::hash(bytes), bytes};
        return true;
    };

    ChapterStructureResult result;
    QString successMessage;
    if (chosen == rename) {
        QString currentTitle;
        const QString label = item->text(0);
        const int separator = label.indexOf(QStringLiteral(" — "));
        if (separator >= 0) {
            currentTitle = label.mid(separator + 3);
        }
        bool accepted = false;
        const QString title = QInputDialog::getText(
            this, QStringLiteral("Rename chapter"), QStringLiteral("Chapter title:"),
            QLineEdit::Normal, currentTitle, &accepted);
        if (!accepted) {
            return;
        }
        result = chapterStructure_->renameChapter(chapterId, title);
        successMessage = QStringLiteral("Chapter title saved.");
    } else if (chosen == moveUp || chosen == moveDown) {
        result = chapterStructure_->moveChapter(chapterId, row + (chosen == moveUp ? -1 : 1));
        successMessage = QStringLiteral("Chapter order saved.");
    } else if (chosen == joinPrevious || chosen == joinNext) {
        ChapterEditSource source;
        QString readError;
        if (!readChapterSource(&source, &readError)) {
            QMessageBox::warning(this, QStringLiteral("Chapter could not be joined"), readError);
            return;
        }
        result = chapterStructure_->joinChapter(
            chapterId, chosen == joinPrevious, source);
        successMessage = QStringLiteral("Chapters joined.");
    } else if (chosen == remove) {
        if (QMessageBox::question(
                this, QStringLiteral("Delete chapter"),
                QStringLiteral("Remove this chapter from the book? Its file remains available for undo and recovery."),
                QMessageBox::Yes | QMessageBox::Cancel,
                QMessageBox::Cancel) != QMessageBox::Yes) {
            return;
        }
        ChapterEditSource source;
        QString readError;
        if (!readChapterSource(&source, &readError)) {
            QMessageBox::warning(this, QStringLiteral("Chapter could not be deleted"), readError);
            return;
        }
        result = chapterStructure_->deleteChapter(chapterId, source);
        successMessage = QStringLiteral("Chapter deleted.");
    }
    if (chosen == rename || chosen == moveUp || chosen == moveDown ||
        chosen == joinPrevious || chosen == joinNext || chosen == remove) {
        applyChapterStructureResult(result, bookId, successMessage);
    }
}

void LibraryWindow::handleLibraryDrop(
    QTreeWidgetItem *source, QTreeWidgetItem *target,
    LibraryDropPosition position)
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
                    if (position == LibraryDropPosition::AboveItem) {
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
            if (position == LibraryDropPosition::AboveItem) {
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

bool LibraryWindow::openChapter(QTreeWidgetItem *item, bool quietStatus)
{
    if (!item || item->data(0, ItemKindRole).toInt() != ChapterItemKind) {
        return false;
    }
    const QString bookId = item->data(0, BookIdRole).toString();
    const QString chapterId = item->data(0, ChapterIdRole).toString();
    if (!ensureChapterStructure(bookId)) {
        return false;
    }
    const QString relativePath = bookId + QStringLiteral("/chapters/") + chapterId +
                                 QStringLiteral(".html");
    const QString title = item->parent()->text(0) + QStringLiteral(" — ") + item->text(0);
    const bool opened = openDocument(relativePath, title,
                                     loadChapterLinks(activeLibraryPath_, bookId, chapterId),
                                     false, quietStatus);
    if (opened) {
        activeBookId_ = bookId;
        progress_ = std::make_unique<WritingProgress>(activeLibraryPath_);
        QString progressError;
        if (!progress_->load(bookId, &progressError)) {
            progress_.reset();
            statusBar()->showMessage(QStringLiteral("Progress unavailable: %1").arg(progressError));
        } else {
            refreshProgress();
        }
        refreshBookPages();
        updateChapterNavigation();
        updateChapterStructureActions();
    }
    return opened;
}

bool LibraryWindow::openOutline(QTreeWidgetItem *item)
{
    if (!item || item->data(0, ItemKindRole).toInt() != OutlineItemKind) {
        return false;
    }
    const QString bookId = item->data(0, BookIdRole).toString();
    const QString relativePath = bookId + QStringLiteral("/outline.html");
    const QString title = item->parent()->text(0) + QStringLiteral(" — Outline");
    const bool opened = openDocument(relativePath, title, {}, true);
    if (opened) {
        refreshBookPages();
        updateChapterNavigation();
    }
    return opened;
}

void LibraryWindow::refreshBookPages()
{
    // Reparenting the editor drops keyboard focus; give it back so a chapter opened from the keyboard accepts typing.
    const bool editorHadFocus = chapterEditor_->hasFocus();
    const auto restoreFocus = qScopeGuard([this, editorHadFocus] {
        if (editorHadFocus && !chapterReadOnly_) chapterEditor_->setFocus();
    });
    bookPagesLayout_->removeWidget(chapterEditor_);
    chapterEditor_->setParent(bookPages_);
    while (QLayoutItem *entry = bookPagesLayout_->takeAt(0)) {
        if (QWidget *oldPage = entry->widget(); oldPage && oldPage != chapterEditor_) {
            oldPage->hide();
            oldPage->setParent(nullptr);
            oldPage->deleteLater();
        }
        delete entry;
    }

    if (activeDocumentIsOutline_) {
        bookPagesLayout_->addWidget(chapterEditor_);
        resizeChapterEditorToContents();
        return;
    }
    QTreeWidgetItem *bookItem = nullptr;
    for (int index = 0; index < tree_->topLevelItemCount() && !bookItem; ++index) {
        bookItem = findBookItem(tree_->topLevelItem(index), activeBookId_);
    }
    if (!bookItem) {
        bookPagesLayout_->addWidget(chapterEditor_);
        return;
    }

    const QString activeId = QFileInfo(activeChapterRelativePath_).completeBaseName();
    QWidget *activePage = nullptr;
    for (int index = 0; index < bookItem->childCount(); ++index) {
        QTreeWidgetItem *item = bookItem->child(index);
        if (item->data(0, ItemKindRole).toInt() != ChapterItemKind) {
            continue;
        }
        const QString id = item->data(0, ChapterIdRole).toString();
        auto *page = new QWidget(bookPages_);
        page->setObjectName(QStringLiteral("chapter-page-") + id);
        page->setFocusPolicy(Qt::NoFocus);
        const bool night = activePreferences_.pageTheme == QStringLiteral("night");
        page->setStyleSheet(QStringLiteral("background: %1; color: %2;")
            .arg(night ? QStringLiteral("#262329") : QStringLiteral("#fffdf7"),
                 night ? QStringLiteral("#e8dfd4") : QStringLiteral("#26211e")));
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(24, 24, 24, 24);
        auto *heading = new QPushButton(item->text(0), page);
        heading->setObjectName(QStringLiteral("chapter-page-heading-") + id);
        heading->setAccessibleName(QStringLiteral("Open ") + item->text(0));
        heading->setFlat(true);
        layout->addWidget(heading);
        connect(heading, &QPushButton::clicked, this, [this, id] {
            QTreeWidgetItem *selectedBook = nullptr;
            for (int root = 0; root < tree_->topLevelItemCount() && !selectedBook; ++root) {
                selectedBook = findBookItem(tree_->topLevelItem(root), activeBookId_);
            }
            if (!selectedBook) {
                return;
            }
            for (int row = 0; row < selectedBook->childCount(); ++row) {
                QTreeWidgetItem *chapter = selectedBook->child(row);
                if (chapter->data(0, ChapterIdRole).toString() == id && openChapter(chapter)) {
                    tree_->setCurrentItem(chapter);
                    return;
                }
            }
        });
        if (id == activeId) {
            layout->addWidget(chapterEditor_);
            activePage = page;
        } else {
            const QString relativePath = activeBookId_ + QStringLiteral("/chapters/") + id +
                                         QStringLiteral(".html");
            QByteArray bytes;
            QString error;
            QString preview = QStringLiteral("Chapter could not be read safely.");
            if (LibraryPersistence::readLibraryFile(activeLibraryPath_, relativePath,
                                                    &bytes, &error)) {
                const LegacyChapterDocument document = LegacyChapterCodec::decode(
                    bytes, loadChapterLinks(activeLibraryPath_, activeBookId_, id));
                preview = document.editable() ? document.text : QString::fromUtf8(bytes);
            }
            auto *text = new QLabel(preview, page);
            text->setObjectName(QStringLiteral("chapter-preview-") + id);
            text->setWordWrap(true);
            text->setTextFormat(Qt::PlainText);
            text->setTextInteractionFlags(Qt::TextSelectableByMouse);
            text->setFocusPolicy(Qt::NoFocus);
            layout->addWidget(text);
        }
        page->setAutoFillBackground(true);
        bookPagesLayout_->addWidget(page);
    }
    bookPagesLayout_->addStretch();
    updatePresentation();
    if (activePage) {
        QPointer<QWidget> currentPage(activePage);
        QTimer::singleShot(0, this, [this, currentPage] {
            if (currentPage && currentPage->parent() == bookPages_) {
                bookFlow_->ensureWidgetVisible(currentPage, 0, 24);
            }
        });
    }
}

void LibraryWindow::resizeChapterEditorToContents()
{
    if (!chapterEditor_) {
        return;
    }
    const int height = qBound(480, qCeil(chapterEditor_->document()->size().height()) + 32,
                              16000000);
    chapterEditor_->setFixedHeight(height);
}

void LibraryWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    // A modal dialog closing can leave the reactivated window with nothing focused; keep keyboard writing alive.
    if (event->type() == QEvent::ActivationChange && isActiveWindow() && chapterEditor_ && pages_ &&
        pages_->currentWidget() == editorPage_ && !chapterReadOnly_ && !QApplication::activeModalWidget() &&
        !QApplication::focusWidget()) {
        chapterEditor_->setFocus();
    }
}

bool LibraryWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (chapterEditor_ && watched == chapterEditor_->viewport() &&
        event->type() == QEvent::Wheel && bookFlow_) {
        auto *wheel = static_cast<QWheelEvent *>(event);
        const int distance = !wheel->pixelDelta().isNull()
            ? wheel->pixelDelta().y()
            : wheel->angleDelta().y() * chapterEditor_->fontMetrics().lineSpacing() / 40;
        QScrollBar *bar = bookFlow_->verticalScrollBar();
        bar->setValue(bar->value() - distance);
        return true;
    }
    return QMainWindow::eventFilter(watched, event);
}

void LibraryWindow::navigateChapter(int direction)
{
    if (activeDocumentIsOutline_ || activeBookId_.isEmpty() ||
        activeChapterRelativePath_.isEmpty()) {
        return;
    }
    QTreeWidgetItem *bookItem = nullptr;
    for (int index = 0; index < tree_->topLevelItemCount() && !bookItem; ++index) {
        bookItem = findBookItem(tree_->topLevelItem(index), activeBookId_);
    }
    if (!bookItem) {
        return;
    }
    const QString chapterId = QFileInfo(activeChapterRelativePath_).completeBaseName();
    for (int index = 0; index < bookItem->childCount(); ++index) {
        QTreeWidgetItem *item = bookItem->child(index);
        if (item->data(0, ChapterIdRole).toString() != chapterId) {
            continue;
        }
        const int destination = index + direction;
        if (destination >= 0 && destination < bookItem->childCount() &&
            bookItem->child(destination)->data(0, ItemKindRole).toInt() == ChapterItemKind) {
            QTreeWidgetItem *next = bookItem->child(destination);
            if (openChapter(next)) {
                tree_->setCurrentItem(next);
            }
        }
        return;
    }
}

void LibraryWindow::updateChapterNavigation()
{
    previousChapterButton_->setEnabled(false);
    nextChapterButton_->setEnabled(false);
    if (activeDocumentIsOutline_ || activeBookId_.isEmpty()) {
        return;
    }
    QTreeWidgetItem *bookItem = nullptr;
    for (int index = 0; index < tree_->topLevelItemCount() && !bookItem; ++index) {
        bookItem = findBookItem(tree_->topLevelItem(index), activeBookId_);
    }
    if (!bookItem) {
        return;
    }
    const QString chapterId = QFileInfo(activeChapterRelativePath_).completeBaseName();
    for (int index = 0; index < bookItem->childCount(); ++index) {
        if (bookItem->child(index)->data(0, ChapterIdRole).toString() != chapterId) {
            continue;
        }
        previousChapterButton_->setEnabled(index > 0 &&
            bookItem->child(index - 1)->data(0, ItemKindRole).toInt() == ChapterItemKind);
        nextChapterButton_->setEnabled(index + 1 < bookItem->childCount() &&
            bookItem->child(index + 1)->data(0, ItemKindRole).toInt() == ChapterItemKind);
        return;
    }
}

bool LibraryWindow::ensureChapterStructure(const QString &bookId)
{
    if (activeLibraryPath_.isEmpty() || bookId.isEmpty()) {
        return false;
    }
    if (chapterStructure_ && structureBookId_ == bookId) {
        return true;
    }

    auto structure = std::make_unique<ChapterStructure>(activeLibraryPath_, bookId);
    QString error;
    if (!structure->load(&error)) {
        if (pages_->currentWidget() == editorPage_) {
            updateEditorState(error);
        } else {
            QMessageBox::warning(this, QStringLiteral("Chapter structure unavailable"), error);
        }
        return false;
    }
    chapterStructure_ = std::move(structure);
    structureBookId_ = bookId;
    updateChapterStructureActions();
    return true;
}

void LibraryWindow::splitActiveChapter(const QString &text, int position, bool notify)
{
    if (chapterReadOnly_ || activeDocumentIsOutline_ || activeBookId_.isEmpty() ||
        chapterDocument_.hasProtectedContent()) {
        updateEditorState(QStringLiteral(
            "This chapter contains protected legacy content or is read-only. The split was refused without changing the Library."));
        return;
    }
    if (!ensureChapterStructure(activeBookId_)) {
        return;
    }
    QString encodeError;
    const QByteArray currentBytes = LegacyChapterCodec::encode(
        text, chapterDocument_.hasUtf8Bom, &encodeError);
    if (!encodeError.isEmpty()) {
        updateEditorState(encodeError);
        return;
    }
    const ChapterStructureResult result = chapterStructure_->splitChapter(
        QFileInfo(activeChapterRelativePath_).completeBaseName(), position,
        {currentBytes, sourceHash_, sourceBytes_}, QString());
    applyChapterStructureResult(
        result, activeBookId_, notify ? QStringLiteral("Chapter split.") : QString(), notify);
}

void LibraryWindow::applyChapterStructureResult(const ChapterStructureResult &result,
                                               const QString &bookId,
                                               const QString &successMessage,
                                               bool notify)
{
    if (!result.ok) {
        if (pages_->currentWidget() == editorPage_) {
            updateEditorState(result.error);
        } else {
            QMessageBox::warning(this, QStringLiteral("Chapter structure unchanged"),
                                 result.error);
        }
        if (notify) {
            statusBar()->showMessage(QStringLiteral("Chapter structure was not changed"));
        }
        updateChapterStructureActions();
        return;
    }
    if (!refreshChapterStructureView(bookId, result.chapterId, notify)) {
        return;
    }
    if (notify && !successMessage.isEmpty()) {
        statusBar()->showMessage(successMessage, 5000);
    }
}

bool LibraryWindow::refreshChapterStructureView(const QString &bookId,
                                               const QString &chapterId,
                                               bool notify)
{
    saveTimer_->stop();
    chapterDirty_ = false;
    chapterConflict_ = false;
    saveFailed_ = false;
    const QString libraryPath = activeLibraryPath_;
    if (libraryPath.isEmpty()) {
        return false;
    }
    loadingChapter_ = true;
    const LibraryReadResult result = LibraryReader::read(libraryPath);
    if (!result.ok()) {
        loadingChapter_ = false;
        pages_->setCurrentWidget(libraryPage_);
        const QString message =
            QStringLiteral("Chapter structure was saved, but the Library could not be refreshed: %1")
                .arg(result.error);
        if (notify) {
            statusBar()->showMessage(message);
        } else {
            updateEditorState(message);
        }
        updateChapterStructureActions();
        return false;
    }
    applyPreferences(result.library.preferences);
    populateLibraryTree(result.library);
    pages_->setCurrentWidget(libraryPage_);
    if (chapterId.isEmpty()) {
        loadingChapter_ = false;
        updateChapterStructureActions();
        return true;
    }

    std::function<QTreeWidgetItem *(QTreeWidgetItem *)> findChapter =
        [&](QTreeWidgetItem *parent) -> QTreeWidgetItem * {
        if (parent->data(0, ItemKindRole).toInt() == ChapterItemKind &&
            parent->data(0, BookIdRole).toString() == bookId &&
            parent->data(0, ChapterIdRole).toString() == chapterId) {
            return parent;
        }
        for (int index = 0; index < parent->childCount(); ++index) {
            if (QTreeWidgetItem *found = findChapter(parent->child(index))) {
                return found;
            }
        }
        return nullptr;
    };
    QTreeWidgetItem *chapterItem = nullptr;
    for (int index = 0; index < tree_->topLevelItemCount() && !chapterItem; ++index) {
        chapterItem = findChapter(tree_->topLevelItem(index));
    }
    bool refreshed = false;
    if (chapterItem) {
        tree_->setCurrentItem(chapterItem);
        refreshed = openChapter(chapterItem, !notify) && !saveFailed_;
        if (!refreshed) {
            loadingChapter_ = false;
            pages_->setCurrentWidget(libraryPage_);
            const QString message = QStringLiteral(
                "Chapter structure was saved, but the selected chapter could not be opened.");
            if (notify) {
                statusBar()->showMessage(message);
            } else {
                updateEditorState(message);
            }
        }
    } else {
        loadingChapter_ = false;
        pages_->setCurrentWidget(libraryPage_);
        const QString message = QStringLiteral(
            "Chapter structure was saved, but the selected chapter could not be found.");
        if (notify) {
            statusBar()->showMessage(message);
        } else {
            updateEditorState(message);
        }
    }
    updateChapterStructureActions();
    return refreshed;
}

void LibraryWindow::updateChapterStructureActions()
{
    const bool usable = chapterStructure_ && !chapterDirty_ &&
                        !chapterConflict_ && !saveFailed_;
    if (undoStructureAction_) {
        undoStructureAction_->setEnabled(usable && chapterStructure_->canUndo());
    }
    if (redoStructureAction_) {
        redoStructureAction_->setEnabled(usable && chapterStructure_->canRedo());
    }
}

void LibraryWindow::undoChapterStructure()
{
    if (!chapterStructure_ || !chapterStructure_->canUndo() ||
        !savePendingEdits()) {
        return;
    }
    const QString bookId = structureBookId_;
    applyChapterStructureResult(chapterStructure_->undo(), bookId,
                                QStringLiteral("Chapter structure undone."));
}

void LibraryWindow::cutSelectionToDarlings()
{
    if (activeBookId_.isEmpty() || activeDocumentIsOutline_ || chapterReadOnly_ ||
        pages_->currentWidget() != editorPage_) return;
    const QTextCursor selection = chapterEditor_->textCursor();
    if (!selection.hasSelection()) {
        statusBar()->showMessage(QStringLiteral("Select passage text to save in Darlings."), 5000);
        return;
    }
    const QString bookId = activeBookId_;
    const QString chapterId = QFileInfo(activeChapterRelativePath_).completeBaseName();
    const int start = selection.selectionStart();
    const int end = selection.selectionEnd();
    if (!savePendingEdits()) return;
    const DarlingResult result = DarlingRecords(activeLibraryPath_, bookId).cut(chapterId, start, end);
    if (!result.ok) {
        QMessageBox::warning(this, QStringLiteral("Darling was not saved"), result.error);
        return;
    }
    chapterStructure_.reset();
    structureBookId_.clear();
    refreshChapterStructureView(bookId, chapterId, true);
    statusBar()->showMessage(QStringLiteral("Selection saved in Darlings."), 5000);
}

void LibraryWindow::manageDarlings()
{
    if (activeBookId_.isEmpty()) return;
    const QString bookId = activeBookId_;
    if (!savePendingEdits()) return;
    DarlingRecords darlings(activeLibraryPath_, bookId);
    QJsonArray records;
    QString error;
    if (!darlings.list(&records, &error)) {
        QMessageBox::warning(this, QStringLiteral("Darlings could not be opened"), error);
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Darlings"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *list = new QListWidget(&dialog);
    list->setObjectName(QStringLiteral("darlings-list"));
    for (const QJsonValue &value : records) {
        const QJsonObject record = value.toObject();
        const QString preview = record.value(QStringLiteral("text")).toString().simplified().left(100);
        auto *item = new QListWidgetItem(
            record.value(QStringLiteral("chapterLabel")).toString() +
                QStringLiteral(" — ") + preview, list);
        item->setData(Qt::UserRole, record.value(QStringLiteral("id")).toString());
    }
    layout->addWidget(list);
    auto *restore = new QPushButton(QStringLiteral("Restore selected"), &dialog);
    restore->setObjectName(QStringLiteral("darling-restore"));
    layout->addWidget(restore);
    connect(restore, &QPushButton::clicked, &dialog, [this, &dialog, list, bookId, &darlings] {
        if (!list->currentItem()) return;
        const QString id = list->currentItem()->data(Qt::UserRole).toString();
        DarlingResult result = darlings.restore(id);
        if (result.review) {
            if (QMessageBox::question(
                    &dialog, QStringLiteral("Review restoration location"),
                    result.error + QStringLiteral(" Restore to the end of the available chapter?"),
                    QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
                return;
            result = darlings.restore(id, true);
        }
        if (!result.ok) {
            QMessageBox::warning(&dialog, QStringLiteral("Darling was not restored"), result.error);
            return;
        }
        chapterStructure_.reset();
        structureBookId_.clear();
        dialog.accept();
        refreshChapterStructureView(bookId, result.chapterId, true);
        statusBar()->showMessage(QStringLiteral("Darling restored."), 5000);
    });
    dialog.exec();
}

void LibraryWindow::redoChapterStructure()
{
    if (!chapterStructure_ || !chapterStructure_->canRedo() ||
        !savePendingEdits()) {
        return;
    }
    const QString bookId = structureBookId_;
    applyChapterStructureResult(chapterStructure_->redo(), bookId,
                                QStringLiteral("Chapter structure redone."));
}

void LibraryWindow::showFindReplace()
{
    if (activeLibraryPath_.isEmpty() || activeBookId_.isEmpty() || activeDocumentIsOutline_) {
        statusBar()->showMessage(QStringLiteral("Open a chapter to search its book."), 5000);
        return;
    }
    if (!savePendingEdits()) return;
    if (!bookSearch_ || searchBookId_ != activeBookId_) {
        bookSearch_ = std::make_unique<BookSearch>(activeLibraryPath_, activeBookId_);
        searchBookId_ = activeBookId_;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Find and Replace in Book"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *find = new QLineEdit(&dialog);
    find->setObjectName(QStringLiteral("book-find-input"));
    find->setPlaceholderText(QStringLiteral("Find text across chapters"));
    auto *replacement = new QLineEdit(&dialog);
    replacement->setObjectName(QStringLiteral("book-replace-input"));
    replacement->setPlaceholderText(QStringLiteral("Replace with"));
    auto *hits = new QTreeWidget(&dialog);
    hits->setObjectName(QStringLiteral("book-find-results"));
    hits->setAccessibleName(QStringLiteral("Find results"));
    hits->setHeaderLabels({QStringLiteral("Chapter"), QStringLiteral("Match")});
    auto *message = new QLabel(&dialog);
    message->setWordWrap(true);
    layout->addWidget(find);
    layout->addWidget(replacement);
    layout->addWidget(hits);
    layout->addWidget(message);
    auto *buttons = new QHBoxLayout;
    auto *findButton = new QPushButton(QStringLiteral("Find"), &dialog);
    auto *oneButton = new QPushButton(QStringLiteral("Replace selected"), &dialog);
    auto *allButton = new QPushButton(QStringLiteral("Replace all"), &dialog);
    auto *undoButton = new QPushButton(QStringLiteral("Undo batch"), &dialog);
    buttons->addWidget(findButton);
    buttons->addWidget(oneButton);
    buttons->addWidget(allButton);
    buttons->addWidget(undoButton);
    layout->addLayout(buttons);
    const auto refreshChapter = [this] {
        if (activeChapterRelativePath_.isEmpty()) return;
        const QString path = activeChapterRelativePath_;
        const QString title = editorTitle_->text();
        const auto links = chapterLinks_;
        if (openDocument(path, title, links, false, true)) refreshBookPages();
    };
    const auto scan = [this, find, hits, message] {
        hits->clear();
        const auto result = bookSearch_->find(find->text());
        if (!result.ok) { message->setText(result.error); return; }
        int blocked = 0;
        for (const auto &hit : result.hits) {
            auto *row = new QTreeWidgetItem(hits, {hit.chapterId,
                hit.blocked ? QStringLiteral("Blocked in protected or semantic content")
                            : QStringLiteral("Position %1").arg(hit.position + 1)});
            row->setData(0, Qt::UserRole, hit.position);
            row->setData(0, Qt::UserRole + 1, hit.blocked);
            if (hit.blocked) ++blocked;
        }
        message->setText(QStringLiteral("%1 matches across chapters; %2 blocked.")
                             .arg(result.hits.size()).arg(blocked));
    };
    connect(findButton, &QPushButton::clicked, &dialog, scan);
    connect(find, &QLineEdit::returnPressed, &dialog, scan);
    connect(hits, &QTreeWidget::itemDoubleClicked, &dialog,
            [this, find](QTreeWidgetItem *row) {
        QTreeWidgetItem *bookItem = nullptr;
        for (int index = 0; index < tree_->topLevelItemCount() && !bookItem; ++index)
            bookItem = findBookItem(tree_->topLevelItem(index), activeBookId_);
        if (!bookItem) return;
        for (int index = 0; index < bookItem->childCount(); ++index) {
            QTreeWidgetItem *chapter = bookItem->child(index);
            if (chapter->data(0, ChapterIdRole).toString() == row->text(0) &&
                openChapter(chapter, true)) {
                tree_->setCurrentItem(chapter);
                if (!row->data(0, Qt::UserRole + 1).toBool()) {
                    QTextCursor cursor = chapterEditor_->textCursor();
                    cursor.setPosition(row->data(0, Qt::UserRole).toInt());
                    cursor.setPosition(cursor.position() + find->text().size(),
                                       QTextCursor::KeepAnchor);
                    chapterEditor_->setTextCursor(cursor);
                }
                break;
            }
        }
    });
    connect(oneButton, &QPushButton::clicked, &dialog,
            [this, find, replacement, hits, message, scan, refreshChapter] {
        if (!savePendingEdits()) { message->setText(QStringLiteral("Save the current chapter before replacing.")); return; }
        auto *row = hits->currentItem();
        if (!row) { message->setText(QStringLiteral("Select a match first.")); return; }
        const auto result = bookSearch_->replaceOne(find->text(), replacement->text(),
            row->text(0), row->data(0, Qt::UserRole).toInt());
        if (!result.ok) { message->setText(result.error); return; }
        refreshChapter();
        scan();
        message->setText(QStringLiteral("One match replaced. Undo batch is available."));
    });
    connect(allButton, &QPushButton::clicked, &dialog,
            [this, find, replacement, message, scan, refreshChapter] {
        if (!savePendingEdits()) { message->setText(QStringLiteral("Save the current chapter before replacing.")); return; }
        const auto result = bookSearch_->replaceAll(find->text(), replacement->text());
        if (!result.ok) { message->setText(result.error); return; }
        refreshChapter();
        scan();
        message->setText(QStringLiteral("%1 matches replaced as one batch.").arg(result.changed));
    });
    connect(undoButton, &QPushButton::clicked, &dialog,
            [this, message, scan, refreshChapter] {
        if (!savePendingEdits()) { message->setText(QStringLiteral("Save the current chapter before undoing.")); return; }
        const auto result = bookSearch_->undo();
        if (!result.ok) { message->setText(result.error); return; }
        refreshChapter();
        scan();
        message->setText(QStringLiteral("Replacement batch undone."));
    });
    dialog.resize(520, 400);
    find->setFocus();
    dialog.exec();
}

void LibraryWindow::undoReplacement()
{
    if (!bookSearch_ || !bookSearch_->canUndo() || !savePendingEdits()) {
        statusBar()->showMessage(QStringLiteral("No replacement batch is available to undo."), 5000);
        return;
    }
    const auto result = bookSearch_->undo();
    if (!result.ok) { updateEditorState(result.error); return; }
    if (!activeChapterRelativePath_.isEmpty()) {
        const auto path = activeChapterRelativePath_;
        const auto title = editorTitle_->text();
        const auto links = chapterLinks_;
        if (openDocument(path, title, links, activeDocumentIsOutline_, true)) refreshBookPages();
    }
    statusBar()->showMessage(QStringLiteral("Replacement batch undone."), 5000);
}

void LibraryWindow::showSpellcheck()
{
    if (activeLibraryPath_.isEmpty() || activeChapterRelativePath_.isEmpty() ||
        chapterReadOnly_) {
        statusBar()->showMessage(QStringLiteral("Open editable prose to check spelling."), 5000);
        return;
    }
    Spellcheck spell(activeLibraryPath_);
    if (!spell.ready()) { updateEditorState(spell.error()); return; }
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Check Spelling"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *list = new QListWidget(&dialog);
    list->setObjectName(QStringLiteral("spell-results"));
    auto *suggestions = new QListWidget(&dialog);
    suggestions->setObjectName(QStringLiteral("spell-suggestions"));
    auto *message = new QLabel(&dialog);
    layout->addWidget(list);
    layout->addWidget(suggestions);
    layout->addWidget(message);
    auto *buttons = new QHBoxLayout;
    auto *change = new QPushButton(QStringLiteral("Use suggestion"), &dialog);
    auto *learn = new QPushButton(QStringLiteral("Add to dictionary"), &dialog);
    buttons->addWidget(change);
    buttons->addWidget(learn);
    layout->addLayout(buttons);
    const auto scan = [this, &spell, list, message] {
        list->clear();
        const QString text = chapterEditor_->toPlainText();
        for (const auto &miss : spell.check(text)) {
            bool protectedMatch = false;
            for (const auto &fragment : chapterDocument_.fragments) {
                if (fragment.kind != LegacyChapterContentKind::Protected) continue;
                const int tokenStart = text.indexOf(fragment.token);
                if (tokenStart >= 0 && miss.position >= tokenStart &&
                    miss.position < tokenStart + fragment.token.size()) {
                    protectedMatch = true;
                    break;
                }
            }
            if (protectedMatch) continue;
            auto *item = new QListWidgetItem(QStringLiteral("%1 — position %2")
                .arg(miss.word).arg(miss.position + 1), list);
            item->setData(Qt::UserRole, miss.word);
            item->setData(Qt::UserRole + 1, miss.position);
            item->setData(Qt::UserRole + 2, miss.length);
        }
        message->setText(QStringLiteral("%1 possible misspellings.").arg(list->count()));
    };
    connect(list, &QListWidget::currentItemChanged, &dialog,
            [&spell, suggestions](QListWidgetItem *item) {
        suggestions->clear();
        if (item) suggestions->addItems(spell.suggestions(item->data(Qt::UserRole).toString()));
    });
    connect(change, &QPushButton::clicked, &dialog,
            [this, list, suggestions, message, scan] {
        auto *item = list->currentItem();
        auto *choice = suggestions->currentItem();
        if (!item || !choice) { message->setText(QStringLiteral("Select a word and suggestion.")); return; }
        QTextCursor cursor = chapterEditor_->textCursor();
        cursor.setPosition(item->data(Qt::UserRole + 1).toInt());
        cursor.setPosition(item->data(Qt::UserRole + 1).toInt() +
                           item->data(Qt::UserRole + 2).toInt(), QTextCursor::KeepAnchor);
        cursor.insertText(choice->text());
        chapterEditor_->setTextCursor(cursor);
        scan();
    });
    connect(learn, &QPushButton::clicked, &dialog, [this, &spell, list, message, scan] {
        auto *item = list->currentItem();
        if (!item) { message->setText(QStringLiteral("Select a word first.")); return; }
        QString error;
        if (!spell.learn(item->data(Qt::UserRole).toString(), &error)) {
            message->setText(error); return;
        }
        if (organization_ && !organization_->load(&error)) {
            message->setText(QStringLiteral("Word saved, but Library organization needs reopening: %1")
                                 .arg(error));
            return;
        }
        scan();
    });
    scan();
    dialog.resize(440, 420);
    dialog.exec();
}

bool LibraryWindow::openDocument(const QString &relativePath,
                                 const QString &title,
                                 const LegacyChapterLinkContext &links,
                                 bool outline,
                                 bool quietStatus)
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
        if (!quietStatus) {
            statusBar()->showMessage(outline ? QStringLiteral("Outline could not be opened safely")
                                             : QStringLiteral("Chapter could not be opened safely"));
        }
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
    if (!chapterReadOnly_) {
        LegacyChapterCodec::applyFormatting(chapterDocument_, chapterEditor_->document());
    }
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
    const QString documentKind = outline ? QStringLiteral("Outline") : QStringLiteral("Chapter");
    chapterEditor_->setAccessibleName(
        (chapterReadOnly_ ? QStringLiteral("Read-only %1 source: %2").arg(documentKind.toLower(), title)
                          : QStringLiteral("%1 text: %2").arg(documentKind, title)));
    chapterEditor_->setAccessibleDescription(preferenceNotice_);
    if (!quietStatus) {
        QString status = chapterReadOnly_
                             ? (outline ? QStringLiteral("Outline is read-only")
                                        : QStringLiteral("Chapter is read-only"))
                             : (outline ? QStringLiteral("Outline open; no Library files changed")
                                        : QStringLiteral("Chapter open; no Library files changed"));
        if (!preferenceNotice_.isEmpty()) {
            status += QStringLiteral(" — ") + preferenceNotice_;
        }
        statusBar()->showMessage(status);
    }
    if (!chapterReadOnly_) {
        chapterEditor_->setFocus();
        // The stacked page shows and re-polishes after this call; claim focus again once that settles so
        // typing after a keyboard open lands in the chapter rather than the first button on the page.
        QPointer<QPlainTextEdit> editor(chapterEditor_);
        QTimer::singleShot(0, this, [this, editor] {
            if (editor && !chapterReadOnly_ && pages_->currentWidget() == editorPage_) {
                editor->setFocus();
            }
        });
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

    const QString fallbackFont = FontPreferences::bodyFallbackFamily();
    QString bodyFont = activePreferences_.bodyFont.trimmed();
    if (activePreferences_.bodyFontInvalid) {
        notices.append(QStringLiteral("Saved typeface choice is invalid; using '%1'.")
                           .arg(fallbackFont));
        bodyFont = fallbackFont;
    } else if (!bodyFont.isEmpty()) {
        const QString installed = FontPreferences::installedFamily({bodyFont});
        if (installed.isEmpty()) {
            notices.append(QStringLiteral("Saved typeface '%1' is unavailable; using '%2'.")
                               .arg(bodyFont, fallbackFont));
            bodyFont = fallbackFont;
        } else {
            bodyFont = installed;
        }
    } else {
        bodyFont = fallbackFont;
    }
    activePreferences_.bodyFont = bodyFont;
    QFont editorFont = chapterEditor_->font();
    editorFont.setFamily(bodyFont);
    editorFont.setPointSizeF(activePreferences_.editorFontSize * activePreferences_.pageZoom);
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
        ->setPreferences(activePreferences_.dropCapStyle, bodyFont,
                         activePreferences_.editorFontSize * activePreferences_.pageZoom);
    preferenceNotice_ = notices.join(QLatin1Char(' '));
    chapterEditor_->setAccessibleDescription(preferenceNotice_);
    updatePresentation();
}

void LibraryWindow::updatePresentation()
{
    const bool night = activePreferences_.pageTheme == QStringLiteral("night");
    const QString paperColor = night ? QStringLiteral("#262329") : QStringLiteral("#fffdf7");
    const QString inkColor = night ? QStringLiteral("#e8dfd4") : QStringLiteral("#26211e");
    bookFlow_->setStyleSheet(QStringLiteral("QScrollArea { background: %1; border: 0; }")
                                 .arg(night ? QStringLiteral("#18171b") : QStringLiteral("#ded8ce")));
    bookPages_->setStyleSheet(QStringLiteral("background: %1;").arg(paperColor));
    QFont font = chapterEditor_->font();
    font.setPointSizeF(activePreferences_.editorFontSize * activePreferences_.pageZoom);
    chapterEditor_->setFont(font);
    for (int index = 0; index < bookPagesLayout_->count(); ++index) {
        QWidget *page = bookPagesLayout_->itemAt(index)->widget();
        if (page && page != chapterEditor_) {
            page->setStyleSheet(QStringLiteral("background: %1; color: %2;")
                                    .arg(paperColor, inkColor));
            if (auto *preview = page->findChild<QLabel *>()) preview->setFont(font);
            if (auto *heading = page->findChild<QPushButton *>()) {
                QFont headingFont = font;
                headingFont.setBold(true);
                headingFont.setPointSizeF(font.pointSizeF() * 1.1);
                heading->setFont(headingFont);
            }
        }
    }
    chapterEditor_->setStyleSheet(QStringLiteral("QPlainTextEdit { color: %1; background: %2; "
                                                   "border: 0; selection-background-color: #9a7658; }")
                                      .arg(inkColor, paperColor));
    bookPagesLayout_->setContentsMargins(24, 24, 24,
        activePreferences_.typewriter ? qMax(400, bookFlow_->viewport()->height()) : 24);
    editorChrome_->setStyleSheet(activePreferences_.uiBright
        ? QStringLiteral("QWidget#writing-controls { background: #fff4d9; color: #241b16; } "
                         "QWidget#writing-controls QPushButton { background: #f7e5bd; "
                         "color: #241b16; border: 1px solid #785d36; padding: 4px; }")
        : QString());
    chromeHoverFilter_->setAttention(activePreferences_.chromePinned ||
                                     (saveFailed_ && !chapterConflict_));
    paperAction_->setChecked(!night);
    nightAction_->setChecked(night);
    brightAction_->setChecked(activePreferences_.uiBright);
    pinControlsAction_->setChecked(activePreferences_.chromePinned);
    typewriterAction_->setChecked(activePreferences_.typewriter);
    resizeChapterEditorToContents();
}

void LibraryWindow::savePresentationPreference(const QString &key, const QJsonValue &value)
{
    if (activeLibraryPath_.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Open a Library to save writing preferences."));
        updatePresentation();
        return;
    }
    QByteArray original;
    QString error;
    if (!LibraryPersistence::readLibraryFile(activeLibraryPath_, QStringLiteral("library.json"),
                                             &original, &error)) {
        statusBar()->showMessage(error);
        updatePresentation();
        return;
    }
    const QJsonDocument parsed = QJsonDocument::fromJson(original);
    if (!parsed.isObject()) {
        statusBar()->showMessage(QStringLiteral("Library preferences could not be read safely."));
        updatePresentation();
        return;
    }
    QJsonObject metadata = parsed.object();
    if (key == QStringLiteral("bodyFont") || key == QStringLiteral("dropCapStyle")) {
        QJsonObject fonts = metadata.value(QStringLiteral("fonts")).toObject();
        fonts.insert(key == QStringLiteral("bodyFont") ? QStringLiteral("body")
                                                         : QStringLiteral("dropcap"), value);
        metadata.insert(QStringLiteral("fonts"), fonts);
    } else {
        metadata.insert(key, value);
    }
    const PersistenceResult saved = LibraryPersistence::saveFile(
        activeLibraryPath_, QStringLiteral("library.json"), LibraryPersistence::hash(original),
        QJsonDocument(metadata).toJson(QJsonDocument::Indented));
    if (!saved.ok) {
        statusBar()->showMessage(QStringLiteral("Writing preference was not saved: %1").arg(saved.error));
        updatePresentation();
        return;
    }
    const bool organizationReady = !organization_ || organization_->load(&error);
    const LibraryReadResult refreshed = LibraryReader::read(activeLibraryPath_);
    if (refreshed.ok()) {
        applyPreferences(refreshed.library.preferences);
        statusBar()->showMessage(organizationReady
            ? QStringLiteral("Writing preferences saved.")
            : QStringLiteral("Preference saved; reopen the Library before organizing: %1").arg(error));
    } else {
        statusBar()->showMessage(refreshed.error);
    }
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
    const QByteArray newBytes = LegacyChapterCodec::encodeRich(
        chapterDocument_, chapterEditor_->document(), &encodeError);
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
    chapterDocument_ = LegacyChapterCodec::decode(newBytes, chapterLinks_);
    static_cast<ProtectedChapterEditor *>(chapterEditor_)
        ->refreshProtectedTokens(protectedTokens(chapterDocument_));
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
    updateChapterStructureActions();
    if (progress_ && !activeDocumentIsOutline_) {
        QString progressError;
        const int count = WritingProgress::countBook(activeLibraryPath_, activeBookId_,
                                                      &progressError);
        if (count >= 0 && !progress_->updateCount(count, QDateTime::currentDateTime(),
                                                  &progressError)) {
            statusBar()->showMessage(QStringLiteral("Chapter saved; progress not saved: %1")
                                         .arg(progressError));
        }
        refreshProgress();
    }
    return true;
}

bool LibraryWindow::ensurePlanningRecords()
{
    if (activeDocumentIsOutline_ || activeBookId_.isEmpty() ||
        activeChapterRelativePath_.isEmpty() || chapterReadOnly_ || chapterConflict_) {
        updateEditorState(QStringLiteral("Open an editable chapter to change planning records."));
        return false;
    }
    if (!savePendingEdits()) return false;
    if (!planningRecords_ || planningBookId_ != activeBookId_) {
        planningRecords_ = std::make_unique<PlanningRecords>(activeLibraryPath_, activeBookId_);
        planningBookId_ = activeBookId_;
    }
    return true;
}

void LibraryWindow::applyPlanningResult(const PlanningResult &result)
{
    if (!result.ok) {
        updateEditorState(result.error +
                          (result.conflict ? QStringLiteral(" Reopen the Library before retrying.")
                                           : QString()));
        return;
    }
    const int position = chapterEditor_->textCursor().position();
    const QString chapterId = QFileInfo(activeChapterRelativePath_).completeBaseName();
    const QString title = editorTitle_->text();
    if (openDocument(activeChapterRelativePath_, title,
                     loadChapterLinks(activeLibraryPath_, activeBookId_, chapterId), false, true)) {
        QTextCursor cursor = chapterEditor_->textCursor();
        cursor.setPosition(qMin(position, chapterEditor_->toPlainText().size()));
        chapterEditor_->setTextCursor(cursor);
        refreshBookPages();
        updateEditorState(QStringLiteral("Planning records saved with their chapter links."));
    }
}

void LibraryWindow::addPlanningSticky()
{
    if (!ensurePlanningRecords()) return;
    bool accepted = false;
    const QString note = QInputDialog::getMultiLineText(
        this, QStringLiteral("New sticky"), QStringLiteral("What needs doing here?"),
        QString(), &accepted);
    if (!accepted) return;
    applyPlanningResult(planningRecords_->addSticky(
        QFileInfo(activeChapterRelativePath_).completeBaseName(), note,
        chapterEditor_->textCursor().blockNumber()));
}

void LibraryWindow::addPlanningSection()
{
    if (!ensurePlanningRecords()) return;
    bool accepted = false;
    const QString note = QInputDialog::getMultiLineText(
        this, QStringLiteral("New section"), QStringLiteral("Section outline"),
        QString(), &accepted);
    if (!accepted) return;
    applyPlanningResult(planningRecords_->addSection(
        QFileInfo(activeChapterRelativePath_).completeBaseName(), note));
}

void LibraryWindow::showPlanningOutline()
{
    if (activeBookId_.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Open a book to view its outline."), 5000);
        return;
    }
    QByteArray bytes;
    QString error;
    if (!LibraryPersistence::readLibraryFile(activeLibraryPath_,
            activeBookId_ + QStringLiteral("/book.json"), &bytes, &error)) {
        updateEditorState(error);
        return;
    }
    const QJsonObject book = QJsonDocument::fromJson(bytes).object();
    const QJsonObject titles = book.value(QStringLiteral("chapterTitles")).toObject();
    const QJsonObject notes = book.value(QStringLiteral("chapterNotes")).toObject();
    const QJsonObject sections = book.value(QStringLiteral("sectionNotes")).toObject();
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Chapter and section outline"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *outline = new QTreeWidget(&dialog);
    outline->setHeaderLabels({QStringLiteral("Chapter / section"), QStringLiteral("Note")});
    const QJsonArray order = book.value(QStringLiteral("chapterOrder")).toArray();
    for (int index = 0; index < order.size(); ++index) {
        const QString chapterId = order.at(index).toString();
        auto *chapter = new QTreeWidgetItem(outline, {
            QStringLiteral("%1. %2").arg(index + 1).arg(titles.value(chapterId).toString()),
            notes.value(chapterId).toString()});
        const QJsonArray chapterSections = sections.value(chapterId).toArray();
        for (int sectionIndex = 0; sectionIndex < chapterSections.size(); ++sectionIndex) {
            const QJsonObject section = chapterSections.at(sectionIndex).toObject();
            new QTreeWidgetItem(chapter, {
                QStringLiteral("%1.%2").arg(index + 1).arg(sectionIndex + 1),
                section.value(QStringLiteral("text")).toString()});
        }
    }
    outline->expandAll();
    outline->resizeColumnToContents(0);
    layout->addWidget(outline);
    auto *close = new QPushButton(QStringLiteral("Close"), &dialog);
    connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    layout->addWidget(close);
    dialog.resize(640, 480);
    dialog.exec();
}

void LibraryWindow::editChapterNote()
{
    if (!ensurePlanningRecords()) return;
    QByteArray bytes;
    QString error;
    if (!LibraryPersistence::readLibraryFile(activeLibraryPath_,
            activeBookId_ + QStringLiteral("/book.json"), &bytes, &error)) {
        updateEditorState(error);
        return;
    }
    const QJsonObject book = QJsonDocument::fromJson(bytes).object();
    const QString chapterId = QFileInfo(activeChapterRelativePath_).completeBaseName();
    const QString current = book.value(QStringLiteral("chapterNotes")).toObject()
                                .value(chapterId).toString();
    bool accepted = false;
    const QString note = QInputDialog::getMultiLineText(
        this, QStringLiteral("Chapter note"), QStringLiteral("Plan this chapter"),
        current, &accepted);
    if (accepted) applyPlanningResult(planningRecords_->setChapterNote(chapterId, note));
}

void LibraryWindow::managePlanningRecords(bool sections)
{
    if (!ensurePlanningRecords()) return;
    const QString chapterId = QFileInfo(activeChapterRelativePath_).completeBaseName();
    QByteArray bookBytes, recordBytes;
    QString error;
    if (!LibraryPersistence::readLibraryFile(activeLibraryPath_,
            activeBookId_ + QStringLiteral("/book.json"), &bookBytes, &error)) {
        updateEditorState(error);
        return;
    }
    const QJsonObject book = QJsonDocument::fromJson(bookBytes).object();
    QJsonArray records;
    if (sections) {
        records = book.value(QStringLiteral("sectionNotes")).toObject()
                     .value(chapterId).toArray();
    } else {
        if (!LibraryPersistence::readLibraryFile(activeLibraryPath_,
                activeBookId_ + QStringLiteral("/stickies.json"), &recordBytes, &error)) {
            updateEditorState(error);
            return;
        }
        for (const QJsonValue &value : QJsonDocument::fromJson(recordBytes).array()) {
            if (value.toObject().value(QStringLiteral("chapterId")).toString() == chapterId)
                records.append(value);
        }
    }
    if (records.isEmpty()) {
        updateEditorState(sections ? QStringLiteral("This chapter has no sections.")
                                   : QStringLiteral("This chapter has no stickies."));
        return;
    }
    QStringList choices;
    for (const QJsonValue &value : records) {
        const QJsonObject record = value.toObject();
        choices.append(record.value(QStringLiteral("id")).toString() +
                       QStringLiteral(" — ") + record.value(QStringLiteral("text")).toString().left(80));
    }
    bool accepted = false;
    const QString selected = QInputDialog::getItem(
        this, sections ? QStringLiteral("Sections") : QStringLiteral("Stickies"),
        QStringLiteral("Choose a record"), choices, 0, false, &accepted);
    if (!accepted) return;
    const QJsonObject record = records.at(choices.indexOf(selected)).toObject();
    const QString id = record.value(QStringLiteral("id")).toString();
    const QString action = QInputDialog::getItem(
        this, QStringLiteral("Planning record"), QStringLiteral("Action"),
        {QStringLiteral("Edit note"), QStringLiteral("Move to chapter"),
         QStringLiteral("Copy to chapter")}, 0, false, &accepted);
    if (!accepted) return;
    if (action == QStringLiteral("Edit note")) {
        const QString note = QInputDialog::getMultiLineText(
            this, QStringLiteral("Edit note"), QStringLiteral("Note"),
            record.value(QStringLiteral("text")).toString(), &accepted);
        if (!accepted) return;
        applyPlanningResult(sections
            ? planningRecords_->updateSection(chapterId, id, note)
            : planningRecords_->updateSticky(id, note));
        return;
    }
    QStringList destinations;
    QStringList destinationIds;
    const QJsonObject titles = book.value(QStringLiteral("chapterTitles")).toObject();
    for (const QJsonValue &value : book.value(QStringLiteral("chapterOrder")).toArray()) {
        const QString targetId = value.toString();
        destinationIds.append(targetId);
        destinations.append(titles.value(targetId).toString(targetId) +
                            QStringLiteral(" (%1)").arg(targetId));
    }
    const QString destination = QInputDialog::getItem(
        this, QStringLiteral("Choose chapter"), QStringLiteral("Destination"),
        destinations, destinationIds.indexOf(chapterId), false, &accepted);
    if (!accepted) return;
    const QString targetId = destinationIds.at(destinations.indexOf(destination));
    const bool copy = action == QStringLiteral("Copy to chapter");
    applyPlanningResult(sections
        ? planningRecords_->transferSection(chapterId, id, targetId, copy)
        : planningRecords_->transferSticky(id, targetId, copy));
}

void LibraryWindow::undoPlanningChange()
{
    if (!ensurePlanningRecords()) return;
    applyPlanningResult(planningRecords_->undo());
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
    chromeHoverFilter_->setAttention(activePreferences_.chromePinned ||
                                     (saveFailed_ && !chapterConflict_));
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

void LibraryWindow::refreshProgress()
{
    if (!progress_ || activeDocumentIsOutline_ || !chapterEditor_) {
        if (progressCount_) progressCount_->clear();
        if (progressGoal_) progressGoal_->clear();
        return;
    }
    QString error;
    const int total = WritingProgress::countBook(
        activeLibraryPath_, activeBookId_, &error,
        chapterReadOnly_ ? QString() : activeChapterRelativePath_,
        chapterReadOnly_ ? QString() : chapterEditor_->toPlainText());
    if (total < 0) {
        progressCount_->setText(QStringLiteral("Count unavailable"));
        progressGoal_->clear();
        return;
    }
    const QString selected = chapterEditor_->textCursor().selectedText();
    const int selectionCount = WritingProgress::countWords(selected);
    QTextDocument renderedChapter;
    if (chapterReadOnly_) renderedChapter.setHtml(QString::fromUtf8(sourceBytes_));
    const int chapterCount = WritingProgress::countWords(
        chapterReadOnly_ ? renderedChapter.toPlainText() : chapterEditor_->toPlainText());
    QString countLabel = QStringLiteral("%1 book · %2 chapter").arg(total).arg(chapterCount);
    if (selectionCount > 0)
        countLabel += QStringLiteral(" · %1 selected").arg(selectionCount);
    progressCount_->setText(countLabel);
    const WritingProgressSnapshot snapshot = progress_->snapshot();
    if (sprintRunning_ && sprintBookId_ == activeBookId_) {
        const int gained = total - sprintStart_;
        if (gained >= sprintTarget_) {
            progressGoal_->setText(QStringLiteral("Sprint complete: %1 / %2")
                                       .arg(gained).arg(sprintTarget_));
        } else {
            progressGoal_->setText(QStringLiteral("Sprint %1 / %2")
                                       .arg(gained).arg(sprintTarget_));
        }
    } else {
        const int today = snapshot.todayWords() + total - snapshot.bookWords;
        progressGoal_->setText(snapshot.dailyGoal > 0
            ? QStringLiteral("%1 / %2 today").arg(today).arg(snapshot.dailyGoal)
            : QStringLiteral("%1 today").arg(today));
    }
}

void LibraryWindow::showProgress()
{
    if (!progress_ || activeBookId_.isEmpty() || activeDocumentIsOutline_) {
        statusBar()->showMessage(QStringLiteral("Open a book chapter to view goals and sprints."));
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Writing progress"));
    auto *layout = new QVBoxLayout(&dialog);
    const WritingProgressSnapshot snapshot = progress_->snapshot();
    QString countError;
    const int total = WritingProgress::countBook(activeLibraryPath_, activeBookId_,
        &countError, chapterReadOnly_ ? QString() : activeChapterRelativePath_,
        chapterReadOnly_ ? QString() : chapterEditor_->toPlainText());
    if (total < 0) {
        layout->addWidget(new QLabel(countError, &dialog));
        dialog.exec();
        return;
    }
    layout->addWidget(new QLabel(QStringLiteral("%1 words in book · %2 today")
        .arg(total).arg(snapshot.todayWords() + total - snapshot.bookWords), &dialog));
    // Draw the chart with a lightweight child that uses the persisted daily history.
    class Chart final : public QWidget {
    public:
        explicit Chart(const WritingProgressSnapshot &snapshot, QWidget *parent)
            : QWidget(parent), data(snapshot) {
            setMinimumSize(520, 170);
            setAccessibleName(QStringLiteral("Thirty day chart of daily words and book total"));
        }
    protected:
        void paintEvent(QPaintEvent *) override {
            QPainter painter(this);
            painter.fillRect(rect(), palette().base());
            const QRect plot = rect().adjusted(12, 10, -12, -24);
            int maxDaily = qMax(1, data.dailyGoal);
            int maxTotal = qMax(1, data.bookGoal);
            int previous = 0;
            for (int i = 29; i >= 0; --i) {
                const auto it = data.history.constFind(data.day.addDays(-i));
                if (it != data.history.cend()) {
                    maxDaily = qMax(maxDaily, qMax(0, it->end - it->start));
                    maxTotal = qMax(maxTotal, it->end);
                }
            }
            QPainterPath line;
            for (int i = 0; i < 30; ++i) {
                const auto it = data.history.constFind(data.day.addDays(i - 29));
                int daily = 0;
                if (it != data.history.cend()) {
                    daily = qMax(0, it->end - it->start);
                    previous = it->end;
                }
                const qreal x = plot.left() + plot.width() * (i + 0.5) / 30.0;
                const qreal width = plot.width() / 30.0 - 2;
                const qreal bar = plot.height() * 0.45 * daily / maxDaily;
                painter.fillRect(QRectF(x - width / 2, plot.bottom() - bar, width, bar),
                                 QColor(QStringLiteral("#3d8a6a")));
                const qreal y = plot.bottom() - plot.height() * previous / maxTotal;
                if (i == 0) line.moveTo(x, y); else line.lineTo(x, y);
            }
            painter.setPen(QPen(QColor(QStringLiteral("#c9a86a")), 2));
            painter.drawPath(line);
            painter.drawText(rect().adjusted(12, 0, -12, -4), Qt::AlignBottom,
                             QStringLiteral("30 days ago       daily words (bars) · book total (line)       today"));
        }
    private:
        WritingProgressSnapshot data;
    };
    auto *chart = new Chart(snapshot, &dialog);
    chart->setObjectName(QStringLiteral("writing-30-day-chart"));
    layout->addWidget(chart);
    auto *form = new QFormLayout;
    auto *daily = new QSpinBox(&dialog);
    daily->setObjectName(QStringLiteral("daily-goal"));
    daily->setRange(0, 10000000);
    daily->setValue(snapshot.dailyGoal);
    form->addRow(QStringLiteral("Daily goal"), daily);
    auto *book = new QSpinBox(&dialog);
    book->setObjectName(QStringLiteral("book-goal"));
    book->setRange(0, 100000000);
    book->setValue(snapshot.bookGoal);
    form->addRow(QStringLiteral("Book goal"), book);
    auto *boundary = new QSpinBox(&dialog);
    boundary->setObjectName(QStringLiteral("writing-day-boundary"));
    boundary->setRange(0, 6);
    boundary->setSuffix(QStringLiteral(":00 local time"));
    boundary->setValue(snapshot.dayEndsAt);
    form->addRow(QStringLiteral("Writing day ends at"), boundary);
    auto *target = new QSpinBox(&dialog);
    target->setObjectName(QStringLiteral("sprint-target"));
    target->setRange(1, 10000000);
    target->setValue(sprintTarget_ > 0 ? sprintTarget_ : 500);
    form->addRow(QStringLiteral("Sprint target (words)"), target);
    layout->addLayout(form);
    const bool activeSprint = sprintRunning_ && sprintBookId_ == activeBookId_;
    auto *sprintButton = new QPushButton(activeSprint
        ? (total - sprintStart_ >= sprintTarget_ ? QStringLiteral("Start new sprint")
                                               : QStringLiteral("Cancel sprint"))
        : QStringLiteral("Start sprint"), &dialog);
    sprintButton->setObjectName(QStringLiteral("sprint-button"));
    connect(sprintButton, &QPushButton::clicked, &dialog, [this, target, total, sprintButton] {
        if (sprintRunning_ && sprintBookId_ == activeBookId_ &&
            total - sprintStart_ < sprintTarget_) {
            sprintRunning_ = false;
            sprintButton->setText(QStringLiteral("Start sprint"));
        } else {
            sprintStart_ = total;
            sprintTarget_ = target->value();
            sprintBookId_ = activeBookId_;
            sprintRunning_ = true;
            sprintButton->setText(QStringLiteral("Cancel sprint"));
        }
        refreshProgress();
    });
    layout->addWidget(sprintButton);
    auto *done = new QPushButton(QStringLiteral("Done"), &dialog);
    connect(done, &QPushButton::clicked, &dialog, [&] {
        QString error;
        if (!progress_->setGoals(daily->value(), book->value(), boundary->value(), &error)) {
            QMessageBox::warning(&dialog, QStringLiteral("Goals not saved"), error);
            return;
        }
        refreshProgress();
        dialog.accept();
    });
    layout->addWidget(done);
    dialog.exec();
}
