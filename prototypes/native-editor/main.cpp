// THROWAWAY decision probe. Synthetic data only; never open a real library.
#include <QtWidgets>
#include <QPdfWriter>
#include <QTextBlock>
#include <QTextFragment>
#include <QXmlStreamReader>
#include <functional>
#include <stdexcept>

constexpr int Attrs = QTextFormat::UserProperty + 1;
using Object = QJsonObject;
QString esc(QString s) { return s.toHtmlEscaped(); }
QString attributes(const Object &a) {
    QString s;
    for (auto i = a.begin(); i != a.end(); ++i)
        s += " " + i.key() + "=\"" + esc(i.value().toString()) + "\"";
    return s;
}
Object attrs(const QTextFormat &f) { return f.property(Attrs).toJsonObject(); }
bool hasClass(Object a, QString name) { return a["class"].toString().split(' ').contains(name); }
QString encode(QTextDocument *doc, bool exporting = false) {
    QString html;
    QSet<QString> ghosts;
    for (auto b = doc->begin(); b.isValid(); b = b.next())
        if (hasClass(attrs(b.blockFormat()), "ghost")) ghosts.insert(attrs(b.blockFormat())["data-sec-id"].toString());
    for (auto b = doc->begin(); b.isValid(); b = b.next()) {
        auto a = attrs(b.blockFormat());
        if (exporting && (hasClass(a,"ghost") || (a.contains("data-sec-brk") && ghosts.contains(a["data-sec-brk"].toString())))) continue;
        Object exportAttrs;
        if (a.contains("style")) exportAttrs["style"] = a["style"];
        if (hasClass(a,"scene-break")) exportAttrs["style"] = "text-align:center";
        html += "<p" + attributes(exporting ? exportAttrs : a) + ">";
        bool content = false;
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            auto f = it.fragment(); if (!f.isValid()) continue;
            auto cf = f.charFormat(); auto fa = attrs(cf);
            if (exporting && (hasClass(fa,"ph-mark") || hasClass(fa,"darling-anchor"))) continue;
            QString t = esc(f.text()).replace(QChar::LineSeparator, "<br/>");
            if (cf.fontItalic()) t = "<i>" + t + "</i>";
            if (cf.fontWeight() >= QFont::Bold) t = "<b>" + t + "</b>";
            if (!exporting && !fa.isEmpty()) t = "<span" + attributes(fa) + ">" + t + "</span>";
            html += t; content = true;
        }
        if (!content) html += "<br/>";
        html += "</p>";
    }
    return html;
}
// Strict, deliberately bounded fixture codec. Reject rather than repair/drop unsupported HTML.
void decode(QTextDocument *doc, QString html) {
    html.replace(QRegularExpression("<br\\s*>", QRegularExpression::CaseInsensitiveOption), "<br/>");
    QXmlStreamReader xml("<root>" + html + "</root>");
    QTextDocument temp;
    QTextCursor c(&temp);
    QList<QTextCharFormat> stack;
    bool first = true, inP = false;
    while (!xml.atEnd()) {
        xml.readNext();
        QString tag = xml.name().toString();
        if (xml.isStartElement()) {
            Object a;
            for (auto at : xml.attributes()) a[at.name().toString()] = at.value().toString();
            if (tag == "root") continue;
            if (tag == "p") {
                if (inP) throw std::runtime_error("Nested paragraph refused");
                QTextBlockFormat bf; bf.setProperty(Attrs, a);
                auto style = a["style"].toString();
                if (!style.isEmpty() && !QRegularExpression("^\\s*text-align\\s*:\\s*(left|right|center|justify)\\s*;?\\s*$").match(style).hasMatch())
                    throw std::runtime_error("Unsupported paragraph style refused");
                if (style.contains("center")) bf.setAlignment(Qt::AlignHCenter);
                if (style.contains("right")) bf.setAlignment(Qt::AlignRight);
                if (style.contains("justify")) bf.setAlignment(Qt::AlignJustify);
                if (hasClass(a,"scene-break")) bf.setAlignment(Qt::AlignHCenter);
                if (first) { c.setBlockFormat(bf); first = false; }
                else c.insertBlock(bf, QTextCharFormat());
                c.setCharFormat(QTextCharFormat()); inP = true;
            } else if (!inP) throw std::runtime_error("Content outside paragraphs refused");
            else if (tag == "br") c.insertText(QString(QChar::LineSeparator));
            else if (tag == "b" || tag == "strong" || tag == "i" || tag == "em" || tag == "span") {
                auto f = c.charFormat(); stack.append(f);
                if (tag == "b" || tag == "strong") f.setFontWeight(QFont::Bold);
                if (tag == "i" || tag == "em") f.setFontItalic(true);
                if (tag == "span") {
                    if (a.contains("style") || !attrs(f).isEmpty()) throw std::runtime_error("Styled/nested spans refused");
                    f.setProperty(Attrs, a);
                    if (hasClass(a,"ph-mark")) f.setForeground(QColor("#ac652c"));
                } else if (!a.isEmpty()) throw std::runtime_error("Attributes on emphasis refused");
                c.setCharFormat(f);
            } else throw std::runtime_error(("Unsupported element: " + tag).toStdString());
        } else if (xml.isEndElement()) {
            if (tag == "p") {
                // Empty-paragraph <br> is a placeholder, not a hard line break.
                if (c.block().text() == QString(QChar::LineSeparator)) { c.deletePreviousChar(); }
                inP = false;
            } else if (tag == "b" || tag == "strong" || tag == "i" || tag == "em" || tag == "span") {
                c.setCharFormat(stack.takeLast());
            }
        } else if (xml.isCharacters()) {
            if (inP) c.insertText(xml.text().toString());
            else if (!xml.isWhitespace()) throw std::runtime_error("Loose text refused");
        }
    }
    if (xml.hasError()) throw std::runtime_error(xml.errorString().toStdString());
    doc->clear(); QTextCursor out(doc); out.insertFragment(QTextDocumentFragment(&temp));
}
QString fixture() {
    return QString::fromUtf8("<p data-extra=\"keep-me\">Before <b>bold</b> and <i>italic</i>; বাংলা; العربية; é; 👩🏽‍💻.</p>"
        "<p data-sec-id=\"section-written\">Written section <span class=\"ph-mark\" data-sid=\"sticky-one\" contenteditable=\"false\">⚑</span> after marker.</p>"
        "<p class=\"scene-break\">***</p><p style=\"text-align:center\">Centered prose.</p>"
        "<p class=\"scene-break\" data-sec-brk=\"section-ghost\">***</p>"
        "<p class=\"ghost\" data-sec-id=\"section-ghost\">Replace this outline prompt with prose.</p>");
}
struct Snapshot { QStringList chapters; Object meta; QJsonArray darlings, stickies; int chapter, pos, anchor; };
class Editor : public QTextEdit {
public:
    std::function<void(std::function<void()>)> transaction;
    std::function<void()> enter, undoAction, redoAction;
    bool composing = false;
    void keyPressEvent(QKeyEvent *e) override {
        if (composing) { QTextEdit::keyPressEvent(e); return; }
        if (e->matches(QKeySequence::Undo)) { undoAction(); return; }
        if (e->matches(QKeySequence::Redo)) { redoAction(); return; }
        if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && e->modifiers() == Qt::NoModifier && !textCursor().hasSelection()) { enter(); return; }
        transaction([&] {
            auto c = textCursor();
            bool editing = !e->text().isEmpty() || e->key() == Qt::Key_Backspace || e->key() == Qt::Key_Delete;
            if (editing) {
                auto a = attrs(c.blockFormat());
                if (hasClass(a,"ghost")) {
                    a.remove("class"); auto bf = c.blockFormat(); bf.setProperty(Attrs,a); c.setBlockFormat(bf);
                }
                if (!c.hasSelection()) { auto f = c.charFormat(); f.clearProperty(Attrs); c.setCharFormat(f); setTextCursor(c); }
            }
            QTextEdit::keyPressEvent(e);
        });
    }
    void inputMethodEvent(QInputMethodEvent *e) override {
        transaction([&] { QTextEdit::inputMethodEvent(e); });
        composing = !e->preeditString().isEmpty();
    }
    void insertFromMimeData(const QMimeData *m) override {
        transaction([&] { auto c = textCursor(); c.insertText(m->text(), QTextCharFormat()); setTextCursor(c); });
    }
};
class Window : public QMainWindow {
public:
    Editor *edit = new Editor;
    QPlainTextEdit *state = new QPlainTextEdit;
    QComboBox *chapters = new QComboBox;
    QStringList content;
    Object meta;
    QJsonArray darlings, stickies;
    QList<Snapshot> history, future;
    int current = 0, run = 0;
    bool busy = false;
    QString output;
    Window(QString out) : output(out) {
        setWindowTitle("Leo — THROWAWAY native editing probe (synthetic only)"); resize(1200,780);
        auto main = new QWidget; auto layout = new QVBoxLayout(main);
        auto notice = new QLabel("Synthetic scratch data only • Enter ×2: scene, ×3: chapter • Ctrl+Z / Ctrl+Shift+Z • Select prose, then Cut Darling\n"
            "Pending gates: real IME, legacy/Pocket reopen, drop caps, accessibility. This is not the replacement application.");
        notice->setWordWrap(true); notice->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Maximum); layout->addWidget(notice);
        auto row = new QHBoxLayout; layout->addLayout(row); row->addWidget(chapters);
        auto button = [&](QString name, std::function<void()> f) { auto b = new QPushButton(name); row->addWidget(b); connect(b,&QPushButton::clicked,this,[=]{ guarded(f); }); };
        button("Reset fixture",[&]{reset();}); button("Undo",[&]{undo();}); button("Redo",[&]{redo();});
        button("Cut Darling",[&]{cut();}); button("Restore Darling",[&]{restore();});
        button("Save / reopen",[&]{saveReopen();}); button("PDF",[&]{pdf();});
        auto splitter = new QSplitter; layout->addWidget(splitter,1); splitter->addWidget(edit); splitter->addWidget(state);
        splitter->setSizes({720,480}); state->setReadOnly(true); state->setAccessibleName("Serialized probe state");
        edit->setAccessibleName("Synthetic manuscript"); edit->setFont(QFont("serif",16)); edit->setUndoRedoEnabled(false);
        edit->setAcceptRichText(false); edit->setContextMenuPolicy(Qt::NoContextMenu);
        setCentralWidget(main);
        edit->transaction = [&](auto action){ tx(action); };
        edit->enter = [&]{ tx([&]{enter();}, true); };
        edit->undoAction = [&]{undo();}; edit->redoAction = [&]{redo();};
        connect(chapters,qOverload<int>(&QComboBox::currentIndexChanged),this,[&](int i){
            if (busy || i < 0) return; sync(); current=i; render(); run=0;
        });
        reset();
    }
    void guarded(std::function<void()> f) { try { f(); } catch (const std::exception &e) { QMessageBox::warning(this,"Probe refused operation",e.what()); } }
    void sync() { if (!content.isEmpty()) content[current]=encode(edit->document()); }
    Snapshot snap() { sync(); auto c=edit->textCursor(); return {content,meta,darlings,stickies,current,c.position(),c.anchor()}; }
    QByteArray data() { sync(); return QJsonDocument(Object{{"chapters",QJsonArray::fromStringList(content)},{"book",meta},{"darlings",darlings},{"stickies",stickies}}).toJson(); }
    void showState() { state->setPlainText(QString::fromUtf8(data())); statusBar()->showMessage(QString("%1 chapters · %2 Darlings · %3 undo events · %4").arg(content.size()).arg(darlings.size()).arg(history.size()).arg(output)); }
    void render() {
        busy=true; chapters->clear(); for(int i=0;i<content.size();++i) chapters->addItem(QString("Chapter %1").arg(i+1)); chapters->setCurrentIndex(current);
        decode(edit->document(),content[current]); busy=false; showState();
    }
    void reset() {
        content={fixture()}; current=0; run=0; history.clear(); future.clear(); darlings={};
        meta=Object{{"id","book-PROTOTYPE"},{"title","Synthetic native probe"},{"author","No real manuscript"},{"chapterOrder",QJsonArray{"ch-one"}},
            {"unknownMetadata",Object{{"nested",QJsonArray{1,"preserve",true}}}},
            {"sectionNotes",Object{{"ch-one",QJsonArray{Object{{"id","section-written"},{"text","Written"}},Object{{"id","section-ghost"},{"text","Replace this outline prompt with prose."}}}}}}};
        stickies=QJsonArray{Object{{"id","sticky-one"},{"chapterId","ch-one"},{"text","Synthetic sticky"},{"resolved",false}}};
        render();
    }
    void apply(const Snapshot &s) {
        content=s.chapters; meta=s.meta; darlings=s.darlings; stickies=s.stickies; current=s.chapter; render();
        auto c=edit->textCursor(); c.setPosition(qMin(s.anchor,edit->document()->characterCount()-1)); c.setPosition(qMin(s.pos,edit->document()->characterCount()-1),QTextCursor::KeepAnchor); edit->setTextCursor(c);
    }
    void tx(std::function<void()> f, bool isEnter=false) {
        if(busy) {f();return;}
        auto before=snap(); auto old=data(); busy=true; f(); busy=false;
        if(data()!=old) { if (!(isEnter && run==2 && !history.isEmpty())) history.append(before); future.clear(); }
        if(!isEnter) run=0;
        showState();
    }
    void undo() { if(history.isEmpty()) return; future.append(snap()); apply(history.takeLast()); run=0; showState(); }
    void redo() { if(future.isEmpty()) return; history.append(snap()); apply(future.takeLast()); run=0; showState(); }
    void enter() {
        auto c=edit->textCursor(); auto a=attrs(c.blockFormat());
        if(hasClass(a,"scene-break")) return;
        ++run;
        if(run==1) { c.insertBlock(QTextBlockFormat(),QTextCharFormat()); edit->setTextCursor(c); return; }
        if(run==2) {
            c.movePosition(QTextCursor::StartOfBlock); QTextBlockFormat b; b.setProperty(Attrs,Object{{"class","scene-break"}}); b.setAlignment(Qt::AlignHCenter);
            c.insertBlock(b,QTextCharFormat());
            c.movePosition(QTextCursor::PreviousBlock); c.setBlockFormat(b); c.insertText("***",QTextCharFormat());
            c.movePosition(QTextCursor::NextBlock); c.setBlockFormat(QTextBlockFormat()); edit->setTextCursor(c); return;
        }
        // Remove the immediately preceding generated scene, then transfer the tail.
        auto block=c.block(); QTextCursor tail(edit->document()); tail.setPosition(block.position()); tail.movePosition(QTextCursor::End,QTextCursor::KeepAnchor);
        QTextDocument next; QTextCursor nc(&next); nc.insertFragment(QTextDocumentFragment(tail)); QString nextHtml=encode(&next);
        tail.removeSelectedText(); tail.deletePreviousChar(); // paragraph boundary
        auto last=tail.block(); QTextCursor removal(last); removal.select(QTextCursor::BlockUnderCursor); removal.removeSelectedText();
        sync(); content.insert(current+1,nextHtml);
        auto ids=meta["chapterOrder"].toArray(); ids.insert(current+1,"ch-"+QUuid::createUuid().toString(QUuid::WithoutBraces)); meta["chapterOrder"]=ids;
        QString oldId=ids[current].toString(), newId=ids[current+1].toString();
        auto sections=meta["sectionNotes"].toObject(); QJsonArray stay, moved;
        for(auto section: sections[oldId].toArray()) {
            auto id=section.toObject()["id"].toString();
            (nextHtml.contains("data-sec-id=\""+id+"\"") ? moved : stay).append(section);
        }
        sections[oldId]=stay; sections[newId]=moved; meta["sectionNotes"]=sections;
        for(int i=0;i<stickies.size();++i) {
            auto sticky=stickies[i].toObject();
            if(nextHtml.contains("data-sid=\""+sticky["id"].toString()+"\"")) {sticky["chapterId"]=newId;stickies[i]=sticky;}
        }
        ++current; render(); run=0;
    }
    void cut() {
        auto c=edit->textCursor(); if(!c.hasSelection()) return;
        // Bounded probe: restore only inline passages without semantic markers.
        if(c.selectedText().contains(QChar::ParagraphSeparator)) throw std::runtime_error("This probe cuts only within one paragraph");
        QTextDocument fragment; QTextCursor fc(&fragment); fc.insertFragment(QTextDocumentFragment(c)); auto html=encode(&fragment);
        if(html.contains("ph-mark") || html.contains("darling-anchor")) throw std::runtime_error("Marker-bearing Darling selection refused");
        tx([&]{
            QString plain=edit->toPlainText(); int start=c.selectionStart(), end=c.selectionEnd();
            darlings.append(Object{{"id",QUuid::createUuid().toString(QUuid::WithoutBraces)},{"chapterId",meta["chapterOrder"].toArray()[current]},
                {"chapterLabel",chapters->currentText()},{"text",c.selectedText()},{"html",html.mid(html.indexOf('>')+1).chopped(4)},
                {"anchorPrefix",plain.left(start).right(60)},{"anchorSuffix",plain.mid(end).left(60)},{"date",QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}});
            c.removeSelectedText(); edit->setTextCursor(c);
        });
    }
    void restore() {
        if(darlings.isEmpty()) return;
        tx([&]{
            auto d=darlings.last().toObject(); auto ids=meta["chapterOrder"].toArray(); int target=0;
            while(target<ids.size() && ids[target]!=d["chapterId"]) ++target;
            if(target==ids.size()) target=ids.size()-1;
            sync(); current=target; render(); QString text=edit->toPlainText(); QString prefix=d["anchorPrefix"].toString(), suffix=d["anchorSuffix"].toString();
            int pos=-1;
            for(int i=0;i<=text.size();++i) if(text.left(i).endsWith(prefix) && text.mid(i).startsWith(suffix)) { if(pos>=0) {pos=-1;break;} pos=i; }
            auto c=edit->textCursor(); c.setPosition(pos<0?edit->document()->characterCount()-1:pos);
            if(pos<0) c.insertBlock(QTextBlockFormat(),QTextCharFormat());
            QTextDocument fragment; decode(&fragment,"<p>"+d["html"].toString()+"</p>");
            c.insertFragment(QTextDocumentFragment(&fragment)); edit->setTextCursor(c); darlings.removeLast();
        });
    }
    void write(QString path,QByteArray bytes) {
        QSaveFile f(path); if(!f.open(QIODevice::WriteOnly)||f.write(bytes)!=bytes.size()||!f.commit()) throw std::runtime_error("Scratch write failed");
    }
    void saveReopen() {
        sync(); QDir().mkpath(output+"/book-PROTOTYPE/chapters"); auto base=output+"/book-PROTOTYPE/";
        write(base+"book.json",QJsonDocument(meta).toJson()); write(base+"darlings.json",QJsonDocument(darlings).toJson());
        write(base+"stickies.json",QJsonDocument(stickies).toJson());
        auto ids=meta["chapterOrder"].toArray();
        QStringList reopened;
        for(int i=0;i<content.size();++i) {
            auto path=base+"chapters/"+ids[i].toString()+".html"; write(path,content[i].toUtf8()); QFile f(path); if(!f.open(QIODevice::ReadOnly)) throw std::runtime_error("Scratch reopen failed");
            QTextDocument doc; decode(&doc,QString::fromUtf8(f.readAll())); reopened.append(encode(&doc));
        }
        auto readJson=[&](QString name) {
            QFile file(base+name); if(!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Scratch JSON reopen failed");
            QJsonParseError error; auto doc=QJsonDocument::fromJson(file.readAll(),&error);
            if(error.error!=QJsonParseError::NoError) throw std::runtime_error("Scratch JSON invalid"); return doc;
        };
        meta=readJson("book.json").object(); darlings=readJson("darlings.json").array(); stickies=readJson("stickies.json").array();
        content=reopened; render(); statusBar()->showMessage("Saved/reopened synthetic HTML in "+base);
    }
    void pdf() {
        sync(); QDir().mkpath(output); QString html="<h1>Synthetic native probe</h1><p>Qt native PDF — synthetic content only</p>";
        for(int i=0;i<content.size();++i) { QTextDocument doc; decode(&doc,content[i]); html+=QString("<h2 style=\"page-break-before:always\">Chapter %1</h2>").arg(i+1)+encode(&doc,true); }
        html+="<h2 style=\"page-break-before:always\">Pagination sample</h2>";
        for(int i=0;i<80;++i) html+=QString("<p>%1. A synthetic paragraph crosses pages, with <b>emphasis</b>, <i>italics</i> and বাংলা العربية é 👩🏽‍💻.</p>").arg(i+1);
        QTextDocument doc; doc.setDefaultFont(QFont("serif",12)); doc.setHtml(html); QPdfWriter writer(output+"/sample.pdf"); writer.setPageSize(QPageSize(QPageSize::A5)); doc.print(&writer);
        statusBar()->showMessage("PDF written to "+output+"/sample.pdf (default Qt pagination; not export parity)");
    }
};

