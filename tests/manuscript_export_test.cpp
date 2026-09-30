#include "manuscript_export.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QTextCursor>
#include <QTextBlock>
#include <QTextFragment>
#include <QRegularExpression>
#include "book_covers.h"
#include <QtTest>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>
class ExportTest : public QObject {
    Q_OBJECT
private slots:
    void orderedProse_data();
    void orderedProse();
    void draftingAndFormats_data();
    void draftingAndFormats();
    void failurePreservesDestination();
    void importedCoverAndAtomicReplacement();
    void inlineBreaks();
    void docxBook();
    void docxShelf();
    void independentDocxReader();
};

void ExportTest::orderedProse_data() {
    QTest::addColumn<int>("format");
    QTest::newRow("TXT") << int(ManuscriptFormat::Text);
    QTest::newRow("Markdown") << int(ManuscriptFormat::Markdown);
    QTest::newRow("HTML") << int(ManuscriptFormat::Html);
}
void ExportTest::orderedProse() {
        QFETCH(int, format);
        QTemporaryDir dir; QTemporaryDir output;
        auto put = [&](const QString &name, const QByteArray &bytes) {
            QDir().mkpath(QFileInfo(dir.filePath(name)).absolutePath());
            QFile f(dir.filePath(name)); QVERIFY(f.open(QIODevice::WriteOnly)); QCOMPARE(f.write(bytes), bytes.size());
        };
        put("b/book.json", R"({"title":"Story","author":"Ada","chapterOrder":["two","one"],"chapterTitles":{"two":"Second","one":"First"}})");
        put("b/chapters/two.html", "<p>Arrival <b>bold</b> <i>soft</i>.</p><p class=\"scene-break\">***</p>");
        put("b/chapters/one.html", "<p style=\"text-align:right\">Departure.</p>");
        const auto result = ManuscriptExport::write(dir.path(), "b", ManuscriptFormat(format), output.filePath("out.html"));
        QVERIFY2(result.ok, qPrintable(result.error));
        QFile f(output.filePath("out.html")); QVERIFY(f.open(QIODevice::ReadOnly));
        QTextDocument reader; const auto source = QString::fromUtf8(f.readAll());
        if (ManuscriptFormat(format) == ManuscriptFormat::Html) reader.setHtml(source);
        else if (ManuscriptFormat(format) == ManuscriptFormat::Markdown) reader.setMarkdown(source);
        else reader.setPlainText(source);
        const QString text = reader.toPlainText();
        QVERIFY(text.indexOf("Arrival") < text.indexOf("Departure"));
        if (ManuscriptFormat(format) != ManuscriptFormat::Markdown) QVERIFY(text.contains("***"));
        QVERIFY(text.contains("Chapter 1 — Second"));
        if (ManuscriptFormat(format) == ManuscriptFormat::Text) return;
        QTextCursor cursor(&reader); cursor = reader.find("bold");
        QVERIFY(cursor.charFormat().fontWeight() >= QFont::Bold);
        cursor = reader.find("soft"); QVERIFY(cursor.charFormat().fontItalic());
        if (ManuscriptFormat(format) == ManuscriptFormat::Markdown) return;
        cursor = reader.find("Departure"); QVERIFY(cursor.blockFormat().alignment() & Qt::AlignRight);
        QVERIFY(BookCovers::exportCover(dir.path(), "b").size().isValid());
        QVERIFY(f.seek(0)); QVERIFY(f.readAll().contains("data:image/png;base64,"));
    }
void ExportTest::draftingAndFormats_data() {
        QTest::addColumn<int>("format");
        QTest::newRow("TXT") << int(ManuscriptFormat::Text);
        QTest::newRow("Markdown") << int(ManuscriptFormat::Markdown);
        QTest::newRow("HTML") << int(ManuscriptFormat::Html);
    }
