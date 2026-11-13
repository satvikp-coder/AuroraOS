/**
 * tests/perf/profile.js - M8-H: measure the Aurora Glass prototype. NOT part of CI and NOT a pass/fail test: it prints and saves MEASUREMENTS so changes can be
 * judged against real numbers instead of guesses. Run:  node tests/perf/profile.js [--runs 7] [--out docs/agents/perf-results.json]
 *
 * What it measures (headless Chromium on THIS machine, software rendering, so absolute times are NOT phone numbers; compare before/after on the same machine):
 *   assets    bytes of every served file and the number of requests a cold load makes
 *   load      cold-load timings (DOMContentLoaded, load, first contentful paint) and Chromium metrics (script/layout/style time, nodes, JS heap)
 *   dom       element count per screen
 *   actions   cost of opening each app (first = cold, then warm), an Index keystroke, a Vault render/search/select at 10, 200 and 1000 files, a context menu
 *   frames    frame intervals while scrolling a long list, Clear vs Soft Glass tier (backdrop-filter cost)
 * Options: --runs N, --out file.json, --tier clear|soft (Clear Glass is the baseline for tier comparisons; the app default is soft).
 * Every figure is a median of repeated runs with min and max shown. Nothing here is invented: if a section fails it is reported as failed, not filled in.
 */
const http = require("http");
const fs = require("fs");
const path = require("path");
const zlib = require("zlib");
const os = require("os");
const { chromium } = require("@playwright/test");

const ROOT = path.resolve(__dirname, "..", "..", "aurora-glass", "prototype");
const args = process.argv.slice(2);
const argv = (name, dflt) => { const i = args.indexOf("--" + name); return i >= 0 && args[i + 1] ? args[i + 1] : dflt; };
const RUNS = Math.max(3, parseInt(argv("runs", "7"), 10) || 7);
const OUT = argv("out", "");
const TIER = argv("tier", "default");                               // clear | soft | default (the app default, which is soft). Applied after load in the load/actions sections.
const MIME = { ".html": "text/html", ".js": "text/javascript", ".css": "text/css", ".json": "application/json", ".svg": "image/svg+xml", ".png": "image/png" };

const sorted = (a) => a.slice().sort((x, y) => x - y);
const median = (a) => { const s = sorted(a); const m = s.length >> 1; return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2; };
const pct = (a, p) => { const s = sorted(a); return s[Math.min(s.length - 1, Math.ceil((p / 100) * s.length) - 1)]; };
const r1 = (n) => Math.round(n * 10) / 10;
const stat = (a) => ({ median: r1(median(a)), min: r1(Math.min(...a)), max: r1(Math.max(...a)), n: a.length });
const fmt = (s) => s.median + " (" + s.min + "-" + s.max + ")";

function serve() {
  return new Promise((resolve) => {
    const srv = http.createServer((req, res) => {
      const u = decodeURIComponent(req.url.split("?")[0]); const f = path.join(ROOT, u === "/" ? "index.html" : u);
      if (!f.startsWith(ROOT) || !fs.existsSync(f) || fs.statSync(f).isDirectory()) { res.writeHead(404); return res.end("not found"); }
      res.writeHead(200, { "Content-Type": MIME[path.extname(f)] || "application/octet-stream", "Cache-Control": "no-store" });
      fs.createReadStream(f).pipe(res);
    });
    srv.listen(0, "127.0.0.1", () => resolve({ srv, url: "http://127.0.0.1:" + srv.address().port + "/" }));
  });
}

function assets() {
  const files = fs.readdirSync(ROOT).filter((f) => /\.(js|css|html)$/.test(f)).map((f) => {
    const buf = fs.readFileSync(path.join(ROOT, f));
    return { file: f, bytes: buf.length, gzip: zlib.gzipSync(buf, { level: 9 }).length };
  });
  const sum = (ext) => files.filter((f) => f.file.endsWith(ext)).reduce((a, f) => ({ bytes: a.bytes + f.bytes, gzip: a.gzip + f.gzip, count: a.count + 1 }), { bytes: 0, gzip: 0, count: 0 });
  return { js: sum(".js"), css: sum(".css"), html: sum(".html"), total: { bytes: files.reduce((a, f) => a + f.bytes, 0), gzip: files.reduce((a, f) => a + f.gzip, 0), count: files.length },
    largest: files.slice().sort((a, b) => b.bytes - a.bytes).slice(0, 6).map((f) => f.file + " " + f.bytes) };
}

async function metrics(cdp) { const m = (await cdp.send("Performance.getMetrics")).metrics; return Object.fromEntries(m.map((x) => [x.name, x.value])); }

