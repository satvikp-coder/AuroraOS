/** tests/unit/index.test.js - the DOM-free Index relevance model (aurora-glass/prototype/index-model.js). */
const assert = require("assert");
const path = require("path");
const M = require(path.resolve(__dirname, "..", "..", "aurora-glass", "prototype", "index-model.js"));
const T = M.TIER;

let passed = 0, failed = 0;
function test(name, fn) { try { fn(); passed++; console.log("  PASS:", name); } catch (e) { failed++; console.log("  FAIL:", name, "\n   ", e.message); } }

console.log("Index model: match tiers");
test("tiers are ordered exact < prefix < word-prefix < substring < keyword < fuzzy", () => assert.deepStrictEqual([T.EXACT, T.PREFIX, T.WORD_PREFIX, T.SUBSTRING, T.KEYWORD, T.FUZZY], [0, 1, 2, 3, 4, 5]));
test("exact beats prefix", () => { assert.strictEqual(M.matchTier("notes", "Notes"), T.EXACT); assert.strictEqual(M.matchTier("note", "Notes"), T.PREFIX); });
test("case, accents and extra spaces are ignored", () => { assert.strictEqual(M.matchTier("  NOTES ", "notes"), T.EXACT); assert.strictEqual(M.matchTier("cafe", "Café menu"), T.PREFIX); assert.strictEqual(M.matchTier("dark  mode", "Dark mode"), T.EXACT); });
test("word prefix: the query starts a later word", () => { assert.strictEqual(M.matchTier("recipe", "Dal recipe.txt"), T.WORD_PREFIX); assert.strictEqual(M.matchTier("txt", "Dal recipe.txt"), T.WORD_PREFIX); });
test("several typed words each start some word, in any order", () => { assert.strictEqual(M.matchTier("mode dark", "Dark mode"), T.WORD_PREFIX); assert.strictEqual(M.matchTier("rec dal", "Dal recipe.txt"), T.WORD_PREFIX); assert.strictEqual(M.matchTier("dark zzz", "Dark mode"), T.NONE); });
test("substring is weaker than word prefix", () => { assert.strictEqual(M.matchTier("ecip", "Dal recipe.txt"), T.SUBSTRING); });
test("keywords come after label matches", () => { assert.strictEqual(M.matchTier("multitasking", "Pages", ["multitasking"]), T.KEYWORD); assert.strictEqual(M.matchTier("zzz", "Pages", ["multitasking"]), T.NONE); });
test("an empty query matches everything at the same tier", () => { assert.strictEqual(M.matchTier("", "Anything"), T.PREFIX); assert.strictEqual(M.matchTier("   ", "x"), T.PREFIX); });
test("null/undefined input does not throw", () => { assert.strictEqual(M.matchTier(undefined, undefined), T.PREFIX); assert.strictEqual(M.matchTier("a", null, null), T.NONE); });

console.log("Index model: edit distance");
test("distance: identical, insert, delete, substitute, transposition", () => {
  assert.strictEqual(M.editDistance("clock", "clock"), 0); assert.strictEqual(M.editDistance("clok", "clock"), 1); assert.strictEqual(M.editDistance("clocks", "clock"), 1);
  assert.strictEqual(M.editDistance("clack", "clock"), 1); assert.strictEqual(M.editDistance("clcok", "clock"), 1);                    // a swap of two neighbours is one edit
});
test("distance larger cases and early exit", () => { assert.strictEqual(M.editDistance("kitten", "sitting"), 3); assert.ok(M.editDistance("abcdef", "uvwxyz", 1) > 1); assert.ok(M.editDistance("a", "abcdef", 2) > 2); });
test("distance with empty strings", () => { assert.strictEqual(M.editDistance("", "abc"), 3); assert.strictEqual(M.editDistance("abc", ""), 3); assert.strictEqual(M.editDistance("", ""), 0); });