void ExportTest::draftingAndFormats() {
        QFETCH(int, format);
        QTemporaryDir dir, output;
        QDir().mkpath(dir.filePath("b/chapters"));
        auto put = [&](const QString &name, const QByteArray &bytes) {
            QFile file(dir.filePath(name)); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(bytes);
        };
        put("b/book.json", R"({"title":"Title #1","author":"Ada","chapterOrder":["a"]})");
        put("b/chapters/a.html", R"(<p class="scene-break" data-sec-brk="g">***</p><p class="ghost" data-sec-id="g">Secret outline</p><p data-sec-id="written">Real <span class="ph-mark">private</span><b>bold</b> <i>soft</i> &amp; prose.</p><p class="scene-break" data-sec-brk="written">***</p><p>Literal *star* [link] #heading.</p>)");
        const QString target = output.filePath("out");
        auto result = ManuscriptExport::write(dir.path(), "b", ManuscriptFormat(format), target);
        QVERIFY2(result.ok, qPrintable(result.error));
        QFile file(target); QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll(); const QString source = QString::fromUtf8(bytes);
        QVERIFY(!source.contains("Secret")); QVERIFY(!source.contains("private"));
        QVERIFY(!source.contains("data-sec")); QVERIFY(!source.contains("ghost"));
        QTextDocument reader;
        if (ManuscriptFormat(format) == ManuscriptFormat::Html) reader.setHtml(source);
        else if (ManuscriptFormat(format) == ManuscriptFormat::Markdown) reader.setMarkdown(source);
        else reader.setPlainText(source);
        const QString text = reader.toPlainText();
        QVERIFY2(text.contains("Real bold soft & prose."), qPrintable(text));
        QVERIFY(text.contains("Literal *star* [link] #heading."));
        QVERIFY(text.contains("Title #1"));
        if (ManuscriptFormat(format) != ManuscriptFormat::Text) {
            const auto cursor = reader.find("bold"); QVERIFY(cursor.charFormat().fontWeight() >= QFont::Bold);
            QVERIFY(reader.find("soft").charFormat().fontItalic());
        }
        if (ManuscriptFormat(format) == ManuscriptFormat::Markdown) {
            int breaks = 0;
            for (auto block = reader.begin(); block.isValid(); block = block.next())
                if (block.blockFormat().hasProperty(QTextFormat::BlockTrailingHorizontalRulerWidth)) ++breaks;
            QCOMPARE(breaks, 1);
        } else QCOMPARE(text.count("***"), 1);
        if (ManuscriptFormat(format) != ManuscriptFormat::Html) QVERIFY(!source.contains("data:image"));
    }
void ExportTest::failurePreservesDestination() {
        QTemporaryDir dir, output;
        QDir().mkpath(dir.filePath("b/chapters"));
        QFile book(dir.filePath("b/book.json")); QVERIFY(book.open(QIODevice::WriteOnly));
        book.write(R"({"title":"Title","chapterOrder":["a","missing"]})"); book.close();
        QFile chapter(dir.filePath("b/chapters/a.html")); QVERIFY(chapter.open(QIODevice::WriteOnly));
        chapter.write("<p>Valid prose</p>"); chapter.close();
        const QString target = output.filePath("out");
        QFile file(target); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("Original manuscript"); file.close();
        QVERIFY(!ManuscriptExport::write(dir.path(), "b", ManuscriptFormat::Text, target).ok);
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("Original manuscript")); file.close();
        QVERIFY(!ManuscriptExport::write(dir.path(), "b", ManuscriptFormat::Text, {}).ok);
        QVERIFY(!ManuscriptExport::write(dir.path(), "b", ManuscriptFormat::Text, book.fileName()).ok);
        QVERIFY(chapter.open(QIODevice::WriteOnly | QIODevice::Truncate)); chapter.write("<p>Broken"); chapter.close();
        QVERIFY(!ManuscriptExport::write(dir.path(), "b", ManuscriptFormat::Html, target).ok);
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("Original manuscript"));
    }

