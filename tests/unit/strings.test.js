/** tests/unit/strings.test.js - the language-ready strings: model, catalog rules, and a scan of the prototype source for missing or unused keys. */
const assert = require("assert");
const fs = require("fs");
const path = require("path");
const PROTO = path.resolve(__dirname, "..", "..", "aurora-glass", "prototype");
const M = require(path.join(PROTO, "strings-model.js"));
const C = require(path.join(PROTO, "strings-catalog.js"));

let passed = 0, failed = 0;
function test(name, fn) { try { fn(); passed++; console.log("  PASS:", name); } catch (e) { failed++; console.log("  FAIL:", name, "\n   ", e.message); } }
const make = (locale) => { const s = M.create({ catalogs: C.locales }); if (locale) s.setLocale(locale); return s; };
const EN = C.locales.en.messages;
const placeholders = (s) => (String(s).match(/\{\w+\}/g) || []).sort();

console.log("Strings: lookup and formatting");
test("English returns the catalog text, with {name} filled in", () => { const s = make(); assert.strictEqual(s.t("dock.home"), "Home"); assert.strictEqual(s.t("welcome.stepLabel", { n: 2, total: 3 }), "Step 2 of 3"); });
test("a number 0 is a real value, not a missing one", () => assert.strictEqual(make().t("welcome.stepLabel", { n: 0, total: 3 }), "Step 0 of 3"));
test("a missing parameter stays visible as {name} and is recorded", () => { const s = make(); assert.strictEqual(s.t("shortcuts.onThisScreen"), "On this screen: {group}"); assert.ok(s.missing().some((m) => /shortcuts\.onThisScreen.*\{group\}/.test(m))); });
test("an unknown key returns the key itself and is recorded", () => { const s = make(); assert.strictEqual(s.t("no.such.key"), "no.such.key"); assert.deepStrictEqual(s.missing(), ["no.such.key"]); s.clearMissing(); assert.deepStrictEqual(s.missing(), []); });
test("extra parameters are ignored", () => assert.strictEqual(make().t("dock.home", { x: 1 }), "Home"));
test("text is never interpreted as markup or as a format: values are inserted verbatim", () => assert.strictEqual(make().t("shortcuts.none", { query: "<b>{n}</b>$&" }), "No shortcut matches “<b>{n}</b>$&”."));
test("format() reports what it could not fill", () => assert.deepStrictEqual(M.format("a {x} b {y}", { x: 1 }), { text: "a 1 b {y}", unfilled: ["y"] }));

console.log("Strings: plurals");
test("English one/other", () => { const s = make(); assert.strictEqual(s.tn("index.results", 1), "1 result"); assert.strictEqual(s.tn("index.results", 0), "0 results"); assert.strictEqual(s.tn("index.results", 2), "2 results"); assert.strictEqual(s.tn("index.results", 21), "21 results"); });
test("a placeholder locale uses ENGLISH rules for English text (Hindi would call 0 'one')", () => { const s = make("hi"); assert.strictEqual(s.tn("index.results", 0), "0 results"); assert.strictEqual(s.tn("index.results", 1), "1 result"); });
test("a missing plural key falls back to .other, and an unknown plural key is recorded", () => { const s = make(); assert.strictEqual(s.tn("index.noResults", 5), "No results"); s.tn("nope", 1); assert.ok(s.missing().includes("nope")); });

console.log("Strings: fallback and locales");
test("a placeholder locale (hi, gu) shows English for every key", () => ["hi", "gu"].forEach((l) => { const s = make(l); Object.keys(EN).forEach((k) => assert.strictEqual(s.t(k, { n: 1, total: 3, group: "g", query: "q" }), make().t(k, { n: 1, total: 3, group: "g", query: "q" }), l + " " + k)); }));
test("an own translation wins, and a missing one falls back to English", () => {
  const catalogs = { en: { status: "verified", messages: { a: "A", b: "B" } }, fr: { status: "draft", messages: { a: "Ah" } } };
  const s = M.create({ catalogs }); s.setLocale("fr"); assert.strictEqual(s.t("a"), "Ah"); assert.strictEqual(s.t("b"), "B");
});
test("a regional locale falls back to its base language, then English", () => {
  const catalogs = { en: { status: "verified", messages: { a: "A", b: "B" } }, fr: { status: "draft", messages: { a: "Ah" } }, "fr-CA": { status: "draft", messages: { b: "Bé" } } };
  const s = M.create({ catalogs }); s.setLocale("fr-CA"); assert.strictEqual(s.t("b"), "Bé"); assert.strictEqual(s.t("a"), "Ah");
});
test("setLocale refuses an unknown language and keeps the current one", () => { const s = make(); assert.strictEqual(s.setLocale("xx"), false); assert.strictEqual(s.locale(), "en"); assert.strictEqual(s.setLocale("hi"), true); assert.strictEqual(s.locale(), "hi"); });
test("prototype-ish names are not languages", () => { const s = make(); ["__proto__", "constructor", "toString", ""].forEach((l) => assert.strictEqual(s.setLocale(l), false, l)); });
test("locales() lists every locale with its status", () => assert.deepStrictEqual(make().locales().map((l) => [l.id, l.status]), [["en", "verified"], ["hi", "placeholder"], ["gu", "placeholder"], ["en-XA", "test-only"]]));

