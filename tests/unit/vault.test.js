/** tests/unit/vault.test.js — the DOM-free Vault model (aurora-glass/prototype/vault-model.js). */
const assert = require("assert");
const path = require("path");
const M = require(path.resolve(__dirname, "..", "..", "aurora-glass", "prototype", "vault-model.js"));

let passed = 0, failed = 0;
function test(name, fn) { try { fn(); passed++; console.log("  PASS:", name); } catch (e) { failed++; console.log("  FAIL:", name, "\n   ", e.message); } }

const F = (id, name, kind, space, size, modified) => ({ id, name, kind, space, size, modified });
const FILES = [
  F("a", "banana.txt", "text", "study", 900, 300), F("b", "Apple.md", "note", "work", 5000, 100), F("c", "cherry 10.csv", "table", "work", 120000, 200),
  F("d", "cherry 2.csv", "table", "personal", 120000, 50), F("e", "diagram.svg", "image", "projects", 48200, 400),
];
const ids = (l) => l.map((f) => f.id).join(",");

console.log("== sort ==");
test("by name, ascending and descending, case-insensitive and natural (2 before 10)", () => {
  assert.strictEqual(ids(M.sortFiles(FILES, "name", "asc")), "b,a,d,c,e");
  assert.strictEqual(ids(M.sortFiles(FILES, "name", "desc")), "e,c,d,a,b");
});
test("by size and by modified, both directions; ties fall back to name so the order is total", () => {
  assert.strictEqual(ids(M.sortFiles(FILES, "size", "asc")), "a,b,e,d,c");
  assert.strictEqual(ids(M.sortFiles(FILES, "size", "desc")), "c,d,e,b,a");
  assert.strictEqual(ids(M.sortFiles(FILES, "modified", "asc")), "d,b,c,a,e");
  assert.strictEqual(ids(M.sortFiles(FILES, "modified", "desc")), "e,a,c,b,d");
});
test("bad key or direction fall back to name ascending; the input is not mutated; bad lists are empty", () => {
  const copy = JSON.stringify(FILES);
  assert.strictEqual(ids(M.sortFiles(FILES, "__proto__", "sideways")), "b,a,d,c,e");
  assert.strictEqual(ids(M.sortFiles(FILES, null)), "b,a,d,c,e");
  assert.strictEqual(JSON.stringify(FILES), copy);
  assert.deepStrictEqual(M.sortFiles(null, "name", "asc"), []);
});

console.log("== filter ==");
test("search matches name, kind label and Space, case-insensitively; Space filter combines with it", () => {
  assert.strictEqual(ids(M.filterFiles(FILES, { query: "CHERRY" })), "c,d");
  assert.strictEqual(ids(M.filterFiles(FILES, { query: "table" })), "c,d");
  assert.strictEqual(ids(M.filterFiles(FILES, { query: "projects" })), "e");
  assert.strictEqual(ids(M.filterFiles(FILES, { space: "work" })), "b,c");
  assert.strictEqual(ids(M.filterFiles(FILES, { space: "work", query: "cherry" })), "c");
  assert.strictEqual(ids(M.filterFiles(FILES, { space: "work", query: "nothing" })), "");
  assert.strictEqual(ids(M.filterFiles(FILES, { space: "all", query: "  " })), "a,b,c,d,e");
});
test("bad options never hide files by accident", () => {
  assert.strictEqual(M.filterFiles(FILES, null).length, 5);
  assert.strictEqual(M.filterFiles(FILES, { query: 5, space: 7 }).length, 5);
  assert.deepStrictEqual(M.filterFiles(undefined, {}), []);
});

console.log("== formatting ==");
test("sizes and kinds", () => {
  assert.strictEqual(M.formatSize(0), "0 B");
  assert.strictEqual(M.formatSize(860), "860 B");
  assert.strictEqual(M.formatSize(4210), "4.1 KB");
  assert.strictEqual(M.formatSize(1048576 * 2.5), "2.5 MB");
  for (const bad of [-5, NaN, Infinity, "9", null, undefined]) assert.strictEqual(M.formatSize(bad), bad === Infinity ? "0 B" : "0 B");
  assert.strictEqual(M.kindLabel("image"), "Image");
  assert.strictEqual(M.kindLabel("__proto__"), "File");
  assert.strictEqual(M.kindLabel("constructor"), "File");
  assert.strictEqual(M.kindIcon("nope"), "file");
});

