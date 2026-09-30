"""Read exported publications through ZIP/XML and, when installed, EbookLib."""
import importlib.util
import posixpath
import struct
import sys
import xml.etree.ElementTree as ET
import zipfile

path, kind = sys.argv[1:]
ns = {'opf': 'http://www.idpf.org/2007/opf', 'dc': 'http://purl.org/dc/elements/1.1/',
      'h': 'http://www.w3.org/1999/xhtml'}
with zipfile.ZipFile(path) as archive:
    assert archive.testzip() is None
    mime = archive.infolist()[0]
    assert mime.filename == 'mimetype' and mime.compress_type == 0 and not mime.extra
    assert archive.read(mime) == b'application/epub+zip'
    container = ET.fromstring(archive.read('META-INF/container.xml'))
    package_path = container.find('.//{*}rootfile').attrib['full-path']
    base = posixpath.dirname(package_path) + '/'
    package = ET.fromstring(archive.read(package_path))
    assert package.attrib['version'] == '3.0'
    assert package.find('opf:metadata/dc:title', ns).text == ('Collected' if kind == 'shelf' else 'Story & more')
    items = {i.attrib['id']: i.attrib for i in package.find('opf:manifest', ns)}
    for item in items.values():
        data = archive.read(base + item['href'])
        if item['media-type'] == 'application/xhtml+xml':
            ET.fromstring(data)
            assert b'Private' not in data
    covers = [i for i in items.values() if i.get('properties') == 'cover-image']
    assert len(covers) == 1
    image = archive.read(base + covers[0]['href'])
    assert struct.unpack('>II', image[16:24]) == ((1600, 2560) if kind == 'fallback' else (4, 6))
    nav_item = next(i for i in items.values() if i.get('properties') == 'nav')
    nav = ET.fromstring(archive.read(base + nav_item['href']))
    links = nav.findall('.//h:a', ns)
    labels = [a.text for a in links]
    assert labels[:3] == ['Story & more', 'Chapter 1 — Arrival', 'Chapter 2 — Departure']
    for link in links:
        archive.read(base + link.attrib['href'])
    spine = [items[i.attrib['idref']]['href'] for i in package.find('opf:spine', ns)]
    assert spine.index('book1-chapter1.xhtml') < spine.index('book1-chapter2.xhtml')
    chapter = ET.fromstring(archive.read(base + 'book1-chapter1.xhtml'))
    text = ''.join(chapter.itertext())
    assert 'Real bold soft both' in text and 'Next line.' in text
    assert text.count('***') == 1
    scene = chapter.find('.//h:p[@class="brk"]', ns)
    assert scene is not None and scene.text == '***'
    assert 'After scene.' in text
    css = archive.read(base + 'style.css')
    assert b'p.brk { text-align: center; text-indent: 0; margin: 2.5em 0;' in css
    assert b'p.brk + p { text-indent: 0; }' in css
    assert chapter.find('.//h:b/h:i', ns).text == 'both'
    assert chapter.find('.//h:p', ns).attrib['style'] == 'text-align:right'
    assert chapter.find('.//h:br', ns) is not None
    last = ET.fromstring(archive.read(base + 'book1-chapter2.xhtml'))
    assert last.find('.//h:p', ns).attrib['style'] == 'text-align:justify'
    assert b'font-family: serif' in archive.read(base + 'style.css')
    if kind == 'shelf':
        assert labels[3:] == ['Other story', 'Other story']
        assert spine.index('book1-chapter2.xhtml') < spine.index('book2-chapter1.xhtml')
        assert b'Other prose.' in archive.read(base + 'book2-chapter1.xhtml')
        assert len([n for n in spine if 'chapter' in n]) == 3

if importlib.util.find_spec('ebooklib'):
    from ebooklib import epub
    publication = epub.read_epub(path)
    documents = [publication.get_item_with_id(i).get_content() for i, _ in publication.spine]
    prose = b''.join(documents)
    assert prose.index(b'Real') < prose.index(b'Last prose.')
    assert b'Private' not in prose
    assert len(publication.toc) == (2 if kind == 'shelf' else 1)
    if kind == 'shelf':
        assert prose.index(b'Last prose.') < prose.index(b'Other prose.')
    if importlib.util.find_spec('PIL'):
        import io
        from PIL import Image
        pixel = Image.open(io.BytesIO(image)).convert('RGB').getpixel((0, 0))
        assert pixel != (0, 0, 255) if kind == 'fallback' else pixel == (255, 0, 0)
