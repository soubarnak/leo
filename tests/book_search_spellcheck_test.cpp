#include "book_search.h"
#include "spellcheck.h"
#include "library_persistence.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>

namespace {
void write(const QString &path, const QByteArray &bytes)
{
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(bytes), bytes.size());
}
QByteArray read(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}
void fixture(const QString &root, const QByteArray &first, const QByteArray &second)
{
    write(QDir(root).filePath(QStringLiteral("library.json")),
          QByteArrayLiteral(R"({"authors":[],"shelves":[]})"));
    write(QDir(root).filePath(QStringLiteral("book/book.json")),
          QByteArrayLiteral(R"({"id":"book","chapterOrder":["a","b"]})"));
    write(QDir(root).filePath(QStringLiteral("book/chapters/a.html")), first);
    write(QDir(root).filePath(QStringLiteral("book/chapters/b.html")), second);
}
}

class BookSearchSpellcheckTest : public QObject {
    Q_OBJECT
private slots:
    void replaceAcrossChaptersAndUndoAsOneBatch()
    {
        QTemporaryDir root, data, state;
        QVERIFY(root.isValid() && data.isValid() && state.isValid());
        qputenv("XDG_DATA_HOME", data.path().toUtf8());
        qputenv("XDG_STATE_HOME", state.path().toUtf8());
        const QByteArray first = "<p>Blue <b>bird</b>.</p>";
        const QByteArray second = "<p>Blue sea.</p>";
        fixture(root.path(), first, second);
        BookSearch search(root.path(), QStringLiteral("book"));
        const auto found = search.find(QStringLiteral("blue"));
        QVERIFY2(found.ok, qPrintable(found.error));
        QCOMPARE(found.hits.size(), 2);
        const auto changed = search.replaceAll(QStringLiteral("blue"), QStringLiteral("Green"));
        QVERIFY2(changed.ok, qPrintable(changed.error));
        QCOMPARE(changed.changed, 2);
        QVERIFY(read(QDir(root.path()).filePath(QStringLiteral("book/chapters/a.html")))
                    .contains("Green <b>bird</b>"));
        QVERIFY(read(QDir(root.path()).filePath(QStringLiteral("book/chapters/b.html")))
                    .contains("Green sea"));
        QVERIFY(search.canUndo());
        const auto secondBatch = search.replaceAll(QStringLiteral("Green"), QStringLiteral("Gold"));
        QVERIFY2(secondBatch.ok, qPrintable(secondBatch.error));
        QCOMPARE(secondBatch.changed, 2);
        QVERIFY(search.undo().ok);
        QVERIFY(read(QDir(root.path()).filePath(QStringLiteral("book/chapters/a.html")))
                    .contains("Green <b>bird</b>"));
        QVERIFY(search.canUndo());
        const auto undone = search.undo();
        QVERIFY2(undone.ok, qPrintable(undone.error));
        QCOMPARE(read(QDir(root.path()).filePath(QStringLiteral("book/chapters/a.html"))), first);
        QCOMPARE(read(QDir(root.path()).filePath(QStringLiteral("book/chapters/b.html"))), second);
    }

    void semanticMatchBlocksWholeBatch()
    {
        QTemporaryDir root, data, state;
        qputenv("XDG_DATA_HOME", data.path().toUtf8());
        qputenv("XDG_STATE_HOME", state.path().toUtf8());
        const QByteArray first = "<p>Blue sky.</p>";
        const QByteArray second = "<p>Plain.</p><div data-future=\"Blue\">Blue.</div>";
        fixture(root.path(), first, second);
        BookSearch search(root.path(), QStringLiteral("book"));
        const auto found = search.find(QStringLiteral("Blue"));
        QVERIFY2(found.ok, qPrintable(found.error));
        QCOMPARE(std::count_if(found.hits.begin(), found.hits.end(),
                               [](const BookSearchHit &hit) { return hit.blocked; }), 2);
        const auto result = search.replaceAll(QStringLiteral("Blue"), QStringLiteral("Red"));
        QVERIFY(!result.ok);
        QVERIFY(result.error.contains(QStringLiteral("protected")));
        QCOMPARE(read(QDir(root.path()).filePath(QStringLiteral("book/chapters/a.html"))), first);
        QCOMPARE(read(QDir(root.path()).filePath(QStringLiteral("book/chapters/b.html"))), second);
    }

    void replacesSafeProseBesideProtectedContent()
    {
        QTemporaryDir root, data, state;
        qputenv("XDG_DATA_HOME", data.path().toUtf8());
        qputenv("XDG_STATE_HOME", state.path().toUtf8());
        const QByteArray first = "<p>Blue sky.</p><div data-future=\"keep\">opaque</div>";
        fixture(root.path(), first, "<p>Blue sea.</p>");
        BookSearch search(root.path(), QStringLiteral("book"));
        const auto placeholder = search.find(QStringLiteral("Protected legacy content"));
        QVERIFY2(placeholder.ok, qPrintable(placeholder.error));
        QVERIFY(placeholder.hits.isEmpty());
        const auto result = search.replaceAll(QStringLiteral("Blue"), QStringLiteral("Green"));
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(result.changed, 2);
        const QByteArray saved = read(QDir(root.path()).filePath(QStringLiteral("book/chapters/a.html")));
        QVERIFY(saved.contains("Green sky"));
        QVERIFY(saved.contains("<div data-future=\"keep\">opaque</div>"));
    }

    void learnedWordPersistsAfterReopen()
    {
        QTemporaryDir root, data, state;
        qputenv("XDG_DATA_HOME", data.path().toUtf8());
        qputenv("XDG_STATE_HOME", state.path().toUtf8());
        fixture(root.path(), "<p>Text.</p>", "<p>Other.</p>");
        {
            Spellcheck spell(root.path());
            QVERIFY2(spell.ready(), qPrintable(spell.error()));
            QVERIFY(!spell.check(QStringLiteral("Zorbwizzle" )).isEmpty());
            QString error;
            QVERIFY2(spell.learn(QStringLiteral("Zorbwizzle"), &error), qPrintable(error));
            QVERIFY(spell.check(QStringLiteral("Zorbwizzle")).isEmpty());
        }
        Spellcheck reopened(root.path());
        QVERIFY2(reopened.ready(), qPrintable(reopened.error()));
        QVERIFY(reopened.check(QStringLiteral("Zorbwizzle")).isEmpty());
        QVERIFY(!reopened.suggestions(QStringLiteral("speling")).isEmpty());
    }
};

QTEST_MAIN(BookSearchSpellcheckTest)
#include "book_search_spellcheck_test.moc"
