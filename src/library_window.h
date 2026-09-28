#pragma once

#include <QByteArray>
#include <QMainWindow>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
class QWidget;
class QCloseEvent;

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
    void updateEditorState(const QString &message = QString());
    void closeEvent(QCloseEvent *event) override;

    QStackedWidget *pages_ = nullptr;
    QTreeWidget *tree_ = nullptr;
    QWidget *editorPage_ = nullptr;
    QLabel *editorTitle_ = nullptr;
    QLabel *editorState_ = nullptr;
    QPlainTextEdit *chapterEditor_ = nullptr;
    QPushButton *saveButton_ = nullptr;
    QTimer *saveTimer_ = nullptr;
    QWidget *refusalPage_ = nullptr;
    QLabel *refusal_ = nullptr;
    QString defaultPath_;
    QString activeLibraryPath_;
    QString activeChapterRelativePath_;
    QByteArray sourceHash_;
    bool chapterDirty_ = false;
    bool chapterReadOnly_ = true;
    bool chapterHasUtf8Bom_ = false;
    bool loadingChapter_ = false;
    bool saveFailed_ = false;
};
