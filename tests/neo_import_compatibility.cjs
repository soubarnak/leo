// Exercise NEO's actual Library/book/chapter read IPC handlers without a window.
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const assert = require('assert');
const [mainPath, libraryPath, bookId] = process.argv.slice(2);
const handlers = new Map();
const source = fs.readFileSync(mainPath, 'utf8');
const end = source.indexOf("ipcMain.handle('chapter:delete'");
assert(end > 0, 'NEO read handlers must be available');
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
const library = handlers.get('library:read')();
assert(library.shelves.some(shelf => shelf.bookIds.includes(bookId)));
const book = handlers.get('book:readMeta')(null, bookId);
assert.strictEqual(book.title, 'story');
assert.strictEqual(book.chapterOrder.length, 2);
assert.strictEqual(handlers.get('chapter:read')(null, bookId, book.chapterOrder[0]), '<p>Hello.</p>\n');
assert.strictEqual(handlers.get('chapter:read')(null, bookId, book.chapterOrder[1]), '<p>World.</p>\n');
