#include "library_window.h"

#include "library_reader.h"

#include <QAction>
#include <QDir>
#include <QFileDialog>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTreeWidget>
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
    connect(chooseAgain, &QPushButton::clicked, this, [this] { chooseLibrary(); });
    refusalLayout->addWidget(chooseAgain, 0, Qt::AlignHCenter);
    refusalLayout->addStretch();

    pages_->addWidget(tree_);
    pages_->addWidget(refusalPage_);
    setCentralWidget(pages_);
    statusBar()->showMessage(QStringLiteral("No Library open"));

    QMenu *fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    QAction *openAction = fileMenu->addAction(QStringLiteral("&Open Library…"));
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, [this] { chooseLibrary(); });
    fileMenu->addSeparator();
    QAction *quitAction = fileMenu->addAction(QStringLiteral("E&xit"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);
}

QString LibraryWindow::defaultLibraryPath()
{
    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString documentsPath = documents.isEmpty()
                                      ? QDir::home().filePath(QStringLiteral("Documents"))
                                      : documents;
    return QDir(documentsPath).filePath(QStringLiteral("NEO Library"));
}

bool LibraryWindow::openLibrary(const QString &path)
{
    const LibraryReadResult result = LibraryReader::read(path);
    tree_->clear();
    if (!result.ok()) {
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
    const QString path = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Open existing NEO Library"), defaultPath_,
        QFileDialog::ShowDirsOnly);
    if (!path.isEmpty()) {
        openLibrary(path);
    }
}