void ExportTest::importedCoverAndAtomicReplacement() {
    QTemporaryDir dir, output;
    QDir().mkpath(dir.filePath("b/chapters"));
    QFile book(dir.filePath("b/book.json")); QVERIFY(book.open(QIODevice::WriteOnly));
    book.write(R"({"title":"Title","chapterOrder":["a"],"coverImage":"cover.png","coverMode":"painted","coverArt":{"status":"done","file":"painted.png"}})"); book.close();
    QFile chapter(dir.filePath("b/chapters/a.html")); QVERIFY(chapter.open(QIODevice::WriteOnly));
    chapter.write("<p>Kept prose.</p>"); chapter.close();
    QImage imported(4, 6, QImage::Format_RGB32); imported.fill(Qt::red);
    QVERIFY(imported.save(dir.filePath("b/cover.png")));
    QImage painted(4, 6, QImage::Format_RGB32); painted.fill(Qt::blue);
    QVERIFY(painted.save(dir.filePath("b/painted.png")));
    const QString target = output.filePath("out.html");
    QFile file(target); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("Old export"); file.close();
    auto result = ManuscriptExport::write(dir.path(), "b", ManuscriptFormat::Html, target);
    QVERIFY2(result.ok, qPrintable(result.error));
    QVERIFY(file.open(QIODevice::ReadOnly)); const QByteArray html = file.readAll(); file.close();
    const auto match = QRegularExpression("data:image/png;base64,([^\"]+)").match(QString::fromUtf8(html));
    QVERIFY(match.hasMatch());
    QImage cover; QVERIFY(cover.loadFromData(QByteArray::fromBase64(match.captured(1).toLatin1()), "PNG"));
    QCOMPARE(cover.size(), imported.size()); QCOMPARE(cover.pixelColor(0, 0), QColor(Qt::red));
    QVERIFY(book.open(QIODevice::WriteOnly | QIODevice::Truncate));
    book.write(R"({"title":"Title","chapterOrder":["a"],"coverMode":"painted","coverArt":{"status":"done","file":"painted.png"}})"); book.close();
    QVERIFY(ManuscriptExport::write(dir.path(), "b", ManuscriptFormat::Html, target).ok);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto fallback = QRegularExpression("data:image/png;base64,([^\"]+)").match(QString::fromUtf8(file.readAll())); file.close();
    QVERIFY(fallback.hasMatch());
    QVERIFY(cover.loadFromData(QByteArray::fromBase64(fallback.captured(1).toLatin1()), "PNG"));
    QCOMPARE(cover.size(), QSize(1600, 2560));
    QVERIFY(cover.pixelColor(0, 0) != QColor(Qt::blue));
    QVERIFY(QDir().mkpath(output.filePath("directory")));
    QVERIFY(!ManuscriptExport::write(dir.path(), "b", ManuscriptFormat::Text, output.filePath("directory")).ok);
    QVERIFY(QFile::link(chapter.fileName(), output.filePath("linked")));
    QVERIFY(!ManuscriptExport::write(dir.path(), "b", ManuscriptFormat::Text, output.filePath("linked")).ok);
    QVERIFY(chapter.open(QIODevice::ReadOnly)); QCOMPARE(chapter.readAll(), QByteArray("<p>Kept prose.</p>"));
}

