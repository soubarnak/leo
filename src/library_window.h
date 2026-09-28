#pragma once

#include <QByteArray>
#include <QMainWindow>

#include "legacy_chapter_codec.h"

class QLabel;
class QPlainTextEdit;
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
    void chooseLibrary();
    bool openChapter(QTreeWidgetItem *item);
    bool saveCurrentChapter();
    bool savePendingEdits();
    void saveRepairCopy();
    void switchToRecoveredLibrary();
    void updateEditorState(const QString &message = QString());
    void closeEvent(QCloseEvent *event) override;

    QStackedWidget *pages_ = nullptr;
    QLabel *recoveryNotice_ = nullptr;
    QPushButton *openRecoveredLibraryButton_ = nullptr;
    QPushButton *openRecoveredFromRefusalButton_ = nullptr;
    QTreeWidget *tree_ = nullptr;
    QWidget *editorPage_ = nullptr;
    QLabel *editorTitle_ = nullptr;
    QLabel *editorState_ = nullptr;
    QPlainTextEdit *chapterEditor_ = nullptr;
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
    QByteArray sourceHash_;
    QByteArray sourceBytes_;
    LegacyChapterDocument chapterDocument_;
    LegacyChapterLinkContext chapterLinks_;
    QString lastValidEditorText_;
    bool chapterDirty_ = false;
    bool chapterConflict_ = false;
    bool chapterReadOnly_ = true;
    bool sourceAvailable_ = false;
    bool loadingChapter_ = false;
    bool saveFailed_ = false;
};
