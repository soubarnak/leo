// Evaluate a JS expression in Pocket's real WebView through Chrome DevTools
// (adb-forwarded to http://127.0.0.1:<port>). Read-only helper: the handoff test
// uses it to find element positions and read state; edits go in via adb input.
//   node cdp.cjs <port> '<expression>'    prints the JSON result
const [port, expression] = process.argv.slice(2);
(async () => {
  const targets = await (await fetch(`http://127.0.0.1:${port}/json`)).json();
  const page = targets.find((t) => t.type === 'page');
  if (!page) throw new Error('no page target');
  const ws = new WebSocket(page.webSocketDebuggerUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = () => rej(new Error('ws error')); });
  const reply = await new Promise((res) => {
    ws.onmessage = (m) => { const d = JSON.parse(m.data); if (d.id === 1) res(d); };
    ws.send(JSON.stringify({ id: 1, method: 'Runtime.evaluate',
      params: { expression, returnByValue: true, awaitPromise: true } }));
  });
  ws.close();
  if (reply.result.exceptionDetails) throw new Error(JSON.stringify(reply.result.exceptionDetails));
  process.stdout.write(JSON.stringify(reply.result.result.value === undefined ? null : reply.result.result.value));
})().catch((e) => { console.error(e.message); process.exit(1); });
