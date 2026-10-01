// Compare the original fixture snapshot with what NEO reads after the full
// Syncthing handoff. Usage: node assert_handoff.cjs <initial.json> <final.json> <p1> <p2> <p3> <binSha256>
const fs = require('fs');
const assert = require('assert');
const crypto = require('crypto');
const [initialFile, finalFile, ...paras] = process.argv.slice(2);
const binSha = paras.pop();
const initial = JSON.parse(fs.readFileSync(initialFile, 'utf8'));
const final = JSON.parse(fs.readFileSync(finalFile, 'utf8'));
const original = crypto.createHash('sha256').update(Buffer.from('\0future\xff', 'latin1')).digest('hex');

if (process.env.HANDOFF_REAL_EDITOR) {
  // The real Pocket app additively migrates library.json on first open (an `authors`
  // list derived from the pen names, and the one-time `hintShown` flag). Everything
  // that was there before, including unknown keys, must be unchanged.
  const added = Object.keys(final.library).filter((k) => !(k in initial.library));
  const allowed = ['authors', 'hintShown'];
  assert(added.every((k) => allowed.includes(k)), 'unexpected library.json keys added: ' + added);
  for (const k of added) console.log('note: real app added library.json key: ' + k);
  const kept = { ...final.library };
  for (const k of added) delete kept[k];
  assert.deepStrictEqual(kept, initial.library, 'library.json unchanged incl. unknown keys');
} else {
  assert.deepStrictEqual(final.library, initial.library, 'library.json unchanged incl. unknown keys');
}
assert.deepStrictEqual(final.book.chapterOrder, initial.book.chapterOrder, 'chapter order');
const { modified: _a, ...bookInitial } = initial.book;
const { modified: _b, ...bookFinal } = final.book;
if (process.env.HANDOFF_REAL_EDITOR) {
  // Pocket's real app records writing progress in book.json (word count, per-day
  // counts, per-chapter notes) and where the reader scrolled to. Those may be new
  // or changed; everything else, including titles and unknown keys, must be intact.
  const added = Object.keys(bookFinal).filter((k) => !(k in bookInitial));
  const allowed = ['chapterNotes', 'wordCount', 'dailyCounts'];
  assert(added.every((k) => allowed.includes(k)), 'unexpected book.json keys added: ' + added);
  for (const k of added) { console.log('note: real app added book.json key: ' + k); delete bookFinal[k]; }
  assert.strictEqual(bookFinal.lastPosition && bookFinal.lastPosition.chapterId, bookInitial.lastPosition.chapterId, 'last chapter');
  bookFinal.lastPosition = bookInitial.lastPosition;
}
assert.deepStrictEqual(bookFinal, bookInitial, 'book.json keeps titles and unknown keys');
assert.deepStrictEqual(final.stickies, initial.stickies, 'stickies');
assert.deepStrictEqual(final.darlings, initial.darlings, 'darlings');
assert.strictEqual(final.chapters['chapter-b'], initial.chapters['chapter-b'], 'untouched chapter');
const a = final.chapters['chapter-a'];
if (process.env.HANDOFF_REAL_EDITOR) {
  // Pocket's real editor (app.js in a WebView) re-serializes the chapter it saves,
  // so byte-prefix equality is not expected. Check what must survive instead.
  assert(a.indexOf('Before prose.') < a.indexOf('Question') && a.indexOf('Question') < a.indexOf('After prose.'),
    'original paragraphs kept in order');
  assert(/<span class="ph-mark" data-sid="s-existing" contenteditable="false"[^>]*>/.test(a), 'protected sticky marker');
  assert(/<div data-future="keep&amp;exact">[\s\S]*future[\s\S]*<\/div>/.test(a), 'unknown element, attribute and text');
  const normalized = [];
  if (!a.includes('<div data-future="keep&amp;exact"><span>future</span></div>')) normalized.push('inner <span> of unknown <div> unwrapped by editor');
  if (/class="ph-mark"[^>]*inputmode=/.test(a)) normalized.push('inputmode="none" added to sticky marker by Pocket');
  for (const n of normalized) console.log('note: real editor normalized chapter HTML: ' + n);
} else {
  assert(a.startsWith(initial.chapters['chapter-a']), 'original chapter content kept as prefix');
  assert(a.includes('data-sid="s-existing"') && a.includes('class="ph-mark"'), 'protected sticky marker');
  assert(a.includes('<div data-future="keep&amp;exact"><span>future</span></div>'), 'unknown markup');
}
const at = paras.map((p) => a.indexOf('<p>' + p + '</p>'));
assert(at.every((i) => i > 0) && at[0] < at[1] && at[1] < at[2], 'edits present in order: ' + at);
assert.strictEqual(binSha, original, 'unknown .bin byte-identical (sha256)');
console.log('handoff assertions passed');