console.log("Strings: honesty rules for the catalog");
test("only English is verified", () => assert.deepStrictEqual(Object.keys(C.locales).filter((l) => C.locales[l].status === "verified"), ["en"]));
test("a placeholder catalog is EMPTY: no unreviewed or machine translation can slip in without changing its status", () => Object.keys(C.locales).filter((l) => C.locales[l].status === "placeholder").forEach((l) => assert.deepStrictEqual(C.locales[l].messages, {}, l)));
test("hi and gu are placeholders", () => { assert.strictEqual(C.locales.hi.status, "placeholder"); assert.strictEqual(C.locales.gu.status, "placeholder"); });
test("any non-English catalog only uses keys English has, with the same {placeholders}", () => Object.keys(C.locales).filter((l) => l !== "en").forEach((l) => Object.keys(C.locales[l].messages).forEach((k) => { assert.ok(k in EN, l + " has an orphan key " + k); assert.deepStrictEqual(placeholders(C.locales[l].messages[k]), placeholders(EN[k]), l + " " + k); })));
test("English messages are non-empty strings and plural keys come in .one/.other pairs", () => {
  Object.keys(EN).forEach((k) => assert.ok(typeof EN[k] === "string" && EN[k].trim(), k));
  Object.keys(EN).filter((k) => k.endsWith(".one")).forEach((k) => assert.ok(EN[k.replace(/\.one$/, ".other")], k + " needs .other"));
});
test("isVerified is true only for English", () => { const s = make(); assert.ok(s.isVerified("en")); ["hi", "gu", "en-XA", "zz"].forEach((l) => assert.ok(!s.isVerified(l), l)); });

console.log("Strings: pseudo-locale for long-string tests");
test("pseudo text is accented, bracketed and at least 40% longer", () => Object.keys(EN).forEach((k) => {
  const p = M.pseudo(EN[k]); const base = EN[k].replace(/\{\w+\}/g, "");
  assert.ok(p.startsWith("[") && p.endsWith("]"), k); assert.ok(p.length >= EN[k].length + Math.ceil(base.length * 0.4), k + " " + p.length + " vs " + EN[k].length);
  assert.ok(!/[A-Za-z]/.test(p.replace(/\{\w+\}/g, "")), k + " still has plain ASCII letters");
}));
test("pseudo keeps {placeholders} intact so they still fill", () => { const s = make("en-XA"); assert.ok(/^\[.+ 2 .+ 3 .+\]$/.test(s.t("welcome.stepLabel", { n: 2, total: 3 }))); assert.deepStrictEqual(placeholders(M.pseudo("a {n} b {total}")), ["{n}", "{total}"]); });
test("pseudo adds spaces, not one long word, so it wraps like real text", () => assert.ok(M.pseudo("Skip tour").split(" ").length >= 3));
test("pseudo of empty or non-string input is safe", () => { assert.strictEqual(M.pseudo(""), ""); assert.strictEqual(M.pseudo(undefined), ""); assert.strictEqual(M.pseudo(null), ""); });
test("the pseudo-locale resolves every English key, with plurals", () => { const s = make("en-XA"); Object.keys(EN).forEach((k) => assert.notStrictEqual(s.t(k, { n: 1, total: 3, group: "g", query: "q" }), k, k)); assert.ok(/1/.test(s.tn("index.results", 1))); assert.ok(s.missing().length === 0, s.missing().join(",")); });

console.log("Strings: the prototype source and the catalog agree");
const files = fs.readdirSync(PROTO).filter((f) => /\.(js|html)$/.test(f) && !/^strings-catalog\.js$/.test(f));
const stripComments = (f, s) => (f.endsWith(".html") ? s.replace(/<!--[\s\S]*?-->/g, "") : s.replace(/\/\*[\s\S]*?\*\//g, "").replace(/^\s*\/\/.*$/gm, ""));        // examples in comments are not usages
const src = files.map((f) => ({ f, s: stripComments(f, fs.readFileSync(path.join(PROTO, f), "utf8")) }));
const used = new Set();
src.forEach(({ s }) => {
  for (const m of s.matchAll(/\b(?:t|tn|AuroraStrings\.t|AuroraStrings\.tn|S\.t|S\.tn)\(\s*"([a-z][A-Za-z0-9.]*)"/g)) used.add(m[1]);
  for (const m of s.matchAll(/data-i18n="([^"]+)"/g)) used.add(m[1]);
  for (const m of s.matchAll(/data-i18n-attr="([^"]+)"/g)) m[1].split(";").forEach((p) => used.add(p.slice(p.indexOf(":") + 1).trim()));
});
const stripPlural = (k) => k.replace(/\.(one|other)$/, "");
const enBase = new Set(Object.keys(EN).map(stripPlural));
test("every key the code or HTML uses exists in English (plural bases count)", () => { const bad = [...used].filter((k) => !(k in EN) && !enBase.has(k)); assert.deepStrictEqual(bad, []); });
test("every English key is used somewhere (no stale strings)", () => { const usedBase = new Set([...used].map(stripPlural)); const stale = Object.keys(EN).filter((k) => !usedBase.has(stripPlural(k))); assert.deepStrictEqual(stale, []); });
test("scripts load in an order that works: catalog, model, glue, all before app.js", () => {
  const html = src.find((x) => x.f === "index.html").s; const at = (n) => html.indexOf('src="' + n + '"');
  assert.ok(at("strings-catalog.js") > 0 && at("strings-catalog.js") < at("strings-model.js") && at("strings-model.js") < at("strings.js") && at("strings.js") < at("app.js") && at("strings.js") < at("welcome.js") && at("strings.js") < at("shortcuts.js"));
});
test("the migrated English text is exactly what the interface showed before (welcome and shortcut chrome)", () => {
  assert.strictEqual(EN["welcome.title"], "Welcome to Aurora Glass"); assert.strictEqual(EN["shortcuts.title"], "Keyboard shortcuts"); assert.strictEqual(EN["dock.ask.aria"], "Ask Aura / Search"); assert.strictEqual(EN["index.noResults"], "No results");
});

console.log("\n" + passed + " passed, " + failed + " failed");
process.exit(failed ? 1 : 0);