async function loadSection(browser, base) {
  const runs = [];
  for (let i = 0; i < RUNS; i++) {
    const ctx = await browser.newContext({ viewport: { width: 390, height: 844 } }); const page = await ctx.newPage();
    const cdp = await ctx.newCDPSession(page); await cdp.send("Performance.enable");
    let requests = 0; page.on("request", () => { requests++; });
    await page.goto(base, { waitUntil: "load" }); await page.waitForTimeout(400);
    if (TIER !== "default") { await page.evaluate((t) => window.AuroraShell.setTier(t), TIER); await page.waitForTimeout(150); }
    const nav = await page.evaluate(() => {
      const n = performance.getEntriesByType("navigation")[0], fcp = performance.getEntriesByName("first-contentful-paint")[0];
      return { dcl: n.domContentLoadedEventEnd, load: n.loadEventEnd, fcp: fcp ? fcp.startTime : null };
    });
    const m = await metrics(cdp);
    runs.push({ ...nav, requests, scriptMs: m.ScriptDuration * 1000, layoutMs: m.LayoutDuration * 1000, styleMs: m.RecalcStyleDuration * 1000, taskMs: m.TaskDuration * 1000, nodes: m.Nodes, heapMB: m.JSHeapUsedSize / 1048576, layouts: m.LayoutCount, styles: m.RecalcStyleCount });
    await ctx.close();
  }
  const col = (k) => runs.map((r) => r[k]).filter((v) => v !== null);
  return { domContentLoadedMs: stat(col("dcl")), loadMs: stat(col("load")), firstContentfulPaintMs: col("fcp").length ? stat(col("fcp")) : null, requests: runs[0].requests,
    scriptMs: stat(col("scriptMs")), layoutMs: stat(col("layoutMs")), styleMs: stat(col("styleMs")), taskMs: stat(col("taskMs")), layoutCount: runs[0].layouts, styleRecalcCount: runs[0].styles, nodes: runs[0].nodes, jsHeapMB: stat(col("heapMB")) };
}

async function domSection(page) {
  const screens = { home: () => window.AuroraShell.showScreen("screen-home"), notes: () => window.AuroraShell.openApp("notes"), clock: () => window.AuroraShell.openApp("clock"), calculator: () => window.AuroraShell.openApp("calculator"),
    vault: () => window.AuroraShell.openApp("vault"), pages: () => window.AuroraShell.showScreen("screen-pages"), passport: () => window.AuroraShell.openApp("passport"), trail: () => window.AuroraShell.openApp("trail"), settings: () => window.AuroraShell.openApp("settings"),
    index: () => window.AuroraShell.openIndex("a") };
  const out = {};
  for (const [name, fn] of Object.entries(screens)) { await page.evaluate(fn); await page.waitForTimeout(120); out[name] = await page.evaluate(() => document.querySelectorAll("*").length); }
  return out;
}

