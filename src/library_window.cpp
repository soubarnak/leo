#include "library_window.h"

#include "app_paths.h"
#include "legacy_chapter_codec.h"
#include "library_persistence.h"
#include "library_reader.h"
#include "release_check_dialog.h"

#include <QAction>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTextCursor>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace {

constexpr int ItemKindRole = Qt::UserRole + 1;
constexpr int BookIdRole = Qt::UserRole + 2;
constexpr int ChapterIdRole = Qt::UserRole + 3;
constexpr int ChapterItemKind = 1;

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

}

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
    auto *editorToolbar = new QHBoxLayout;
    auto *backButton = new QPushButton(QStringLiteral("‹ Library"), editorPage_);
    backButton->setAccessibleName(QStringLiteral("Return to Library"));
    connect(backButton, &QPushButton::clicked, this, [this] {
        if (savePendingEdits()) {
            pages_->setCurrentWidget(tree_);
            statusBar()->showMessage(QStringLiteral("Library open: %1").arg(activeLibraryPath_));
        }
    });
    editorToolbar->addWidget(backButton);
    editorTitle_ = new QLabel(editorPage_);
    editorTitle_->setObjectName(QStringLiteral("chapter-title"));
    editorTitle_->setAccessibleName(QStringLiteral("Current chapter"));
    editorToolbar->addWidget(editorTitle_, 1);
    saveButton_ = new QPushButton(QStringLiteral("Save"), editorPage_);
    saveButton_->setObjectName(QStringLiteral("chapter-save"));
    connect(saveButton_, &QPushButton::clicked, this, [this] { saveCurrentChapter(); });
    editorToolbar->addWidget(saveButton_);
    editorLayout->addLayout(editorToolbar);

    editorState_ = new QLabel(editorPage_);
    editorState_->setObjectName(QStringLiteral("chapter-save-state"));
    editorState_->setWordWrap(true);
    editorLayout->addWidget(editorState_);

    chapterEditor_ = new QPlainTextEdit(editorPage_);
    chapterEditor_->setObjectName(QStringLiteral("chapter-editor"));
    chapterEditor_->setAccessibleName(QStringLiteral("Chapter text or read-only source"));
    editorLayout->addWidget(chapterEditor_, 1);
    saveTimer_ = new QTimer(this);
    saveTimer_->setSingleShot(true);
    saveTimer_->setInterval(800);
    connect(saveTimer_, &QTimer::timeout, this, [this] { saveCurrentChapter(); });
    connect(chapterEditor_, &QPlainTextEdit::textChanged, this, [this] {
        if (loadingChapter_ || chapterReadOnly_) {
            return;
        }
        chapterDirty_ = true;
        saveFailed_ = false;
        updateEditorState(QStringLiteral("Unsaved changes. LEO will save shortly."));
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

    const PersistenceResult recovery = LibraryPersistence::recoverLibrary(path);
    if (!recovery.ok) {
        tree_->clear();
        activeLibraryPath_.clear();
        refusal_->setText(QStringLiteral(
            "LEO paused Library opening during save recovery.\n\n%1\n\n"
            "The current Library file was left untouched.")
                              .arg(recovery.error));
        pages_->setCurrentWidget(refusalPage_);
        statusBar()->showMessage(QStringLiteral("Library recovery needs attention"));
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
    chapterDirty_ = false;
    saveFailed_ = false;
    chapterReadOnly_ = true;
    chapterHasUtf8Bom_ = false;

    QByteArray source;
    QString readError;
    loadingChapter_ = true;
    if (!LibraryPersistence::readLibraryFile(activeLibraryPath_, activeChapterRelativePath_,
                                             &source, &readError)) {
        chapterEditor_->clear();
        editorState_->setText(QStringLiteral("Read-only: %1").arg(readError));
        chapterEditor_->setReadOnly(true);
        saveButton_->setEnabled(false);
        loadingChapter_ = false;
        pages_->setCurrentWidget(editorPage_);
        statusBar()->showMessage(QStringLiteral("Chapter could not be opened safely"));
        return true;
    }

    const LegacyChapterDocument document = LegacyChapterCodec::decode(source);
    sourceHash_ = LibraryPersistence::hash(source);
    chapterHasUtf8Bom_ = document.hasUtf8Bom;
    chapterReadOnly_ = !document.editable();
    chapterEditor_->setReadOnly(chapterReadOnly_);
    chapterEditor_->setPlainText(chapterReadOnly_ ? QString::fromUtf8(source) : document.text);
    chapterEditor_->moveCursor(QTextCursor::Start);
    loadingChapter_ = false;

    if (chapterReadOnly_) {
        editorState_->setText(QStringLiteral("Read-only: %1 The original chapter stays unchanged.")
                                  .arg(document.refusalReason));
    } else {
        editorState_->setText(QStringLiteral(
            "Plain prose is editable. Other markup opens as read-only source text."));
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
        chapterEditor_->toPlainText(), chapterHasUtf8Bom_, &encodeError);
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
    chapterDirty_ = false;
    saveFailed_ = false;
    updateEditorState(QStringLiteral("Saved. Library remains compatible with NEO."));
    statusBar()->showMessage(QStringLiteral("Chapter saved safely"));
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

void LibraryWindow::updateEditorState(const QString &message)
{
    if (!message.isEmpty()) {
        editorState_->setText(chapterDirty_ && saveFailed_
                                  ? QStringLiteral("Unsaved changes — %1 Retry with Save.").arg(message)
                                  : message);
    }
    saveButton_->setEnabled(chapterDirty_ && !chapterReadOnly_);
    saveButton_->setText(saveFailed_ ? QStringLiteral("Retry Save") : QStringLiteral("Save"));
    setWindowTitle(chapterDirty_ ? QStringLiteral("LEO — unsaved chapter") : QStringLiteral("LEO"));
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
