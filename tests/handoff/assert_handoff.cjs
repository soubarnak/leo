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

assert.deepStrictEqual(final.library, initial.library, 'library.json unchanged incl. unknown keys');
assert.deepStrictEqual(final.book.chapterOrder, initial.book.chapterOrder, 'chapter order');
const { modified: _a, ...bookInitial } = initial.book;
const { modified: _b, ...bookFinal } = final.book;
assert.deepStrictEqual(bookFinal, bookInitial, 'book.json keeps titles and unknown keys');
assert.deepStrictEqual(final.stickies, initial.stickies, 'stickies');
assert.deepStrictEqual(final.darlings, initial.darlings, 'darlings');
assert.strictEqual(final.chapters['chapter-b'], initial.chapters['chapter-b'], 'untouched chapter');
const a = final.chapters['chapter-a'];
assert(a.startsWith(initial.chapters['chapter-a']), 'original chapter content kept as prefix');
assert(a.includes('data-sid="s-existing"') && a.includes('class="ph-mark"'), 'protected sticky marker');
assert(a.includes('<div data-future="keep&amp;exact"><span>future</span></div>'), 'unknown markup');
const at = paras.map((p) => a.indexOf('<p>' + p + '</p>'));
assert(at.every((i) => i > 0) && at[0] < at[1] && at[1] < at[2], 'edits present in order: ' + at);
assert.strictEqual(binSha, original, 'unknown .bin byte-identical (sha256)');
console.log('handoff assertions passed');