int main(int argc,char **argv) {
    QApplication app(argc,argv); app.setApplicationName("leo-editor-PROTOTYPE");
    QString out=QDir::tempPath()+"/leo-editor-PROTOTYPE-"+QString::number(QCoreApplication::applicationPid());
    if (app.arguments().contains("--roundtrip")) {
        auto args=app.arguments(); int at=args.indexOf("--roundtrip");
        if(args.size()!=at+3) { qCritical("Use --roundtrip INPUT OUTPUT"); return 2; }
        QFile input(args[at+1]); if(!input.open(QIODevice::ReadOnly)||input.size()>1024*1024) return 2;
        try {
            QTextDocument doc; decode(&doc,QString::fromUtf8(input.readAll()));
            QFile output(args[at+2]); if(!output.open(QIODevice::WriteOnly|QIODevice::NewOnly)) return 2;
            auto bytes=encode(&doc).toUtf8(); if(output.write(bytes)!=bytes.size()) return 2;
            QTextStream(stdout)<<"PASS native fixture codec accepted legacy output"<<Qt::endl;
        } catch(const std::exception &e) {qCritical("%s",e.what());return 1;}
        return 0;
    }
    Window w(out);
    if(app.arguments().contains("--probe")) {
        int failures=0; QTextStream log(stdout);
        auto check=[&](QString name,bool pass){log<<(pass?"PASS ":"FAIL ")<<name<<Qt::endl; if(!pass)++failures;};
        auto press=[&](int key,QString text=QString()){QKeyEvent e(QEvent::KeyPress,key,Qt::NoModifier,text); QApplication::sendEvent(w.edit,&e);};
        auto initial=w.data();
        w.saveReopen(); check("fixture save/reopen preserves serialized model",w.data()==initial);
        QTextDocument generic; generic.setHtml(fixture()); check("generic Qt HTML serializer loses domain attributes (negative control)",!generic.toHtml().contains("data-sid"));
        bool refused=false; try {QTextDocument d;decode(&d,"<p>before<img src=\"x\"/>after</p>");} catch(...) {refused=true;} check("unsupported HTML rejected",refused);
        auto c=w.edit->textCursor(); c.setPosition(7); w.edit->setTextCursor(c);
        press(Qt::Key_X,"x"); w.undo(); check("typing undo includes metadata",w.data()==initial); w.redo(); w.undo(); check("typing redo/undo",w.data()==initial);
        c=w.edit->textCursor();c.setPosition(7);w.edit->setTextCursor(c);
        press(Qt::Key_Return); check("first Enter splits paragraph",w.edit->document()->blockCount()==7);
        press(Qt::Key_Return); check("second Enter creates scene",w.content[0].count("scene-break")==3);
        press(Qt::Key_Return); check("third Enter creates chapter",w.content.size()==2 && w.meta["chapterOrder"].toArray().size()==2);
        check("chapter split moves section and sticky ownership",w.meta["sectionNotes"].toObject()[w.meta["chapterOrder"].toArray()[1].toString()].toArray().size()==2 && w.stickies[0].toObject()["chapterId"]==w.meta["chapterOrder"].toArray()[1]);
        w.undo();w.undo();check("structural undo restores document and chapter order",w.data()==initial);
        c=w.edit->textCursor();c.setPosition(7);c.setPosition(11,QTextCursor::KeepAnchor);w.edit->setTextCursor(c);w.cut();check("Darling cut",w.darlings.size()==1 && !w.edit->toPlainText().startsWith("Before bold"));
        w.restore();check("Darling restoration round trip",w.data()==initial);w.undo();check("undo restores Darling record",w.darlings.size()==1);w.undo();check("undo cut restores model",w.data()==initial);
        c=w.edit->textCursor();c.movePosition(QTextCursor::End);w.edit->setTextCursor(c);
        QInputMethodEvent pre(QString::fromUtf8("বাংলা"),{});QApplication::sendEvent(w.edit,&pre);check("IME preedit is not persisted",w.data()==initial);
        QInputMethodEvent cancel;QApplication::sendEvent(w.edit,&cancel);check("IME cancellation unchanged",w.data()==initial);
        QInputMethodEvent commit;commit.setCommitString(QString::fromUtf8("বাংলা"));QApplication::sendEvent(w.edit,&commit);check("IME commit inserted",w.edit->toPlainText().endsWith(QString::fromUtf8("বাংলা")));w.undo();check("IME commit undo",w.data()==initial);
        w.pdf();w.show();app.processEvents();w.grab().save(out+"/window.png");
        log<<"Platform: "<<QGuiApplication::platformName()<<"; Qt "<<qVersion()<<"; artifacts "<<out<<Qt::endl;
        return failures?1:0;
    }
    w.show(); return app.exec();
}