void ExportTest::inlineBreaks() {
    QTemporaryDir dir, output;
    QDir().mkpath(dir.filePath("b/chapters"));
    QFile book(dir.filePath("b/book.json")); QVERIFY(book.open(QIODevice::WriteOnly));
    book.write(R"({"title":"Title","chapterOrder":["a"]})"); book.close();
    QFile chapter(dir.filePath("b/chapters/a.html")); QVERIFY(chapter.open(QIODevice::WriteOnly));
    chapter.write("<p><b>First<br/>second</b></p>"); chapter.close();
    for (auto format : {ManuscriptFormat::Html, ManuscriptFormat::Markdown, ManuscriptFormat::Text}) {
        const QString target = output.filePath("out");
        QVERIFY(ManuscriptExport::write(dir.path(), "b", format, target).ok);
        QFile file(target); QVERIFY(file.open(QIODevice::ReadOnly)); const QString source = QString::fromUtf8(file.readAll());
        QTextDocument reader;
        if (format == ManuscriptFormat::Html) reader.setHtml(source);
        else if (format == ManuscriptFormat::Markdown) reader.setMarkdown(source);
        else reader.setPlainText(source);
        QVERIFY2(reader.toPlainText().contains("First\nsecond"), qPrintable(reader.toPlainText()));
    }
}

void ExportTest::docxBook() {
    QTemporaryDir dir, output;
    QDir().mkpath(dir.filePath("b/chapters"));
    QFile book(dir.filePath("b/book.json")); QVERIFY(book.open(QIODevice::WriteOnly));
    book.write(R"({"title":"Story","chapterOrder":["a"]})"); book.close();
    QFile chapter(dir.filePath("b/chapters/a.html")); QVERIFY(chapter.open(QIODevice::WriteOnly));
    chapter.write("<p>Real <b>bold</b></p>"); chapter.close();
    const auto result = ManuscriptExport::write(dir.path(), "b", ManuscriptFormat::Docx, output.filePath("book.docx"));
    QVERIFY2(result.ok, qPrintable(result.error));
    QFile file(output.filePath("book.docx")); QVERIFY(file.open(QIODevice::ReadOnly));
    const auto bytes = file.readAll();
    QVERIFY(bytes.startsWith("PK")); QVERIFY(bytes.contains("Real ")); QVERIFY(bytes.contains("<w:b/>"));
    QVERIFY(!bytes.contains("image/png")); file.close();
    QVERIFY(chapter.open(QIODevice::WriteOnly | QIODevice::Truncate)); chapter.write("<p>Broken"); chapter.close();
    QVERIFY(!ManuscriptExport::write(dir.path(), "b", ManuscriptFormat::Docx, file.fileName()).ok);
    QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), bytes);
    QVERIFY(!ManuscriptExport::write(dir.path(), "b", ManuscriptFormat::Docx, {}).ok);
}

void ExportTest::docxShelf() {
    QTemporaryDir dir, output;
    auto put = [&](const QString &name, const QByteArray &data) {
        QDir().mkpath(QFileInfo(dir.filePath(name)).absolutePath());
        QFile f(dir.filePath(name)); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(data);
    };
    put("library.json", R"({"shelves":[{"id":"s","name":"Collected","bookIds":["b","a","b"]}]})");
    for (const QString id : {QString("a"), QString("b")}) {
        put(id + "/book.json", ("{\"title\":\"" + id + "\",\"chapterOrder\":[\"one\"]}").toUtf8());
        put(id + "/chapters/one.html", ("<p>Prose " + id + "</p>").toUtf8());
    }
    const QString target = output.filePath("shelf.docx");
    const auto result = ManuscriptExport::writeShelf(dir.path(), "s", target);
    QVERIFY2(result.ok, qPrintable(result.error));
    QFile f(target); QVERIFY(f.open(QIODevice::ReadOnly)); const auto bytes = f.readAll(); f.close();
    QVERIFY(bytes.indexOf("Prose b") < bytes.indexOf("Prose a")); QCOMPARE(bytes.count("Prose b"), 1);
    QFile::remove(dir.filePath("a/chapters/one.html"));
    QVERIFY(!ManuscriptExport::writeShelf(dir.path(), "s", target).ok);
    QVERIFY(f.open(QIODevice::ReadOnly)); QCOMPARE(f.readAll(), bytes);
}