async function actionsSection(browser, base) {
  const ctx = await browser.newContext({ viewport: { width: 390, height: 844 } }); const page = await ctx.newPage();
  const errors = []; page.on("pageerror", (e) => errors.push(String(e)));
  await page.goto(base); await page.waitForTimeout(300);
  await page.evaluate(() => { window.AuroraSystem.dev.stepDelayMs = 0; });
  if (TIER !== "default") { await page.evaluate((t) => window.AuroraShell.setTier(t), TIER); await page.waitForTimeout(150); }
  const res = {};

  res.dom = await domSection(page);

  // opening each app: first = cold, then warm. Includes style + layout (forced), not paint.
  res.openApp = await page.evaluate(() => {
    const flush = () => document.body.getBoundingClientRect().height, out = {};
    ["notes", "clock", "calculator", "vault", "passport", "trail", "settings"].forEach((id) => {
      window.AuroraShell.showScreen("screen-home"); flush();
      const t0 = performance.now(); window.AuroraShell.openApp(id); flush(); const cold = performance.now() - t0;
      const warm = [];
      for (let i = 0; i < 7; i++) { window.AuroraShell.showScreen("screen-home"); flush(); const s = performance.now(); window.AuroraShell.openApp(id); flush(); warm.push(performance.now() - s); }
      out[id] = { first: cold, warm };
    });
    return out;
  });

  // Index: typing a query one letter at a time
  res.indexKeystroke = await page.evaluate(() => {
    const flush = () => document.body.getBoundingClientRect().height;
    window.AuroraShell.openIndex(""); flush();
    const input = document.getElementById("index-query"), times = [];
    for (const q of ["n", "no", "not", "note", "notes", "c", "ca", "cal", "calc", "calcu", "calculater", "s", "se", "set"]) {
      input.value = q; const s = performance.now(); input.dispatchEvent(new Event("input", { bubbles: true })); flush(); times.push(performance.now() - s);
    }
    return times;
  });

  // Vault at growing sizes
  res.vault = {};
  for (const n of [10, 200, 1000]) {
    res.vault[n] = await page.evaluate((count) => {
      const w = window.AuroraSystem.world.files;
      for (let i = 0; Object.keys(w).length < count; i++) if (!w["bulk-" + i]) w["bulk-" + i] = { id: "bulk-" + i, name: "Bulk file " + String(i).padStart(4, "0") + ".txt", kind: "text", space: ["study", "work", "projects", "personal"][i % 4], size: 100 + i, modified: Date.now() - i * 60000, preview: "x" };
      const flush = () => document.body.getBoundingClientRect().height, med = (a) => a.slice().sort((x, y) => x - y)[a.length >> 1];
      window.AuroraShell.showScreen("screen-home"); flush();
      const r = [];
      for (let i = 0; i < 5; i++) { window.AuroraShell.showScreen("screen-home"); flush(); const s = performance.now(); window.AuroraShell.showScreen("screen-vault"); flush(); r.push(performance.now() - s); }
      const search = document.getElementById("vault-search"), srch = [];
      for (const q of ["b", "bu", "bul", "bulk", "bulk 0", "bulk 00", "bulk 001", ""]) { const el = document.getElementById("vault-search") || search; el.value = q; const s = performance.now(); el.dispatchEvent(new Event("input", { bubbles: true })); flush(); srch.push(performance.now() - s); }
      const sel = [];
      for (let i = 0; i < 6; i++) { const b = document.querySelectorAll("#screen-vault .vault-open")[i]; const s = performance.now(); b.click(); flush(); sel.push(performance.now() - s); }
      return { files: Object.keys(w).length, items: document.querySelectorAll("#screen-vault .vault-item").length, nodes: document.querySelectorAll("#screen-vault *").length, render: r, search: srch, select: sel };
    }, n);
  }

  // context menu
  res.contextMenu = await page.evaluate(() => {
    const flush = () => document.body.getBoundingClientRect().height, t = [];
    for (let i = 0; i < 15; i++) {
      const items = [{ label: "Preview", action() {} }, { label: "Select", action() {} }, { separator: true }, { label: "Move file…", action() {} }, { label: "Delete file…", danger: true, action() {} }];
      const s = performance.now(); window.AuroraContextMenu.open({ items, x: 100, y: 100 }); flush(); t.push(performance.now() - s); window.AuroraContextMenu.close();
    }
    return t;
  });

  await ctx.close();
  const sm = (a) => stat(a);
  const out = { errors, domNodes: res.dom,
    openAppMs: Object.fromEntries(Object.entries(res.openApp).map(([k, v]) => [k, { first: r1(v.first), warm: sm(v.warm) }])),
    indexKeystrokeMs: sm(res.indexKeystroke), indexKeystrokeWorstMs: r1(Math.max(...res.indexKeystroke)),
    vault: Object.fromEntries(Object.entries(res.vault).map(([n, v]) => [n, { files: v.files, renderedItems: v.items, nodes: v.nodes, openRenderMs: sm(v.render), searchKeystrokeMs: sm(v.search), selectClickMs: sm(v.select) }])),
    contextMenuOpenMs: sm(res.contextMenu) };
  return out;
}

async function framesSection(browser, base) {
  const out = {};
  for (const tier of ["clear", "soft"]) {
    const runs = [];
    for (let i = 0; i < Math.min(RUNS, 5); i++) {
      const ctx = await browser.newContext({ viewport: { width: 390, height: 844 } }); const page = await ctx.newPage();
      await page.goto(base); await page.waitForTimeout(300);
      await page.evaluate((t) => { window.AuroraSystem.dev.stepDelayMs = 0; window.AuroraShell.setTier(t); const w = window.AuroraSystem.world.files; for (let k = 0; k < 150; k++) w["bulk-" + k] = { id: "bulk-" + k, name: "Bulk file " + k + ".txt", kind: "text", space: "study", size: 100, modified: Date.now(), preview: "x" }; window.AuroraShell.openApp("vault"); }, tier);
      await page.waitForTimeout(300);
      const f = await page.evaluate(() => new Promise((resolve) => {
        const c = document.getElementById("content"), max = c.scrollHeight - c.clientHeight, stamps = []; let start = null;
        function tick(ts) { if (start === null) start = ts; stamps.push(ts); const p = ((ts - start) % 2000) / 2000; c.scrollTop = Math.abs(p * 2 - 1) * max; if (ts - start < 2500) requestAnimationFrame(tick); else resolve({ stamps, max }); }
        requestAnimationFrame(tick);
      }));
      const d = []; for (let k = 1; k < f.stamps.length; k++) d.push(f.stamps[k] - f.stamps[k - 1]);
      runs.push({ mean: d.reduce((a, b) => a + b, 0) / d.length, p95: pct(d, 95), worst: Math.max(...d), over20: d.filter((x) => x > 20).length, frames: d.length, scrollable: f.max });
      await ctx.close();
    }
    out[tier] = { frameIntervalMeanMs: stat(runs.map((r) => r.mean)), frameIntervalP95Ms: stat(runs.map((r) => r.p95)), worstFrameMs: stat(runs.map((r) => r.worst)), framesOver20ms: stat(runs.map((r) => r.over20)), framesPerRun: runs[0].frames, scrollablePx: Math.round(runs[0].scrollable) };
  }
  return out;
}

