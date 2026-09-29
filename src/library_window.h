#pragma once

#include <QMainWindow>
#include <QByteArray>
#include <QAbstractItemView>
#include <QPoint>

#include <memory>

#include "legacy_chapter_codec.h"
#include "library_organization.h"
#include "library_reader.h"

class QLabel;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QSyntaxHighlighter;
class QPushButton;
class QStackedWidget;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
class QWidget;
class QCloseEvent;
class HoverFadeFilter;

class LibraryWindow final : public QMainWindow {
public:
    explicit LibraryWindow(QWidget *parent = nullptr);

    static QString defaultLibraryPath();
    static QString selectLibraryDirectory(QWidget *parent, const QString &startingPath);
    bool openLibrary(const QString &path);

private:
    void beginNewLibrary();
    void createNewLibrary();
    void chooseLibrary();
    void createBookFromSelection();
    void manageAuthor(const QString &authorId, const QPoint &globalPosition);
    void showOrganizationContextMenu(const QPoint &position);
    void handleLibraryDrop(QTreeWidgetItem *source, QTreeWidgetItem *target,
                           QAbstractItemView::DropIndicatorPosition position);
    void finishOrganizationChange(const LibraryOrganizationResult &result,
                                  const QString &successMessage);
    bool refreshOrganizationView();
    void populateLibraryTree(const Library &library);
    bool openChapter(QTreeWidgetItem *item);
    bool openOutline(QTreeWidgetItem *item);
    bool openDocument(const QString &relativePath,
                      const QString &title,
                      const LegacyChapterLinkContext &links,
                      bool outline);
    void applyPreferences(const LibraryPreferences &preferences);
    bool saveCurrentChapter();
    bool savePendingEdits();
    void saveRepairCopy();
    void switchToRecoveredLibrary();
    void updateEditorState(const QString &message = QString());
    void closeEvent(QCloseEvent *event) override;

    QStackedWidget *pages_ = nullptr;
    QWidget *welcomePage_ = nullptr;
    QWidget *onboardingPage_ = nullptr;
    QWidget *onboardingReturnPage_ = nullptr;
    QLabel *onboardingError_ = nullptr;
    QLineEdit *onboardingAuthor_ = nullptr;
    QLineEdit *onboardingLocation_ = nullptr;
    QComboBox *onboardingMode_ = nullptr;
    QComboBox *onboardingBodyFont_ = nullptr;
    QComboBox *onboardingDropCap_ = nullptr;
    QLabel *recoveryNotice_ = nullptr;
    QPushButton *openRecoveredLibraryButton_ = nullptr;
    QPushButton *openRecoveredFromRefusalButton_ = nullptr;
    QWidget *libraryPage_ = nullptr;
    QTreeWidget *tree_ = nullptr;
    std::unique_ptr<LibraryOrganization> organization_;
    QWidget *editorPage_ = nullptr;
    QLabel *editorTitle_ = nullptr;
    QLabel *editorState_ = nullptr;
    QPlainTextEdit *chapterEditor_ = nullptr;
    QSyntaxHighlighter *dropCapHighlighter_ = nullptr;
    QPushButton *saveButton_ = nullptr;
    QPushButton *repairCopyButton_ = nullptr;
    QWidget *editorChrome_ = nullptr;
    HoverFadeFilter *chromeHoverFilter_ = nullptr;
    QTimer *saveTimer_ = nullptr;
    QWidget *refusalPage_ = nullptr;
    QLabel *refusal_ = nullptr;
    QString defaultPath_;
    QString activeLibraryPath_;
    QString recoveredLibraryPath_;
    QString conflictDraftPath_;
    QString activeChapterRelativePath_;
    LibraryPreferences activePreferences_;
    QString preferenceNotice_;
    QByteArray sourceHash_;
    QByteArray sourceBytes_;
    LegacyChapterDocument chapterDocument_;
    LegacyChapterLinkContext chapterLinks_;
    QString lastValidEditorText_;
    bool chapterDirty_ = false;
    bool chapterConflict_ = false;
    bool chapterReadOnly_ = true;
    bool activeDocumentIsOutline_ = false;
    bool sourceAvailable_ = false;
    bool loadingChapter_ = false;
    bool saveFailed_ = false;
};
