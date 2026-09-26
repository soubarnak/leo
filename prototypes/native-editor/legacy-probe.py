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
chapter = meta['chapterOrder'][0]
fixture = (book / 'chapters' / (chapter + '.html')).read_text()
with tempfile.TemporaryDirectory(prefix='leo-legacy-PROTOTYPE-') as temp:
    folder = Path(temp)
    script = '\n'.join(function(n) for n in ['captureBody', 'syncGhosts', 'reconcileMarks'])
    page = '''<!doctype html><meta charset="utf-8"><div class="chapter"><div class="chapter-body" contenteditable="true"></div></div><pre id="result"></pre><script>'''
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
    (folder / 'probe.html').write_text(page)
    command = ['/opt/vivaldi/vivaldi', '--headless', '--disable-gpu', '--no-first-run', '--no-default-browser-check', '--user-data-dir=' + str(folder / 'profile'), '--dump-dom', (folder / 'probe.html').as_uri()]
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True)
    try:
        stdout, stderr = process.communicate(timeout=40)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.communicate()
        raise RuntimeError('Browser probe timed out; no compatibility result established')
    if process.returncode:
        raise RuntimeError(stderr[-2000:])
    match = re.search(r'<pre id="result">([^<]+)</pre>', stdout)
    if not match:
        raise RuntimeError('Browser probe did not produce a result: ' + stderr[-1000:])
    result = json.loads(base64.b64decode(html.unescape(match.group(1))))
    out = Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    (out / 'legacy-result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2))
    (out / 'legacy-saved.html').write_text(result['after'])
    assert result['after'].startswith('<p data-extra="keep-me">Legacy edit: ')
    assert 'data-sid="sticky-one"' in result['after']
    assert 'data-sec-id="section-ghost"' in result['after']
    assert 'data-sec-id="section-written"' in result['after']
    assert result['book']['unknownMetadata'] == meta['unknownMetadata']
    print('PASS baseline syncGhosts/reconcileMarks/captureBody retain fixture identities and unknown book metadata after a browser edit')
    print('Legacy-saved fixture:', out / 'legacy-saved.html')