(async () => {
  const { srv, url } = await serve();
  const browser = await chromium.launch();
  const report = { when: new Date().toISOString(), runsPerMeasure: RUNS,
    environment: { os: os.type() + " " + os.release(), cpu: os.cpus()[0].model, logicalCpus: os.cpus().length, ramGB: Math.round(os.totalmem() / 1073741824), freeRamMBAtStart: Math.round(os.freemem() / 1048576), node: process.version, chromium: browser.version(), mode: "headless, software rendering, viewport 390x844, cold cache, local server without compression", tier: TIER === "default" ? "default (soft)" : TIER } };
  const sections = [["assets", async () => assets()], ["load", () => loadSection(browser, url)], ["actions", () => actionsSection(browser, url)], ["frames", () => framesSection(browser, url)]];
  for (const [name, fn] of sections) {
    try { process.stderr.write("measuring " + name + "...\n"); report[name] = await fn(); } catch (e) { report[name] = { failed: String(e && e.message || e) }; }
  }
  await browser.close(); srv.close();

  const L = report.load, A = report.actions, F = report.frames, S = report.assets;
  const lines = [];
  lines.push("ENVIRONMENT  " + JSON.stringify(report.environment));
  if (S.total) lines.push("ASSETS       " + S.total.count + " files, " + S.total.bytes + " bytes raw / " + S.total.gzip + " gzip  (js " + S.js.bytes + " in " + S.js.count + ", css " + S.css.bytes + " in " + S.css.count + ", html " + S.html.bytes + ")  largest: " + S.largest.join(", "));
  if (L && L.domContentLoadedMs) lines.push("LOAD (cold, " + RUNS + " runs, median (min-max) ms)  requests " + L.requests + "  DCL " + fmt(L.domContentLoadedMs) + "  load " + fmt(L.loadMs) + "  FCP " + (L.firstContentfulPaintMs ? fmt(L.firstContentfulPaintMs) : "n/a") + "  script " + fmt(L.scriptMs) + "  layout " + fmt(L.layoutMs) + "  style " + fmt(L.styleMs) + "  nodes " + L.nodes + "  heap " + fmt(L.jsHeapMB) + " MB");
  if (A && A.domNodes) {
    lines.push("DOM NODES    " + JSON.stringify(A.domNodes));
    lines.push("OPEN APP ms  " + Object.entries(A.openAppMs).map(([k, v]) => k + " first " + v.first + " warm " + fmt(v.warm)).join(" | "));
    lines.push("INDEX keystroke ms " + fmt(A.indexKeystrokeMs) + " worst " + A.indexKeystrokeWorstMs);
    Object.entries(A.vault).forEach(([n, v]) => lines.push("VAULT " + v.files + " files: items " + v.renderedItems + " nodes " + v.nodes + " | open " + fmt(v.openRenderMs) + " | search keystroke " + fmt(v.searchKeystrokeMs) + " | select click " + fmt(v.selectClickMs)));
    lines.push("CONTEXT MENU open ms " + fmt(A.contextMenuOpenMs) + (A.errors.length ? "  PAGE ERRORS: " + A.errors.join("; ") : ""));
  }
  if (F && F.clear) ["clear", "soft"].forEach((t) => lines.push("FRAMES " + t + " (scrolling " + F[t].scrollablePx + "px list, " + F[t].framesPerRun + " frames/run): interval mean " + fmt(F[t].frameIntervalMeanMs) + " ms, p95 " + fmt(F[t].frameIntervalP95Ms) + ", worst " + fmt(F[t].worstFrameMs) + ", frames >20ms " + fmt(F[t].framesOver20ms)));
  Object.entries(report).forEach(([k, v]) => { if (v && v.failed) lines.push("FAILED " + k + ": " + v.failed); });
  console.log(lines.join("\n"));
  if (OUT) { fs.writeFileSync(path.resolve(OUT), JSON.stringify(report, null, 2)); console.log("saved " + path.resolve(OUT)); }
})().catch((e) => { console.error(e); process.exit(1); });
