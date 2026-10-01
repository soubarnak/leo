#pragma once

#include <QMainWindow>
#include <QByteArray>
#include <QPoint>

#include <memory>

#include "chapter_structure.h"
#include "book_search.h"
#include "legacy_chapter_codec.h"
#include "library_organization.h"
#include "library_reader.h"
#include "planning_records.h"
#include "writing_progress.h"

class QLabel;
class QComboBox;
class QAction;
class QLineEdit;
class QPlainTextEdit;
class QSyntaxHighlighter;
class QPushButton;
class QStackedWidget;
class QScrollArea;
class QTimer;
class QVBoxLayout;
class QTreeWidget;
class QTreeWidgetItem;
class QWidget;
class QCloseEvent;
class QJsonValue;
class HoverFadeFilter;

enum class LibraryDropPosition {
    OnItem,
    AboveItem,
    BelowItem,
    OnViewport
};

class LibraryWindow final : public QMainWindow {
public:
    explicit LibraryWindow(QWidget *parent = nullptr);

    static QString defaultLibraryPath();
    static QString selectLibraryDirectory(QWidget *parent, const QString &startingPath);
    bool openLibrary(const QString &path);

private:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void changeEvent(QEvent *event) override;
    void beginNewLibrary();
    void createNewLibrary();
    void chooseLibrary();
    void createBookFromSelection();
    void manageAuthor(const QString &authorId, const QPoint &globalPosition);
    void showOrganizationContextMenu(const QPoint &position);
    void showChapterStructureMenu(QTreeWidgetItem *item, const QPoint &globalPosition);
    void handleLibraryDrop(QTreeWidgetItem *source, QTreeWidgetItem *target,
                           LibraryDropPosition position);
    void finishOrganizationChange(const LibraryOrganizationResult &result,
                                  const QString &successMessage);
    bool refreshOrganizationView();
    void populateLibraryTree(const Library &library);
    bool openChapter(QTreeWidgetItem *item, bool quietStatus = false);
    void navigateChapter(int direction);
    void updateChapterNavigation();
    void refreshBookPages();
    void resizeChapterEditorToContents();
    void clampToAvailableScreen();
    bool openOutline(QTreeWidgetItem *item);
    bool openDocument(const QString &relativePath,
                      const QString &title,
                      const LegacyChapterLinkContext &links,
                      bool outline,
                      bool quietStatus = false);
    bool ensureChapterStructure(const QString &bookId);
    void splitActiveChapter(const QString &text, int position,
                            bool notify = true);
    void applyChapterStructureResult(const ChapterStructureResult &result,
                                    const QString &bookId,
                                    const QString &successMessage,
                                    bool notify = true);
    bool refreshChapterStructureView(const QString &bookId, const QString &chapterId,
                                     bool notify);
    void updateChapterStructureActions();
    void undoChapterStructure();
    void redoChapterStructure();
    void showFindReplace();
    void undoReplacement();
    void cutSelectionToDarlings();
    void manageDarlings();
    void showSpellcheck();
    void addPlanningSticky();
    void addPlanningSection();
    void showPlanningOutline();
    void editChapterNote();
    void managePlanningRecords(bool sections);
    void undoPlanningChange();
    void applyPlanningResult(const PlanningResult &result);
    bool ensurePlanningRecords();
    void applyPreferences(const LibraryPreferences &preferences);
    void savePresentationPreference(const QString &key, const QJsonValue &value);
    void updatePresentation();
    bool saveCurrentChapter();
    bool savePendingEdits();
    void saveRepairCopy();
    void switchToRecoveredLibrary();
    void updateEditorState(const QString &message = QString());
    void showProgress();
    void refreshProgress();
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
    std::unique_ptr<ChapterStructure> chapterStructure_;
    std::unique_ptr<BookSearch> bookSearch_;
    std::unique_ptr<PlanningRecords> planningRecords_;
    QString planningBookId_;
    QString searchBookId_;
    QString structureBookId_;
    QAction *undoStructureAction_ = nullptr;
    QAction *redoStructureAction_ = nullptr;
    QWidget *editorPage_ = nullptr;
    QLabel *editorTitle_ = nullptr;
    QPushButton *previousChapterButton_ = nullptr;
    QPushButton *nextChapterButton_ = nullptr;
    QLabel *editorState_ = nullptr;
    QLabel *progressCount_ = nullptr;
    QLabel *progressGoal_ = nullptr;
    std::unique_ptr<WritingProgress> progress_;
    int sprintStart_ = 0;
    int sprintTarget_ = 0;
    bool sprintRunning_ = false;
    QString sprintBookId_;
    QPlainTextEdit *chapterEditor_ = nullptr;
    QScrollArea *bookFlow_ = nullptr;
    QWidget *bookPages_ = nullptr;
    QVBoxLayout *bookPagesLayout_ = nullptr;
    QSyntaxHighlighter *dropCapHighlighter_ = nullptr;
    QPushButton *saveButton_ = nullptr;
    QPushButton *repairCopyButton_ = nullptr;
    QWidget *editorChrome_ = nullptr;
    HoverFadeFilter *chromeHoverFilter_ = nullptr;
    QAction *paperAction_ = nullptr;
    QAction *nightAction_ = nullptr;
    QAction *brightAction_ = nullptr;
    QAction *pinControlsAction_ = nullptr;
    QAction *typewriterAction_ = nullptr;
    QTimer *saveTimer_ = nullptr;
    QWidget *refusalPage_ = nullptr;
    QLabel *refusal_ = nullptr;
    QString defaultPath_;
    QString activeLibraryPath_;
    QString recoveredLibraryPath_;
    QString conflictDraftPath_;
    QString activeChapterRelativePath_;
    QString activeBookId_;
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
