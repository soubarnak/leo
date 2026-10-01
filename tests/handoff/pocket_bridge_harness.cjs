// Run NEO Pocket's real pocket/www/pocket-bridge.js headlessly against a
// Library folder, standing in for the phone half of a Syncthing handoff.
//   node pocket_bridge_harness.cjs <pocket-bridge.js> <library> <bookId> read
//   node pocket_bridge_harness.cjs <pocket-bridge.js> <library> <bookId> edit "<paragraph text>"
// The bridge source is evaluated unmodified in a vm context. Only the platform
// under it is faked: a Capacitor Filesystem plugin over node fs (the folder
// Pocket calls "Documents/NEO Library" is mapped to <library>) and a minimal
// window/document. No Android app, WebView or Capacitor native layer runs.
// "edit" performs the calls Pocket's open-book + autosave path makes: read the
// Library, book.json, chapter and stickies/darlings, then writeChapter
// (appending one paragraph), writeBookMeta, writeJSON stickies and darlings.
// Emits the same JSON snapshot shape as neo_handoff_roundtrip.cjs on stdout.
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const assert = require('assert');
const [bridgePath, libraryPath, bookId, mode, text] = process.argv.slice(2);
assert(mode === 'read' || mode === 'edit', 'mode must be read or edit');

const ROOT = 'NEO Library';
const toHost = (p) => {
  assert(p === ROOT || p.startsWith(ROOT + '/'), 'path outside NEO Library: ' + p);
  return path.join(libraryPath, p.slice(ROOT.length));
};
const Filesystem = {
  async readFile({ path: p, encoding }) {
    const buf = fs.readFileSync(toHost(p));
    return { data: encoding ? buf.toString(encoding) : buf.toString('base64') };
  },
  async writeFile({ path: p, data, encoding, recursive }) {
    const file = toHost(p);
    if (recursive) fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file, encoding ? Buffer.from(data, encoding) : Buffer.from(data, 'base64'));
  },
  async mkdir({ path: p, recursive }) { fs.mkdirSync(toHost(p), { recursive: !!recursive }); },
  async readdir({ path: p }) { return { files: fs.readdirSync(toHost(p)).map((name) => ({ name })) }; },
  async deleteFile({ path: p }) { fs.unlinkSync(toHost(p)); },
  async getUri({ path: p }) { return { uri: 'file://' + toHost(p) }; }
};
const noop = () => {};
const window = {
  Capacitor: { Plugins: { Filesystem }, convertFileSrc: (u) => u }
};
const context = vm.createContext({
  window,
  document: { addEventListener: noop, createElement: () => ({ style: {}, addEventListener: noop }),
              getElementById: () => null, body: { appendChild: noop } },
  localStorage: { getItem: () => null, setItem: noop },
  MutationObserver: class { observe() {} },
  console, Date, Math, JSON, String, Promise
});
vm.runInContext(fs.readFileSync(bridgePath, 'utf8'), context, { filename: bridgePath });
const neo = window.neo;
assert(neo && neo.readLibrary && neo.writeChapter, 'pocket bridge must define window.neo');

async function snapshot() {
  const library = await neo.readLibrary();
  assert(library.shelves.some((shelf) => shelf.bookIds.includes(bookId)), 'book must be on a shelf');
  const book = await neo.readBookMeta(bookId);
  assert(book, 'book.json must be readable by Pocket');
  const chapters = {};
  for (const id of book.chapterOrder) chapters[id] = await neo.readChapter(bookId, id);
  return {
    library, book, chapters,
    stickies: await neo.readJSON(bookId, 'stickies', []),
    darlings: await neo.readJSON(bookId, 'darlings', [])
  };
}

(async () => {
  const before = await snapshot();
  if (mode === 'edit') {
    const first = before.book.chapterOrder[0];
    await neo.writeChapter(bookId, first, before.chapters[first] + '<p>' + text + '</p>\n');
    await neo.writeBookMeta(bookId, before.book);
    await neo.writeJSON(bookId, 'stickies', before.stickies);
    await neo.writeJSON(bookId, 'darlings', before.darlings);
  }
  process.stdout.write(JSON.stringify(mode === 'edit' ? await snapshot() : before));
})().catch((err) => { console.error(err); process.exit(1); });
