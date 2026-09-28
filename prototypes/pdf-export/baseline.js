// THROWAWAY: run NEO 0.7.9's export builders on a synthetic book.
const { app, BrowserWindow } = require('electron');
const fs = require('fs');
const path = require('path');

const root = path.resolve(__dirname, '../..');
const out = path.join(__dirname, 'evidence');
fs.mkdirSync(out, { recursive: true });

function extract(file, name) {
  const source = fs.readFileSync(path.join(root, file), 'utf8');
  const marker = `${name}(`;
  const functionStart = source.indexOf(`function ${marker}`);
  const start = functionStart >= 6 && source.slice(functionStart - 6, functionStart) === 'async '
    ? functionStart - 6 : functionStart;
  if (start < 0) throw new Error(`Missing ${name}`);
  const next = source.indexOf('\n}', start);
  if (next < 0) throw new Error(`Unclosed ${name}`);
  return source.slice(start, next + 2);
}

function escapeHtml(s) {
  return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}
function para(runs, align = '') {
  const text = runs.map(r => r.text).join('');
  const inner = runs.map(r => {
    let value = escapeHtml(r.text);
    if (r.i) value = `<i>${value}</i>`;
    if (r.b) value = `<b>${value}</b>`;
    return value;
  }).join('');
  return { text, html: `<p${align ? ` style="text-align:${align}"` : ''}>${inner}</p>`, runs, align, sceneBreak: false };
}
const plain = text => para([{ text }]);
const sceneBreak = { text: '', html: '<p class="scene-break">***</p>', runs: [], align: '', sceneBreak: true };
const chapters = [
  {
    num: 1,
    heading: 'Chapter 1 — Arrival',
    paras: [
      para([{ text: 'The ' }, { text: 'river', i: true }, { text: ' kept its own ledger. ' }, { text: 'Every name mattered.', b: true }]),
      plain('This first paragraph tests the opening initial, body indentation, and line spacing.'),
      para([{ text: 'A centered line after two ordinary paragraphs.' }], 'center'),
      para([{ text: 'A right aligned note.' }], 'right'),
      sceneBreak,
      plain('After the scene break, the story starts again with a fresh paragraph.'),
      para([{ text: 'Mixed script: বাংলা; العربية; é; 👩🏽‍💻.' }]),
    ]
  },
  {
    num: 2,
    heading: 'Chapter 2 — The Crossing',
    paras: [
      plain('A second chapter must start on a new page, with its heading centered.'),
      ...Array.from({ length: 34 }, (_, i) => para([
        { text: `Paragraph ${i + 1}. The river turns past the old bridge while a writer checks page flow, line breaks, and the readable page edge. ` },
        { text: 'This clause is bold.', b: true },
        { text: ' This one is italic.', i: true }
      ]))
    ]
  }
];
const data = {
  id: 'pdf-PROTOTYPE', title: 'The River Ledger', subtitle: 'A synthetic export sample',
  author: 'Ada Example', sections: chapters
};
fs.writeFileSync(path.join(out, 'fixture.json'), JSON.stringify(data, null, 2));

const coverSvg = `<svg xmlns="http://www.w3.org/2000/svg" width="800" height="1200" viewBox="0 0 800 1200"><rect width="800" height="1200" fill="#213747"/><path d="M90 810 Q400 640 710 830 M90 875 Q400 705 710 895" fill="none" stroke="#b9c8b0" stroke-width="20"/><text x="400" y="390" text-anchor="middle" fill="white" font-family="serif" font-size="60">THE RIVER</text><text x="400" y="470" text-anchor="middle" fill="white" font-family="serif" font-size="60">LEDGER</text><text x="400" y="1050" text-anchor="middle" fill="white" font-family="serif" font-size="34">ADA EXAMPLE</text></svg>`;
const cover = { mime: 'image/svg+xml', base64: Buffer.from(coverSvg).toString('base64') };
fs.writeFileSync(path.join(out, 'cover.svg'), coverSvg);

async function run() {
  const win = new BrowserWindow({ show: false, webPreferences: { sandbox: true } });
  try {
    await win.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent('<!doctype html><meta charset="utf-8">'));
    const builder = extract('app.js', 'buildHtml');
    const html = await win.webContents.executeJavaScript(`(() => { const countWords = text => (text.trim().match(/\\S+/g) || []).length; ${builder}; return buildHtml(${JSON.stringify(data)}, {cover: ${JSON.stringify(cover)}}); })()`);
    fs.writeFileSync(path.join(out, 'baseline.html'), html);

    // Exact NEO renderPDF body; only country lookup substituted to render both paper choices.
    const requestedCountry = 'IN';
    const source = extract('main.js', 'renderPDF').replace('app.getLocaleCountryCode()', 'requestedCountry');
    const renderPDF = eval(`(${source})`);
    fs.writeFileSync(path.join(out, 'baseline-a4.pdf'), await renderPDF(html));
    const renderLetter = eval(`(${source.replace('requestedCountry', "'US'")})`);
    fs.writeFileSync(path.join(out, 'baseline-letter.pdf'), await renderLetter(html));
  } finally {
    win.destroy();
    app.quit();
  }
}
app.whenReady().then(run).catch(error => { console.error(error); app.exit(1); });
