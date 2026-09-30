#include "manuscript_import.h"
#include "legacy_chapter_codec.h"
#include "library_creator.h"
#include "library_organization.h"
#include "library_reader.h"
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QtTest>

class ManuscriptImportTest : public QObject {
    Q_OBJECT
private slots:
    void importedBookReopens() {
        QTemporaryDir dir;
        const QString root = dir.filePath("Library");
        auto created = LibraryCreator::create({root, "Ada", "pantser", {}, "literary"});
        QVERIFY2(created.ok, qPrintable(created.error));
        LibraryOrganization organization(root);
        QString error;
        QVERIFY2(organization.load(&error), qPrintable(error));
        const QString shelf = organization.metadata().value("shelves").toArray().first().toObject().value("id").toString();
        QFile source(dir.filePath("story.txt"));
        QVERIFY(source.open(QIODevice::WriteOnly));
        source.write("Chapter 1\nHello.\nChapter 2\nWorld."); source.close();
        const auto preview = ManuscriptImport::read(source.fileName());
        const auto saved = organization.importBook(shelf, preview);
        QVERIFY2(saved.ok, qPrintable(saved.error));
        const auto reopened = LibraryReader::read(root);
        QVERIFY2(reopened.ok(), qPrintable(reopened.error));
        const auto books = reopened.library.authors.first().shelves.first().books;
        bool found = false;
        for (const auto &book : books) if (book.id == saved.id) {
            found = true; QCOMPARE(book.title, QStringLiteral("story")); QCOMPARE(book.chapters.size(), 2);
        }
        QVERIFY(found);
        const QString node = QStandardPaths::findExecutable(QStringLiteral("node"));
        if (!node.isEmpty()) {
            QProcess neo;
            neo.start(node, {QStringLiteral(IMPORT_NEO_SCRIPT), QStringLiteral(IMPORT_NEO_MAIN), root, saved.id});
            QVERIFY(neo.waitForFinished(10000));
            QVERIFY2(neo.exitStatus() == QProcess::NormalExit && neo.exitCode() == 0,
                     neo.readAllStandardError().constData());
        }
    }

    void docxCarriageReturnPreservesLineBreak() {
        const auto preview = ManuscriptImport::read(QStringLiteral(IMPORT_FIXTURES) + "/carriage.docx");
        QVERIFY2(preview.ok(), qPrintable(preview.error));
        QCOMPARE(LegacyChapterCodec::decode(preview.chapters.first().html).text.trimmed(), QStringLiteral("one\ntwo"));
    }
    void inlinePageBreakPreservesTextOnBothSides() {
        const auto preview = ManuscriptImport::read(QStringLiteral(IMPORT_FIXTURES) + "/inline-break.docx");
        QVERIFY2(preview.ok(), qPrintable(preview.error));
        QCOMPARE(preview.chapters.size(), 2);
        QCOMPARE(LegacyChapterCodec::decode(preview.chapters.at(0).html).text.trimmed(), QStringLiteral("Before."));
        QCOMPARE(LegacyChapterCodec::decode(preview.chapters.at(1).html).text.trimmed(), QStringLiteral("After."));
    }
    void docxPreservesFormatting_data() {
        QTest::addColumn<QString>("fixture");
        QTest::newRow("deflated") << QStringLiteral("supported.docx");
        QTest::newRow("stored") << QStringLiteral("stored.docx");
    }
    void docxPreservesFormatting() {
        QFETCH(QString, fixture);
        const auto preview = ManuscriptImport::read(QStringLiteral(IMPORT_FIXTURES) + "/" + fixture);
        QVERIFY2(preview.ok(), qPrintable(preview.error));
        QCOMPARE(preview.title, QStringLiteral("গল্প"));
        QCOMPARE(preview.chapters.size(), 2);
        QCOMPARE(preview.sceneBreaks, 1);
        QVERIFY(preview.chapters.first().html.contains(QStringLiteral("<b>Café &amp; গল্প</b>").toUtf8()));
        QVERIFY(preview.chapters.first().html.contains("<i> italic</i>"));
        QVERIFY(!preview.warnings.isEmpty());
    }
    void refusedSourcesLeaveLibraryAndSourceUnchanged() {
        QTemporaryDir dir;
        const QString root = dir.filePath("Library");
        QVERIFY(LibraryCreator::create({root, "Ada", "pantser", {}, "literary"}).ok);
        LibraryOrganization organization(root);
        QString error;
        QVERIFY(organization.load(&error));
        const auto before = organization.metadata();
        const QString shelf = before.value("shelves").toArray().first().toObject().value("id").toString();
        for (const QString &name : {QStringLiteral("unsupported.docx"), QStringLiteral("malformed.docx"), QStringLiteral("alternate.docx"), QStringLiteral("field.docx")}) {
            const QString path = QStringLiteral(IMPORT_FIXTURES) + "/" + name;
            QFile source(path); QVERIFY(source.open(QIODevice::ReadOnly));
            const auto bytes = source.readAll(); source.close();
            const auto preview = ManuscriptImport::read(path);
            QVERIFY(!preview.ok());
            QVERIFY(!organization.importBook(shelf, preview).ok);
            QCOMPARE(organization.metadata(), before);
            QVERIFY(source.open(QIODevice::ReadOnly)); QCOMPARE(source.readAll(), bytes);
        }
        QFile source(dir.filePath("unsafe.md")); QVERIFY(source.open(QIODevice::WriteOnly));
        source.write("<p data-sticky-id=\"foreign\">Words</p>"); source.close();
        QVERIFY(!ManuscriptImport::read(source.fileName()).ok());
        source.setFileName(dir.filePath("invalid.txt")); QVERIFY(source.open(QIODevice::WriteOnly));
        source.write(QByteArray::fromHex("fffe")); source.close();
        QVERIFY(!ManuscriptImport::read(source.fileName()).ok());
        ManuscriptPreview unsafe;
        unsafe.title = "Unsafe"; unsafe.chapters.append(ImportedChapter{"Chapter 1", "<p data-darling-id=\"foreign\">Text</p>"});
        QVERIFY(!organization.importBook(shelf, unsafe).ok);
        QCOMPARE(organization.metadata(), before);
        QVERIFY(LibraryReader::read(root).ok());
    }
    void markdownPreservesUnicodeAndFormatting() {
        QTemporaryDir dir;
        QFile file(dir.filePath("story.md"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QStringLiteral("# গল্প\n\n## Chapter 1\n\nHello **bold** and *italic* café.\n\n***\n\nশেষ\n\n## Chapter 2\n\nNext.").toUtf8());
        file.close();
        const auto result = ManuscriptImport::read(file.fileName());
        QVERIFY2(result.ok(), qPrintable(result.error));
        QCOMPARE(result.title, QStringLiteral("গল্প"));
        QCOMPARE(result.chapters.size(), 2);
        QCOMPARE(result.sceneBreaks, 1);
        const auto decoded = LegacyChapterCodec::decode(result.chapters.first().html);
        QVERIFY(decoded.editable());
        QVERIFY(decoded.text.contains(QStringLiteral("café")));
        QVERIFY(result.chapters.first().html.contains("<b>bold</b>"));
    }
};
QTEST_MAIN(ManuscriptImportTest)
#include "manuscript_import_test.moc"
