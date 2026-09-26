#!/usr/bin/env python3
"""Exercise actual baseline DOM routines on the synthetic native-saved fixture.
This is not a full legacy desktop/Pocket application acceptance test.
"""
import base64
import html
import json
from pathlib import Path
import re
import os
import signal
import subprocess
import sys
import tempfile
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

root = Path(__file__).resolve().parents[2]
source = (root / 'app.js').read_text()
book = Path(sys.argv[1])
# Take whole known top-level functions verbatim, stopping at their unindented closing brace.
def function(name):
    match = re.search(r'^function ' + name + r'\([^\n]*\) \{.*?^\}', source, re.M | re.S)
    if not match:
        raise RuntimeError('Cannot locate baseline function ' + name)
    return match.group(0)
meta = json.loads((book / 'book.json').read_text())
if meta.get('id') != 'book-PROTOTYPE':
    raise RuntimeError('This harness accepts only the synthetic prototype book')
out = Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=False)
chapter = meta['chapterOrder'][0]
fixture = (book / 'chapters' / (chapter + '.html')).read_text()
with tempfile.TemporaryDirectory(prefix='leo-legacy-PROTOTYPE-') as temp:
    folder = Path(temp)
    script = '\n'.join(function(n) for n in ['captureBody', 'syncGhosts', 'reconcileMarks'])
    page = '''<!doctype html><meta charset="utf-8"><title>Leo synthetic legacy compatibility probe</title><h1>Synthetic legacy compatibility probe</h1><p>Baseline DOM routines only; not the full desktop or Pocket application.</p><div class="chapter"><div class="chapter-body" contenteditable="true"></div></div><pre id="result"></pre><script>'''
    page += 'let book=' + json.dumps(meta) + ';let chapter=' + json.dumps(chapter) + ';'
    page += 'let stickies=' + (book / 'stickies.json').read_text() + ';'
    page += '''let body=document.querySelector('.chapter-body');document.querySelector('.chapter').dataset.id=chapter;
let writes=[];window.neo={writeJSON:(...a)=>writes.push(a)};
function renderStickies(){} function renderNav(){} function syncChapter(){}
'''
    page += 'body.innerHTML=' + json.dumps(fixture) + ';' + script
    page += '''
syncGhosts(chapter);reconcileMarks();
let before=captureBody(body);body.focus();
let r=document.createRange();r.setStart(body.querySelector('p').firstChild,0);r.collapse(true);
let s=window.getSelection();s.removeAllRanges();s.addRange(r);
document.execCommand('insertText',false,'Legacy edit: ');
let result={before,after:captureBody(body),book,stickies,writes};
document.querySelector('#result').textContent=btoa(unescape(encodeURIComponent(JSON.stringify(result))));
</script>'''
    if '--serve' in sys.argv:
        page = page.replace('</script>', '''
fetch('/result', {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(result)})
.then(r=>{if(!r.ok)throw Error('Capture failed');document.querySelector('#result').textContent='Captured synthetic browser result for native reimport. Assertions run in the terminal.';});
</script>''')
        captured = []
        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                if self.path != '/':
                    self.send_error(404); return
                data = page.encode()
                self.send_response(200)
                self.send_header('Content-Type', 'text/html; charset=utf-8')
                self.end_headers(); self.wfile.write(data)
            def do_POST(self):
                if self.path != '/result' or self.headers.get('Origin') != origin:
                    self.send_error(403); return
                length = int(self.headers.get('Content-Length', 0))
                if not 0 < length < 1024 * 1024:
                    self.send_error(413); return
                captured.append(json.loads(self.rfile.read(length)))
                self.send_response(200); self.end_headers()
            def log_message(self, *args):
                pass
        with HTTPServer(('127.0.0.1', 0), Handler) as server:
            origin = 'http://127.0.0.1:' + str(server.server_port)
            print('Open synthetic probe:', origin, flush=True)
            server.timeout = 1
            deadline = time.monotonic() + 300
            while not captured and time.monotonic() < deadline:
                server.handle_request()
            if not captured:
                raise RuntimeError('No browser result within five minutes')
        result = captured[0]
    else:
        result = None
    (folder / 'probe.html').write_text(page)
    command = ['/opt/vivaldi/vivaldi', '--headless', '--disable-gpu', '--no-first-run', '--no-default-browser-check', '--user-data-dir=' + str(folder / 'profile'), '--dump-dom', (folder / 'probe.html').as_uri()]
    process = None if result is not None else subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True)
    try:
        stdout, stderr = process.communicate(timeout=40) if process else ('', '')
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.communicate()
        raise RuntimeError('Browser probe timed out; no compatibility result established')
    if process and process.returncode:
        raise RuntimeError(stderr[-2000:])
    match = re.search(r'<pre id="result">([^<]+)</pre>', stdout)
    if result is None and not match:
        raise RuntimeError('Browser probe did not produce a result: ' + stderr[-1000:])
    if result is None:
        result = json.loads(base64.b64decode(html.unescape(match.group(1))))
    (out / 'legacy-result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2))
    (out / 'legacy-saved.html').write_text(result['after'])
    assert result['after'].startswith('<p data-extra="keep-me">Legacy edit: ')
    assert 'data-sid="sticky-one"' in result['after']
    assert 'data-sec-id="section-ghost"' in result['after']
    assert 'data-sec-id="section-written"' in result['after']
    assert result['book']['unknownMetadata'] == meta['unknownMetadata']
    print('PASS baseline syncGhosts/reconcileMarks/captureBody retain fixture identities and unknown book metadata after a browser edit')
    print('Legacy-saved fixture:', out / 'legacy-saved.html')
