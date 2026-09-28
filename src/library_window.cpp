#include "library_window.h"

#include "app_paths.h"
#include "library_reader.h"
#include "release_check_dialog.h"

#include <QAction>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace {

void addBook(QTreeWidgetItem *parent, const Book &book)
{
    auto *bookItem = new QTreeWidgetItem(parent, {book.title, book.author});
    bookItem->setToolTip(0, book.title);
    bookItem->setToolTip(1, book.author);

    for (int index = 0; index < book.chapters.size(); ++index) {
        const Chapter &chapter = book.chapters.at(index);
        QString label = QStringLiteral("Chapter %1").arg(index + 1);
        if (!chapter.title.isEmpty()) {
            label += QStringLiteral(" — ") + chapter.title;
        }
        auto *chapterItem = new QTreeWidgetItem(bookItem, {label});
        chapterItem->setToolTip(0, label);
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
    const LibraryReadResult result = LibraryReader::read(path);
    tree_->clear();
    if (!result.ok()) {
        qWarning().noquote() << "Library open refused:" << result.error;
        refusal_->setText(QStringLiteral(
            "LEO refused to open this Library.\n\n%1\n\nNo Library files were changed.")
                              .arg(result.error));
        pages_->setCurrentWidget(refusalPage_);
        statusBar()->showMessage(QStringLiteral("No Library open"));
        return false;
    }

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
    statusBar()->showMessage(QStringLiteral("Read-only Library: %1").arg(result.library.path));
    return true;
}

void LibraryWindow::chooseLibrary()
{
    const QString path = selectLibraryDirectory(this, defaultPath_);
    if (!path.isEmpty()) {
        openLibrary(path);
    }
}