void ExportTest::independentDocxReader() {
    const QString executable = QStandardPaths::findExecutable("libreoffice");
    if (executable.isEmpty()) QSKIP("LibreOffice is required for independent DOCX reader checks.");
    QTemporaryDir dir, output;
    auto put = [&](const QString &name, const QByteArray &data) {
        QDir().mkpath(QFileInfo(dir.filePath(name)).absolutePath());
        QFile f(dir.filePath(name)); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(data);
    };
    put("b/book.json", R"({"title":"Story & more","author":"Ada","subtitle":"A tale","chapterOrder":["two","one"],"chapterTitles":{"two":"Arrival","one":"Departure"}})");
    put("b/chapters/two.html", R"(<p style="text-align:right">Real <b>bold</b> <i>soft</i> <b><i>both</i></b><br/>Next line.</p><p class="scene-break">***</p><p class="ghost" data-sec-id="g">Private outline</p><p><span class="ph-mark">Private placeholder</span></p>)");
    put("b/chapters/one.html", "<p style=\"text-align:justify\">Last prose.</p>");
    put("a/book.json", R"({"title":"Other story","chapterOrder":["one"]})");
    put("a/chapters/one.html", "<p>Other prose.</p>");
    put("library.json", R"({"shelves":[{"id":"s","name":"Collected","bookIds":["b","a","b"]}]})");
    for (bool shelf : {false, true}) {
        const QString name = shelf ? "shelf" : "book";
        const QString target = output.filePath(name + ".docx");
        const auto result = shelf ? ManuscriptExport::writeShelf(dir.path(), "s", target)
            : ManuscriptExport::write(dir.path(), "b", ManuscriptFormat::Docx, target);
        QVERIFY2(result.ok, qPrintable(result.error));
        QProcess process;
        process.start(executable, {"-env:UserInstallation=" + QUrl::fromLocalFile(output.filePath("profile")).toString(),
            "--headless", "--convert-to", "html", "--outdir", output.path(), target});
        QVERIFY(process.waitForFinished(30000)); QCOMPARE(process.exitCode(), 0);
        QFile converted(output.filePath(name + ".html")); QVERIFY2(converted.open(QIODevice::ReadOnly), process.readAllStandardError().constData());
        QString html = QString::fromUtf8(converted.readAll());
        QVERIFY(html.count("page-break-before: always") >= (shelf ? 5 : 2));
        QTextDocument reader; reader.setHtml(html.replace("text-align: end", "text-align: right").replace("text-align: start", "text-align: left"));
        const QString text = reader.toPlainText();
        QVERIFY(text.contains("Story & more")); QVERIFY(text.contains("A tale"));
        QVERIFY(text.contains("Chapter 1 — Arrival")); QVERIFY(text.contains("Chapter 2 — Departure"));
        QVERIFY(text.indexOf("Real") < text.indexOf("Last prose"));
        QCOMPARE(text.count("***"), 1); QVERIFY(!text.contains("Private"));
        QVERIFY(text.contains("Next line."));
        QVERIFY(reader.find("bold").charFormat().fontWeight() >= QFont::Bold);
        QVERIFY(reader.find("soft").charFormat().fontItalic());
        QVERIFY(reader.find("both").charFormat().fontItalic());
        QVERIFY(reader.find("both").charFormat().fontWeight() >= QFont::Bold);
        QVERIFY(reader.find("Real").blockFormat().alignment() & Qt::AlignRight);
        QVERIFY(reader.find("Last prose").blockFormat().alignment() & Qt::AlignJustify);
        for (auto block = reader.begin(); block.isValid(); block = block.next())
            for (auto it = block.begin(); !it.atEnd(); ++it) QVERIFY(!it.fragment().charFormat().isImageFormat());
        if (shelf) {
            QVERIFY(text.contains("Collected")); QCOMPARE(text.count("Other prose."), 1);
            QCOMPARE(text.count("Real"), 1); QVERIFY(text.indexOf("Last prose") < text.indexOf("Other prose"));
        }
    }
}

QTEST_MAIN(ExportTest)
#include "manuscript_export_test.moc"
