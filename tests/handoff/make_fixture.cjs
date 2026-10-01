// Write a synthetic NEO Library (no real manuscript) for the Syncthing handoff
// test: 2 chapters, stickies, darlings, a protected sticky marker, unknown JSON
// keys and an unknown supporting .bin file. Shape mirrors makeNeoLibrary() in
// tests/library_browser_test.cpp. Usage: node make_fixture.cjs <libraryDir>
const fs = require('fs');
const path = require('path');
const lib = process.argv[2];
const book = path.join(lib, 'book-1');
fs.mkdirSync(path.join(book, 'chapters'), { recursive: true });
const json = (file, data) => fs.writeFileSync(file, JSON.stringify(data, null, 2));
json(path.join(lib, 'library.json'), {
  authorName: 'Ada Lovelace', penNames: ['Ada Lovelace', 'A. L.'], firstRunDone: true, pageTheme: 'night',
  shelves: [{ id: 'shelf-1', name: 'Works in Progress', bookIds: ['book-1'] }],
  futureLibraryField: { keep: true }
});
json(path.join(book, 'book.json'), {
  id: 'book-1', title: 'First Title', subtitle: 'A subtitle', series: 'A series', author: 'Ada Lovelace',
  wordGoal: 1200, created: '2026-09-01T00:00:00.000Z', modified: '2026-09-28T00:00:00.000Z',
  chapterOrder: ['chapter-a', 'chapter-b'],
  chapterTitles: { 'chapter-a': 'Arrival', 'chapter-b': 'Departure' },
  lastPosition: { chapterId: 'chapter-a', scroll: 12 },
  futureBookField: { keep: [1, 'future'] }
});
fs.writeFileSync(path.join(book, 'chapters/chapter-a.html'),
  '<p>Before prose.</p>' +
  '<p>Question <span class="ph-mark" data-sid="s-existing" contenteditable="false">⚑</span></p>' +
  '<div data-future="keep&amp;exact"><span>future</span></div><p>After prose.</p>');
fs.writeFileSync(path.join(book, 'chapters/chapter-b.html'), '<p>Second chapter prose.</p>');
fs.writeFileSync(path.join(book, 'notes.html'), '<p>Library notes</p>');
fs.writeFileSync(path.join(book, 'outline.html'), '<p>Library outline</p>');
json(path.join(book, 'stickies.json'), [{
  id: 's-existing', chapterId: 'chapter-a', text: 'keep this link', resolved: false,
  futureStickyField: { keep: true }
}]);
json(path.join(book, 'darlings.json'), [{
  id: 'd-existing', html: '<p>Saved line</p>', text: 'Saved line', chapterId: 'chapter-a',
  chapterLabel: 'Chapter 1', anchorPrefix: 'before', anchorSuffix: 'after',
  date: '2026-09-20T00:00:00.000Z', futureDarlingField: { keep: true }
}]);
fs.writeFileSync(path.join(book, 'unknown-supporting-data.bin'), Buffer.from('\0future\xff', 'latin1'));
