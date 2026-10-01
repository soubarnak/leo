// Drive NEO's real Library/book/chapter/json IPC handlers without a window to
// stand in for the NEO half of a LEO -> NEO -> LEO device handoff.
//   node neo_handoff_roundtrip.cjs <main.js> <library> <bookId> read
//   node neo_handoff_roundtrip.cjs <main.js> <library> <bookId> edit "<paragraph text>"
// "edit" reads everything the way NEO's renderer does, appends one plain
// paragraph to the first chapter, and writes back what NEO would keep:
// the chapter, book.json, stickies.json and darlings.json. Emits JSON on stdout.
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const assert = require('assert');
const [mainPath, libraryPath, bookId, mode, text] = process.argv.slice(2);
assert(mode === 'read' || mode === 'edit', 'mode must be read or edit');
const handlers = new Map();
const source = fs.readFileSync(mainPath, 'utf8');
const end = source.indexOf("ipcMain.handle('book:delete'");
assert(end > 0, 'NEO book handlers must be available');
const context = vm.createContext({
  require(name) {
    if (name === 'electron') return {
      app: { commandLine: { appendSwitch() {} } },
      ipcMain: { handle(name, handler) { handlers.set(name, handler); } }
    };
    return require(name);
  },
  libraryPath
});
vm.runInContext(source.slice(0, end) + '\nLIBRARY_DIR = libraryPath; LIBRARY_FILE = path.join(libraryPath, "library.json");', context);
const call = (name, ...args) => handlers.get(name)(null, ...args);

function snapshot() {
  const library = call('library:read');
  assert(library.shelves.some(shelf => shelf.bookIds.includes(bookId)), 'book must be on a shelf');
  const book = call('book:readMeta', bookId);
  const chapters = {};
  for (const id of book.chapterOrder) chapters[id] = call('chapter:read', bookId, id);
  return {
    library, book, chapters,
    stickies: call('json:read', bookId, 'stickies', []),
    darlings: call('json:read', bookId, 'darlings', [])
  };
}

const before = snapshot();
if (mode === 'edit') {
  const first = before.book.chapterOrder[0];
  call('chapter:write', bookId, first, before.chapters[first] + '<p>' + text + '</p>\n');
  call('book:writeMeta', bookId, before.book);
  call('json:write', bookId, 'stickies', before.stickies);
  call('json:write', bookId, 'darlings', before.darlings);
}
process.stdout.write(JSON.stringify(mode === 'edit' ? snapshot() : before));
