/** tests/unit/handoff-docs.test.js - the native compositor mapping must only name things the prototype really has (classes, ids, CSS tokens, files, colours). */
const assert = require("assert");
const fs = require("fs");
const path = require("path");

const ROOT = path.resolve(__dirname, "..", "..");
const PROTO = path.join(ROOT, "aurora-glass", "prototype");
const DOC = path.join(ROOT, "aurora-glass", "open-design-handoff", "native-compositor-mapping.md");

let passed = 0, failed = 0;
function test(name, fn) { try { fn(); passed++; console.log("  PASS:", name); } catch (e) { failed++; console.log("  FAIL:", name, "\n   ", e.message); } }

const doc = fs.readFileSync(DOC, "utf8");
const protoFiles = fs.readdirSync(PROTO).filter((f) => /\.(js|css|html)$/.test(f));
const source = protoFiles.map((f) => fs.readFileSync(path.join(PROTO, f), "utf8")).join("\n");
const css = protoFiles.filter((f) => f.endsWith(".css")).map((f) => fs.readFileSync(path.join(PROTO, f), "utf8")).join("\n");
const tokens = fs.readFileSync(path.join(PROTO, "tokens.css"), "utf8");
const squash = (s) => String(s).replace(/\s+/g, "").toLowerCase();
const spans = [...doc.matchAll(/`([^`\n]+)`/g)].map((m) => m[1]);
const uniq = (a) => [...new Set(a)];
const escRe = (s) => s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");

console.log("Mapping doc: every named thing exists");
test("the document has content and a status banner that says no native graphics exists", () => { assert.ok(doc.length > 5000); assert.ok(/No native graphics exists today/.test(doc)); });
test("every `.class` is used in the prototype", () => {
  const bad = uniq(spans.filter((s) => /^\.[a-z][\w-]*$/.test(s))).filter((s) => !new RegExp("(^|[^\\w-])" + escRe(s.slice(1)) + "($|[^\\w-])").test(source));
  assert.deepStrictEqual(bad, []);
});
test("every `#id` is used in the prototype", () => {
  const bad = uniq(spans.filter((s) => /^#[a-z][\w-]*$/.test(s))).filter((s) => !new RegExp("(^|[^\\w-])" + escRe(s.slice(1)) + "($|[^\\w-])").test(source));
  assert.deepStrictEqual(bad, []);
});
test("every `--token` is defined or used in the prototype CSS", () => {
  const bad = uniq(spans.filter((s) => /^--[\w-]+$/.test(s))).filter((s) => !css.includes(s));
  assert.deepStrictEqual(bad, []);
});
test("every `file.js` or `file.css` names a real prototype file, and every path names a real file", () => {
  const bad = uniq(spans.filter((s) => /^[\w-]+\.(js|css)$/.test(s))).filter((s) => !protoFiles.includes(s));
  assert.deepStrictEqual(bad, []);
  const badPaths = uniq(spans.filter((s) => /^(docs|tests|aurora-glass|kernel)\/[\w./-]+\.(md|js|css|c|json)$/.test(s))).filter((s) => !fs.existsSync(path.join(ROOT, s)));
  assert.deepStrictEqual(badPaths, []);
});
test("every `data-attribute=\"value\"` is present in the prototype", () => {
  const bad = uniq(spans.filter((s) => /^data-[\w-]+="[^"]+"$/.test(s))).filter((s) => !source.includes(s));
  assert.deepStrictEqual(bad, []);
});
test("the named global objects exist (AuroraStrings and data-i18n)", () => { assert.ok(/window\.AuroraStrings\s*=/.test(source)); assert.ok(source.includes("data-i18n")); });

console.log("Mapping doc: the colour table matches tokens.css");
const rows = [...doc.matchAll(/^\|\s*([a-z-]+)\s*\|\s*(#[0-9A-Fa-f]{6}|rgba\([^)]*\))\s*\|\s*(#[0-9A-Fa-f]{6}|rgba\([^)]*\))\s*\|/gm)];
test("the table was found and has the ten colours", () => assert.deepStrictEqual(rows.map((r) => r[1]), ["canvas", "surface-opaque", "surface-glass", "dock-glass", "ink", "ink-muted", "accent", "aura", "focus", "danger"]));
const lightBlock = tokens.slice(tokens.indexOf(":root,"), tokens.indexOf(':root[data-theme="dark"]'));
const darkBlock = tokens.slice(tokens.indexOf(':root[data-theme="dark"]'));
rows.forEach(([, name, light, dark]) => {
  test("colour " + name + " matches light and dark in tokens.css", () => {
    const find = (block, v) => { const m = block.match(new RegExp("--color-" + escRe(name) + ":\\s*([^;]+);")); return m && squash(m[1]) === squash(v); };
    assert.ok(find(lightBlock, light), "light " + light);
    assert.ok(find(darkBlock, dark), "dark " + dark);
  });
});

console.log("Mapping doc: geometry, type and motion numbers match tokens.css");
const tok = (n) => { const m = tokens.match(new RegExp(escRe(n) + ":\\s*([^;]+);")); return m ? m[1].trim() : null; };
test("radii, border, touch target and dock height", () => {
  assert.strictEqual(tok("--radius-control"), "14px"); assert.strictEqual(tok("--radius-card"), "20px"); assert.strictEqual(tok("--radius-sheet"), "24px"); assert.strictEqual(tok("--radius-sm"), "8px");
  assert.strictEqual(tok("--border-width"), "1px"); assert.strictEqual(tok("--touch-target"), "48px"); assert.strictEqual(tok("--dock-height"), "76px");
});
test("spacing and type scales named in the doc are the real ones", () => {
  assert.deepStrictEqual(["--space-1", "--space-2", "--space-3", "--space-4", "--space-5", "--space-6", "--space-7", "--space-8"].map(tok), ["4px", "8px", "12px", "16px", "24px", "32px", "48px", "64px"]);
  assert.deepStrictEqual(["--text-xs", "--text-sm", "--text-base", "--text-lg", "--text-xl", "--text-2xl", "--text-3xl", "--text-display"].map(tok), ["12px", "14px", "16px", "18px", "24px", "30px", "34px", "56px"]);
  assert.ok(/12, 14, 16, 18, 24, 30, 34, 56 px/.test(doc.replace(/`--text-[\w-]+` /g, "").replace(/\s+/g, " ")) || /`--text-xs` 12, `--text-sm` 14/.test(doc));
  assert.ok(/4, 8, 12, 16, 24, 32, 48, 64 px/.test(doc));
});
test("motion durations", () => { assert.strictEqual(tok("--motion-fast"), "120ms"); assert.strictEqual(tok("--motion-base"), "180ms"); assert.strictEqual(tok("--motion-slow"), "220ms"); assert.ok(/120 ms/.test(doc) && /180 ms/.test(doc) && /220 ms/.test(doc)); });
test("the Soft tier blur and the 1.2 s hold the doc quotes are the real values", () => {
  assert.ok(/--backdrop-blur:\s*blur\(14px\) saturate\(1\.15\)/.test(tokens) && doc.includes("blur(14px) saturate(1.15)"));
  assert.ok(/o\.ms \|\| 1200/.test(source) && doc.includes("1.2 s"));
});
test("the quoted behaviour limits are the real ones (500 ms long press, 10 px tolerance, 8 px margin, 12 steps, 600 ms autosave, Notes limits)", () => {
  const cm = fs.readFileSync(path.join(PROTO, "contextmenu-model.js"), "utf8"), nm = fs.readFileSync(path.join(PROTO, "notes-model.js"), "utf8"), nt = fs.readFileSync(path.join(PROTO, "notes.js"), "utf8"), vm = fs.readFileSync(path.join(PROTO, "vault-model.js"), "utf8");
  assert.ok(/MARGIN = 8, LONG_PRESS_MS = 500, MOVE_TOLERANCE = 10/.test(cm)); assert.ok(/AUTOSAVE_MS = 600/.test(nt)); assert.ok(/MAX_PLAN_STEPS = 12/.test(vm));
  assert.ok(/title: 120, body: 20000, notes: 200/.test(nm)); assert.ok(/200 notes, 120 characters of title, 20,000 characters of body/.test(doc));
});
test("the doc never claims a native implementation exists for a UI row", () => {
  const rowsUi = doc.split("\n").filter((l) => /^\| [A-Z]/.test(l) && /(not started|kernel|mock only)/.test(l));
  assert.ok(rowsUi.length >= 20);
  assert.ok(!/\|\s*(done|working|implemented|complete)\s*\|\s*$/im.test(doc));
});

console.log("\n" + passed + " passed, " + failed + " failed");
process.exit(failed ? 1 : 0);
