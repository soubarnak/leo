// Locate an element in a `uiautomator dump` (XML on stdin) and print "x y" of
// its centre. Usage: ui.cjs text=<regex> | id=<substring> | desc=<regex>  (exit 1 if absent)
//        ui.cjs list    prints every node with text/id/desc for debugging.
const fs = require('fs');
const xml = fs.readFileSync(0, 'utf8');
const dec = (s) => s.replace(/&#10;/g, '\n').replace(/&quot;/g, '"').replace(/&amp;/g, '&').replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&apos;/g, "'");
const nodes = [...xml.matchAll(/<node ([^>]*?)\/?>/g)].map((m) => {
  const a = {};
  for (const [, k, v] of m[1].matchAll(/([\w-]+)="([^"]*)"/g)) a[k] = dec(v);
  const b = (a.bounds || '').match(/\[(\d+),(\d+)\]\[(\d+),(\d+)\]/);
  a.cx = b ? (+b[1] + +b[3]) >> 1 : 0; a.cy = b ? (+b[2] + +b[4]) >> 1 : 0;
  return a;
});
const arg = process.argv[2] || '';
if (arg === 'webview') {   // "left top" of the app's WebView on screen
  const w = xml.match(/class="android.webkit.WebView"[^>]*bounds="\[(\d+),(\d+)\]/);
  if (!w) process.exit(1);
  console.log(w[1] + ' ' + w[2]);
  process.exit(0);
}
if (arg === 'list') {
  for (const n of nodes) if (n.text || n['content-desc'] || n['resource-id'])
    console.log(`${n['resource-id'] || '-'} | ${JSON.stringify(n.text)} | ${JSON.stringify(n['content-desc'])} | ${n.cx},${n.cy} ${n.clickable === 'true' ? 'C' : ''}`);
  process.exit(0);
}
const [kind, ...rest] = arg.split('='); const pat = rest.join('=');
const hit = nodes.find((n) =>
  kind === 'text' ? new RegExp(pat, 'i').test(n.text) && n.text :
  kind === 'id' ? (n['resource-id'] || '').includes(pat) :
  kind === 'desc' ? new RegExp(pat, 'i').test(n['content-desc']) && n['content-desc'] : false);
if (!hit) process.exit(1);
console.log(hit.cx + ' ' + hit.cy);