console.log("Index model: conservative typo tolerance (app names)");
const fz = (q, n) => M.fuzzyMatch(q, n).ok;
test("finds common slips: missing, extra, wrong and swapped letters", () => {
  assert.ok(fz("calculater", "Calculator")); assert.ok(fz("clok", "Clock")); assert.ok(fz("vualt", "Vault")); assert.ok(fz("setings", "Settings")); assert.ok(fz("passprot", "Passport"));
});
test("needs at least 4 characters", () => { assert.ok(!fz("clk", "Clock")); assert.ok(!fz("cl", "Clock")); assert.ok(fz("nots", "Notes")); });
test("the first letter must agree", () => { assert.ok(!fz("xlock", "Clock")); assert.ok(!fz("lotes", "Notes")); });
test("2 edits are allowed only from 8 characters", () => { assert.ok(!fz("claok", "Clock")); assert.ok(fz("calcluater", "Calculator")); assert.ok(M.fuzzyMatch("calcluater", "Calculator").distance === 2); });
test("anything that already matches is not fuzzy (it would have ranked higher)", () => { assert.ok(!fz("calc", "Calculator")); assert.ok(!fz("clock", "Clock")); assert.ok(!fz("lock", "Clock")); });
test("unrelated words do not match", () => { ["banana", "photo", "weather", "music", "zebra", "settle"].forEach((q) => ["Notes", "Clock", "Vault", "Calculator", "Passport", "Trail", "Settings", "Files", "Pages"].forEach((n) => assert.ok(!fz(q, n), q + " vs " + n))); });
test("multi-word queries are never fuzzy", () => assert.ok(!fz("calculater app", "Calculator")));
test("a word of a longer name can match", () => assert.ok(fz("recoder", "Voice Recorder")));
test("distance is reported", () => { assert.strictEqual(M.fuzzyMatch("clok", "Clock").distance, 1); assert.strictEqual(M.fuzzyMatch("zzzz", "Clock").distance, -1); });

console.log("Index model: recency");
test("pushRecent puts the newest first, removes duplicates, and is bounded", () => {
  let l = []; ["a", "b", "c", "b"].forEach((k) => { l = M.pushRecent(l, k); }); assert.deepStrictEqual(l, ["b", "c", "a"]);
  let big = []; for (let i = 0; i < 50; i++) big = M.pushRecent(big, "k" + i); assert.strictEqual(big.length, M.MAX_RECENT); assert.strictEqual(big[0], "k49");
});
test("pushRecent ignores bad keys and does not mutate", () => { const l = ["a"]; const out = M.pushRecent(l, ""); assert.deepStrictEqual(out, ["a"]); assert.notStrictEqual(out, l); assert.deepStrictEqual(M.pushRecent(l, null), ["a"]); assert.deepStrictEqual(l, ["a"]); });
test("recencyOf is the position or Infinity", () => { assert.strictEqual(M.recencyOf(["x", "y"], "y"), 1); assert.strictEqual(M.recencyOf(["x"], "q"), Infinity); assert.strictEqual(M.recencyOf(undefined, "q"), Infinity); });

console.log("Index model: ordering");
const it = (key, rank, n, extra) => Object.assign({ key, rank, n }, extra || {});
test("a better tier always wins over recency", () => {
  const out = M.order([it("old-exact", T.EXACT, 0), it("recent-sub", T.SUBSTRING, 1)], ["recent-sub"]);
  assert.deepStrictEqual(out.map((x) => x.key), ["old-exact", "recent-sub"]);
});
test("inside a tier, the more recently used item comes first", () => {
  const out = M.order([it("a", T.PREFIX, 0), it("b", T.PREFIX, 1), it("c", T.PREFIX, 2)], ["c", "b"]);
  assert.deepStrictEqual(out.map((x) => x.key), ["c", "b", "a"]);
});
test("with no recency, source order decides; the order is stable and repeatable", () => {
  const items = [it("a", 1, 2), it("b", 1, 0), it("c", 1, 1)];
  const one = M.order(items, []).map((x) => x.key), two = M.order(items, []).map((x) => x.key);
  assert.deepStrictEqual(one, ["b", "c", "a"]); assert.deepStrictEqual(one, two);
});
test("fuzzy items rank last in their group even if they are the most recent", () => {
  const out = M.order([it("typo", T.FUZZY, 0, { fuzzy: true }), it("kw", T.KEYWORD, 1)], ["typo"]);
  assert.deepStrictEqual(out.map((x) => x.key), ["kw", "typo"]);
});
test("order does not mutate its input and tolerates empty input", () => { const items = [it("b", 2, 0), it("a", 1, 1)]; M.order(items, []); assert.strictEqual(items[0].key, "b"); assert.deepStrictEqual(M.order([], []), []); assert.deepStrictEqual(M.order(undefined), []); });
test("ties with no n fall back to input position", () => assert.deepStrictEqual(M.order([{ key: "x", rank: 1 }, { key: "y", rank: 1 }], []).map((v) => v.key), ["x", "y"]));

console.log("Index model: the default result");
test("defaultActive is the first non-fuzzy result", () => { assert.strictEqual(M.defaultActive([{ fuzzy: false }, { fuzzy: true }]), 0); assert.strictEqual(M.defaultActive([{ fuzzy: true }, { label: "x" }]), 1); });
test("defaultActive is -1 when every result is a typo suggestion or there are none (Enter must not open anything)", () => { assert.strictEqual(M.defaultActive([{ fuzzy: true }, { fuzzy: true }]), -1); assert.strictEqual(M.defaultActive([]), -1); assert.strictEqual(M.defaultActive(undefined), -1); });

console.log("\n" + passed + " passed, " + failed + " failed");
process.exit(failed ? 1 : 0);
