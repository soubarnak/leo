#pragma once

#include <QMainWindow>

class QLabel;
class QStackedWidget;
class QTreeWidget;

class LibraryWindow final : public QMainWindow {
public:
    explicit LibraryWindow(QWidget *parent = nullptr);

    static QString defaultLibraryPath();
    bool openLibrary(const QString &path);

private:
    void chooseLibrary();

    QStackedWidget *pages_ = nullptr;
    QTreeWidget *tree_ = nullptr;
    QWidget *refusalPage_ = nullptr;
    QLabel *refusal_ = nullptr;
    QString defaultPath_;
};
