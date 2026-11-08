/** tests/unit/core.test.js — pure-model tests for aurora-glass/prototype/core.js (simulation models only). */
const assert = require("assert");
const path = require("path");
const Core = require(path.resolve(__dirname, "..", "..", "aurora-glass", "prototype", "core.js"));

let passed = 0, failed = 0;
function test(name, fn) {
  try { fn(); passed++; console.log("  PASS:", name); }
  catch (e) { failed++; console.log("  FAIL:", name, "\n   ", e.message); }
}
const mk = (extra) => Core.createPages(Object.assign({
  budgetMB: 1000,
  pages: [
    { id: "a", appId: "a", name: "A", space: "study", state: "running", memMB: 400 },
    { id: "b", appId: "b", name: "B", space: "study", state: "paused", memMB: 400 },
    { id: "c", appId: "c", name: "C", space: "work", state: "saved", memMB: 300 },
  ],
}, extra));

console.log("== Pages model ==");
test("saved pages hold no RAM", () => assert.strictEqual(mk().usedMB(), 800));
test("pause only works on running pages", () => {
  const p = mk();
  assert.deepStrictEqual(p.pause("b"), { ok: false, reason: "not-running" });
  assert.strictEqual(p.pause("a").ok, true);
  assert.strictEqual(p.get("a").state, "paused");
});
test("resume saved page rejected when it would exceed budget", () => {
  const p = mk();
  assert.deepStrictEqual(p.resume("c"), { ok: false, reason: "low-memory" });
  assert.strictEqual(p.get("c").state, "saved");
});
test("resume saved page succeeds after freeing memory", () => {
  const p = mk();
  const r = p.freeMemory();
  assert.strictEqual(r.freedMB, 400);
  assert.strictEqual(p.get("b").state, "saved");
  assert.strictEqual(p.resume("c").ok, true);
  assert.strictEqual(p.usedMB(), 700);
});
test("resuming a paused page needs no extra memory", () => assert.strictEqual(mk().resume("b").ok, true));
test("low-memory warning at 85%", () => {
  assert.strictEqual(mk({ budgetMB: 1000 }).usage().low, false);
  assert.strictEqual(mk({ budgetMB: 900 }).usage().low, true);
});
test("close removes page and frees memory", () => {
  const p = mk();
  assert.strictEqual(p.close("a").ok, true);
  assert.strictEqual(p.get("a"), null);
  assert.strictEqual(p.usedMB(), 400);
  assert.strictEqual(p.close("a").reason, "not-found");
});
test("open reuses existing page, rejects over budget", () => {
  const p = mk();
  assert.strictEqual(p.open("a", "A", "study", 400).id, "a");
  assert.strictEqual(p.list().length, 3);
  assert.deepStrictEqual(p.open("z", "Z", "study", 500), { ok: false, reason: "low-memory" });
  assert.strictEqual(p.open("y", "Y", "study", 100).ok, true);
});
test("unknown ids fail cleanly", () => {
  const p = mk();
  ["pause", "resume", "save", "close"].forEach((m) => assert.strictEqual(p[m]("nope").reason, "not-found"));
});
test("onChange fires on mutation but not on rejected operations", () => {
  const p = mk(); let n = 0; p.onChange(() => n++);
  p.pause("b"); p.resume("c"); assert.strictEqual(n, 0);
  p.pause("a"); assert.strictEqual(n, 1);
});
test("list() returns copies", () => {
  const p = mk(); p.list()[0].state = "saved"; assert.strictEqual(p.get("a").state, "running");
});

console.log("== Quick settings model ==");
test("defaults and ranges", () => {
  const q = Core.createQuickSettings();
  assert.strictEqual(q.get("brightness"), 100);
  assert.strictEqual(q.set("brightness", 150).value, 100);
  assert.strictEqual(q.set("volume", -4).value, 0);
  assert.strictEqual(q.set("volume", 33.6).value, 34);
});
test("rejects invalid values and unknown settings", () => {
  const q = Core.createQuickSettings();
  assert.strictEqual(q.set("wifi", "yes").reason, "invalid-value");
  assert.strictEqual(q.set("brightness", NaN).reason, "invalid-value");
  assert.strictEqual(q.set("brightness", "50").reason, "invalid-value");
  assert.strictEqual(q.set("appearance", "sepia").reason, "invalid-value");
  assert.strictEqual(q.set("auraMode", "root").reason, "invalid-value");
  assert.strictEqual(q.set("__proto__", true).reason, "unknown-setting");
  assert.strictEqual(q.set("constructor", true).reason, "unknown-setting");
  assert.strictEqual(q.get("wifi"), true);
});
test("toggle only for booleans", () => {
  const q = Core.createQuickSettings();
  assert.strictEqual(q.toggle("focus").value, true);
  assert.strictEqual(q.toggle("brightness").reason, "not-a-toggle");
  assert.strictEqual(q.toggle("nope").reason, "not-a-toggle");
});
test("onChange only fires on real change", () => {
  const q = Core.createQuickSettings(); const seen = [];
  q.onChange((k, v, o) => seen.push([k, v, o]));
  q.set("focus", false); q.set("focus", true);
  assert.deepStrictEqual(seen, [["focus", true, false]]);
});
test("privacy indicators reflect sessions", () => {
  const q = Core.createQuickSettings();
  assert.deepStrictEqual(q.privacy({ camera: true }), { microphone: false, camera: true, location: false });
});

console.log(`\n== Summary ==\nPassed: ${passed}\nFailed: ${failed}`);
process.exit(failed ? 1 : 0);
