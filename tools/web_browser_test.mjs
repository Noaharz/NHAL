#!/usr/bin/env node
// Führt Web-Tests in einem echten Browser aus (headless Chrome/Chromium).
//
// In node fehlen WebGPU, Web Audio und die Gamepad API. Dieses Werkzeug
// liefert das Testverzeichnis über einen lokalen Server (nur 127.0.0.1) aus,
// startet den Browser mit DevTools-Protokoll, öffnet runner.html?test=<name>
// und wartet, bis der Test "alles in Ordnung" oder einen Fehler meldet.
//
// Aufruf: node tools/web_browser_test.mjs <build/web/tests> <test> [<test> …]
// Browser: Umgebungsvariable CHROME, sonst der übliche Installationsort.
// Rückgabe: 0 = alle bestanden, 1 = Fehler, 2 = Aufruf/Browser-Problem.

import { spawn } from "node:child_process";
import { createReadStream, existsSync, mkdtempSync, rmSync, statSync } from "node:fs";
import { createServer } from "node:http";
import { tmpdir } from "node:os";
import { extname, join, normalize, resolve, sep } from "node:path";

const TIMEOUT_MS = 60000;
const TYPES = { ".html": "text/html; charset=utf-8", ".js": "text/javascript", ".wasm": "application/wasm",
                ".data": "application/octet-stream" };

function findChrome() {
  if (process.env.CHROME) return process.env.CHROME;
  const candidates = {
    darwin: ["/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
             "/Applications/Chromium.app/Contents/MacOS/Chromium"],
    linux: ["/usr/bin/google-chrome", "/usr/bin/chromium", "/usr/bin/chromium-browser"],
    win32: ["C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe"],
  }[process.platform] || [];
  return candidates.find((p) => existsSync(p));
}

function serve(root) {
  const base = resolve(root) + sep;
  const server = createServer((req, res) => {
    const path = normalize(join(base, decodeURIComponent(new URL(req.url, "http://x").pathname)));
    if (!path.startsWith(base) || !existsSync(path) || !statSync(path).isFile()) {
      res.writeHead(404).end();
      return;
    }
    res.writeHead(200, { "Content-Type": TYPES[extname(path)] || "application/octet-stream" });
    createReadStream(path).pipe(res);
  });
  return new Promise((ok) => server.listen(0, "127.0.0.1", () => ok(server)));
}

async function waitForDevTools(port, deadline) {
  while (Date.now() < deadline) {
    try {
      const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
      const page = list.find((t) => t.type === "page");
      if (page) return page.webSocketDebuggerUrl;
    } catch { /* Browser startet noch */ }
    await new Promise((r) => setTimeout(r, 200));
  }
  throw new Error("Browser meldet sich nicht über DevTools");
}

function devtools(url) {
  const ws = new WebSocket(url);
  let next = 1;
  const waiting = new Map();
  const events = [];
  ws.onmessage = (ev) => {
    const msg = JSON.parse(ev.data);
    if (msg.method === "Runtime.exceptionThrown") {
      const d = msg.params.exceptionDetails;
      events.push("[Ausnahme] " + ((d.exception && d.exception.description) || d.text));
    } else if (msg.method === "Runtime.consoleAPICalled" && ["error", "warning"].includes(msg.params.type)) {
      events.push(`[console.${msg.params.type}] ` + msg.params.args.map((a) => a.value ?? a.description).join(" "));
    }
    if (msg.id && waiting.has(msg.id)) {
      waiting.get(msg.id)(msg);
      waiting.delete(msg.id);
    }
  };
  const ready = new Promise((ok, fail) => { ws.onopen = ok; ws.onerror = () => fail(new Error("DevTools-Verbindung fehlgeschlagen")); });
  const call = (method, params = {}) => new Promise((ok) => {
    const id = next++;
    waiting.set(id, ok);
    ws.send(JSON.stringify({ id, method, params }));
  });
  return { ready, call, events, close: () => ws.close() };
}

async function evaluate(dt, expr) {
  const r = await dt.call("Runtime.evaluate", { expression: expr, returnByValue: true });
  return r.result && r.result.result ? r.result.result.value : undefined;
}

async function runTest(dt, origin, name) {
  dt.events.length = 0;
  await dt.call("Page.navigate", { url: `${origin}/runner.html?test=${encodeURIComponent(name)}` });
  const deadline = Date.now() + TIMEOUT_MS;
  let result = null;
  while (Date.now() < deadline) {
    await new Promise((r) => setTimeout(r, 250));
    result = await evaluate(dt, "window.nhalResult");
    if (result) break;
  }
  const log = (await evaluate(dt, "(window.nhalLog || []).join('\\n')")) || "";
  console.log(`── ${name} (Browser) ──`);
  if (log) console.log(log);
  for (const e of dt.events) console.log(e);
  if (!result) console.log(`${name}: keine Rückmeldung nach ${TIMEOUT_MS / 1000} s`);
  return result === "OK";
}

async function main() {
  const [root, ...tests] = process.argv.slice(2);
  if (!root || !tests.length) {
    console.error("Aufruf: node tools/web_browser_test.mjs <build/web/tests> <test> [<test> …]");
    return 2;
  }
  const chrome = findChrome();
  if (!chrome) {
    console.error("Kein Chrome/Chromium gefunden (Umgebungsvariable CHROME setzen)");
    return 2;
  }
  const server = await serve(root);
  const origin = `http://127.0.0.1:${server.address().port}`;
  const profile = mkdtempSync(join(tmpdir(), "nhal-browser-"));
  const port = 9300 + Math.floor(Math.random() * 500);
  const browser = spawn(chrome, [
    "--headless=new", `--remote-debugging-port=${port}`, `--user-data-dir=${profile}`,
    "--no-first-run", "--no-default-browser-check", "--enable-unsafe-webgpu",
    "--autoplay-policy=no-user-gesture-required", "about:blank",
  ], { stdio: "ignore" });
  let ok = false;
  try {
    const dt = devtools(await waitForDevTools(port, Date.now() + 20000));
    await dt.ready;
    await dt.call("Page.enable");
    await dt.call("Runtime.enable");
    ok = true;
    for (const t of tests) ok = (await runTest(dt, origin, t)) && ok;
    dt.close();
  } catch (e) {
    console.error(String(e.message || e));
    browser.kill();
    server.close();
    rmSync(profile, { recursive: true, force: true });
    return 2;
  }
  browser.kill();
  server.close();
  await new Promise((r) => setTimeout(r, 300));
  rmSync(profile, { recursive: true, force: true });
  return ok ? 0 : 1;
}

process.exit(await main());