console.log("== range selection and batching ==");
test("rangeIds is inclusive, order-independent and safe with unknown ids", () => {
  const v = ["a", "b", "c", "d", "e"];
  assert.deepStrictEqual(M.rangeIds(v, "b", "d"), ["b", "c", "d"]);
  assert.deepStrictEqual(M.rangeIds(v, "d", "b"), ["b", "c", "d"]);
  assert.deepStrictEqual(M.rangeIds(v, "c", "c"), ["c"]);
  assert.deepStrictEqual(M.rangeIds(v, "zz", "c"), ["c"]);
  assert.deepStrictEqual(M.rangeIds(v, "a", "zz"), []);
});
test("chunk never drops or duplicates an id and respects the plan limit", () => {
  assert.strictEqual(M.MAX_PLAN_STEPS, 12);
  const ids30 = Array.from({ length: 30 }, (_, i) => "f" + i);
  const parts = M.chunk(ids30, M.MAX_PLAN_STEPS);
  assert.deepStrictEqual(parts.map((p) => p.length), [12, 12, 6]);
  assert.deepStrictEqual([].concat(...parts), ids30);
  assert.deepStrictEqual(M.chunk(["a"], 12), [["a"]]);
  assert.deepStrictEqual(M.chunk([], 12), []);
  assert.deepStrictEqual(M.chunk(["a", "b", "c"], 0), [["a", "b", "c"]]);   // bad size falls back to the limit
});

console.log("== payloads ==");
test("one file is a single action; several are a plan; arguments are exactly id and destination", () => {
  assert.deepStrictEqual(M.movePayload(["a"], "work"), { type: "file.move", arguments: { id: "a", destination: "work" } });
  assert.deepStrictEqual(M.movePayload(["a", "b"], "work"), { steps: [{ type: "file.move", arguments: { id: "a", destination: "work" } }, { type: "file.move", arguments: { id: "b", destination: "work" } }] });
  assert.deepStrictEqual(M.deletePayload(["a"]), { type: "file.delete", arguments: { id: "a" } });
  assert.strictEqual(M.deletePayload(["a", "b", "c"]).steps.length, 3);
});
test("movable drops files already in the destination", () => {
  assert.strictEqual(ids(M.movable(FILES, "work")), "a,d,e");
  assert.strictEqual(ids(M.movable(FILES, "elsewhere")), "a,b,c,d,e");
});

console.log("== summarize (truthful results) ==");
const ex = (c) => "E(" + c + ")";
test("full success, cancel, block, nothing ran", () => {
  const names = ["A", "B"];
  assert.deepStrictEqual(M.summarize("move", "Moved", { ok: true, steps: [{ index: 0, status: "done" }, { index: 1, status: "done" }] }, names, ex), { ok: true, tone: "ok", text: "Moved 2 files." });
  assert.strictEqual(M.summarize("move", "Moved", { ok: false, cancelled: true }, names, ex).text, "Cancelled. Nothing was changed.");
  assert.strictEqual(M.summarize("move", "Moved", { ok: false, blocked: true, reason: "permission-denied" }, names, ex).ok, false);
  const none = M.summarize("move", "Moved", { ok: false, reason: "busy" }, names, ex);
  assert.strictEqual(none.ok, false);
  assert.match(none.text, /Could not move: E\(busy\)/);
});
test("partial failure names the file and the reason, counts what did not run, and is never ok", () => {
  const r = M.summarize("move", "Moved", { ok: false, steps: [{ index: 0, status: "done" }, { index: 1, status: "failed", error: "no-such-file" }] }, ["A", "B", "C"], ex);
  assert.strictEqual(r.ok, false);
  assert.match(r.text, /Moved 1 of 3 files/);
  assert.match(r.text, /“B” \(E\(no-such-file\)\)/);
  assert.match(r.text, /1 did not run/);
});
test("a reason that ends in a full stop does not produce '.).'", () => {
  const r = M.summarize("move", "Moved", { ok: false, steps: [{ index: 0, status: "failed", error: "x" }] }, ["A"], () => "Simulated failure (developer option).");
  assert.ok(r.text.includes("(Simulated failure (developer option))"), r.text);
  assert.ok(!r.text.includes(".)"), r.text);
});
test("a result with every step 'done' but fewer steps than files is not success", () => {
  const r = M.summarize("delete", "Deleted", { ok: true, steps: [{ index: 0, status: "done" }] }, ["A", "B"], ex);
  assert.strictEqual(r.ok, false);
});
test("hostile file names stay plain strings (no markup interpretation happens in the model)", () => {
  const r = M.summarize("move", "Moved", { ok: false, steps: [{ index: 0, status: "failed", error: "x" }] }, ["<img src=x onerror=1>"], ex);
  assert.ok(r.text.includes("<img src=x onerror=1>"));
  assert.strictEqual(typeof r.text, "string");
});

console.log(`\nVault model: ${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
