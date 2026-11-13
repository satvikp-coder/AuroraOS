/** tests/unit/contextmenu.test.js - the DOM-free context menu model (aurora-glass/prototype/contextmenu-model.js). */
const assert = require("assert");
const path = require("path");
const M = require(path.resolve(__dirname, "..", "..", "aurora-glass", "prototype", "contextmenu-model.js"));

let passed = 0, failed = 0;
function test(name, fn) { try { fn(); passed++; console.log("  PASS:", name); } catch (e) { failed++; console.log("  FAIL:", name, "\n   ", e.message); } }
const inside = (p, vw, vh, m) => p.left >= m && p.top >= m && p.left + p.width <= vw - m && p.top + p.height <= vh - m;

console.log("Context menu model: placement");
test("opens at the point when there is room", () => { const p = M.place(100, 120, 200, 150, 1000, 800); assert.deepStrictEqual([p.left, p.top, p.flippedX, p.flippedY], [100, 120, false, false]); });
test("flips left when it would pass the right edge", () => { const p = M.place(900, 100, 200, 150, 1000, 800); assert.strictEqual(p.left, 700); assert.ok(p.flippedX); });
test("flips up when it would pass the bottom edge", () => { const p = M.place(100, 700, 200, 150, 1000, 800); assert.strictEqual(p.top, 550); assert.ok(p.flippedY); });
test("flips both ways in the bottom-right corner", () => { const p = M.place(990, 790, 200, 150, 1000, 800); assert.ok(p.flippedX && p.flippedY); assert.ok(inside(p, 1000, 800, 8)); });
test("clamps a point outside the viewport (negative, huge, NaN) back inside", () => {
  [[-50, -50], [5000, 5000], [NaN, NaN], [undefined, undefined], [Infinity, -Infinity]].forEach(([x, y]) => assert.ok(inside(M.place(x, y, 200, 150, 1000, 800), 1000, 800, 8), x + "," + y));
});
test("a flip that would leave the left/top edge is clamped to the margin", () => { const p = M.place(60, 60, 400, 300, 380, 280); assert.ok(inside(p, 380, 280, 8)); });
test("shrinks to fit a tiny viewport and reports that it scrolls", () => {
  const p = M.place(10, 10, 300, 900, 360, 640); assert.ok(p.scrolls); assert.strictEqual(p.height, 624); assert.ok(inside(p, 360, 640, 8)); assert.strictEqual(p.maxHeight, 624);
});
test("a menu that fits does not report scrolling", () => assert.strictEqual(M.place(10, 10, 200, 100, 360, 640).scrolls, false));
test("the margin is configurable and zero is honoured", () => { const p = M.place(-5, -5, 100, 100, 300, 300, 0); assert.deepStrictEqual([p.left, p.top], [0, 0]); });
test("every phone and desktop corner stays inside for a 224x280 menu", () => {
  [[360, 640], [390, 844], [412, 915], [768, 1024], [1024, 768], [1280, 800], [1440, 900]].forEach(([w, h]) => {
    [[0, 0], [w, 0], [0, h], [w, h], [w / 2, h / 2], [w - 1, h / 2], [w / 2, h - 1]].forEach(([x, y]) => assert.ok(inside(M.place(x, y, 224, 280, w, h), w, h, 8), w + "x" + h + " at " + x + "," + y));
  });
});

console.log("Context menu model: items");
const a = () => {};
test("normalise drops falsy entries, items without a label, and items without an action", () => {
  const out = M.normalise([null, undefined, false, { label: "A", action: a }, { label: "", action: a }, { label: "  ", action: a }, { label: "NoAction" }, { label: "B", action: a }]);
  assert.deepStrictEqual(out.map((x) => x.label), ["A", "B"]);
});
test("normalise keeps a disabled item that has no action (so its reason can be read)", () => assert.strictEqual(M.normalise([{ label: "Move", disabled: true, reason: "Blocked" }]).length, 1));
test("normalise collapses repeated and leading/trailing separators", () => {
  const out = M.normalise([{ separator: true }, { label: "A", action: a }, { separator: true }, { separator: true }, { label: "B", action: a }, { separator: true }]);
  assert.deepStrictEqual(out.map((x) => (x.separator ? "-" : x.label)), ["A", "-", "B"]);
});
test("normalise tolerates non-arrays", () => { assert.deepStrictEqual(M.normalise(null), []); assert.deepStrictEqual(M.normalise("x"), []); assert.deepStrictEqual(M.normalise({}), []); });

console.log("Context menu model: keyboard");
const L = [{ label: "Open" }, { separator: true }, { label: "Move" }, { label: "Delete" }];
test("firstIndex / lastIndex skip separators", () => { assert.strictEqual(M.firstIndex([{ separator: true }, { label: "A" }]), 1); assert.strictEqual(M.lastIndex([{ label: "A" }, { separator: true }]), 0); assert.strictEqual(M.firstIndex([]), -1); assert.strictEqual(M.lastIndex([{ separator: true }]), -1); });
test("step moves down and up, skipping the separator", () => { assert.strictEqual(M.step(L, 0, 1), 2); assert.strictEqual(M.step(L, 2, -1), 0); });
test("step wraps at both ends", () => { assert.strictEqual(M.step(L, 3, 1), 0); assert.strictEqual(M.step(L, 0, -1), 3); });
test("step from nothing focused goes to the first or last item", () => { assert.strictEqual(M.step(L, -1, 1), 0); assert.strictEqual(M.step(L, -1, -1), 3); });
test("step with only separators or no items is -1", () => { assert.strictEqual(M.step([{ separator: true }], 0, 1), -1); assert.strictEqual(M.step([], 0, 1), -1); });
test("typeahead finds the next label starting with the text, case-insensitively, after the current item", () => { assert.strictEqual(M.typeahead(L, 0, "m"), 2); assert.strictEqual(M.typeahead(L, 0, "DEL"), 3); });
test("typeahead wraps and cycles on a repeated letter", () => {
  const two = [{ label: "Save" }, { label: "Select" }, { label: "Share" }];
  assert.strictEqual(M.typeahead(two, 0, "s"), 1); assert.strictEqual(M.typeahead(two, 1, "s"), 2); assert.strictEqual(M.typeahead(two, 2, "s"), 0); assert.strictEqual(M.typeahead(two, 0, "ss"), 1);
});
test("typeahead returns -1 for no match or empty text", () => { assert.strictEqual(M.typeahead(L, 0, "z"), -1); assert.strictEqual(M.typeahead(L, 0, ""), -1); assert.strictEqual(M.typeahead(L, 0, undefined), -1); });
test("typeahead never lands on a separator", () => assert.strictEqual(M.typeahead([{ separator: true, label: "mm" }, { label: "Move" }], -1, "m"), 1));

console.log("Context menu model: long press");
function fakeTimers() { let id = 0; const t = new Map(); return { setTimeout: (fn, ms) => { t.set(++id, { fn, ms }); return id; }, clearTimeout: (i) => t.delete(i), fire() { [...t.entries()].forEach(([i, x]) => { t.delete(i); x.fn(); }); }, count: () => t.size, ms: () => [...t.values()][0] && [...t.values()][0].ms }; }
test("a press that stays put fires after the delay at the start point", () => {
  const f = fakeTimers(), lp = M.longPress({ setTimeout: f.setTimeout, clearTimeout: f.clearTimeout }); let at = null;
  lp.start(40, 50, (x, y) => { at = [x, y]; }); assert.ok(lp.pending()); assert.strictEqual(f.ms(), M.LONG_PRESS_MS); lp.move(43, 52); f.fire(); assert.deepStrictEqual(at, [40, 50]); assert.ok(lp.fired()); assert.ok(!lp.pending());
});
test("moving more than the tolerance (a scroll) cancels it", () => {
  const f = fakeTimers(), lp = M.longPress({ setTimeout: f.setTimeout, clearTimeout: f.clearTimeout }); let n = 0;
  lp.start(0, 0, () => { n++; }); lp.move(0, M.MOVE_TOLERANCE + 1); assert.ok(!lp.pending()); f.fire(); assert.strictEqual(n, 0); assert.ok(!lp.fired());
});
test("releasing early (a tap) cancels it", () => {
  const f = fakeTimers(), lp = M.longPress({ setTimeout: f.setTimeout, clearTimeout: f.clearTimeout }); let n = 0;
  lp.start(0, 0, () => { n++; }); lp.end(); f.fire(); assert.strictEqual(n, 0); assert.strictEqual(f.count(), 0);
});
test("starting again replaces the pending press (no double menu)", () => {
  const f = fakeTimers(), lp = M.longPress({ setTimeout: f.setTimeout, clearTimeout: f.clearTimeout }); let n = 0;
  lp.start(0, 0, () => { n++; }); lp.start(5, 5, () => { n++; }); assert.strictEqual(f.count(), 1); f.fire(); assert.strictEqual(n, 1);
});
test("reset clears the fired flag", () => { const f = fakeTimers(), lp = M.longPress({ setTimeout: f.setTimeout, clearTimeout: f.clearTimeout }); lp.start(0, 0, () => {}); f.fire(); assert.ok(lp.fired()); lp.reset(); assert.ok(!lp.fired()); });

console.log("Context menu model: keyboard anchor");
test("anchorFor puts a keyboard-opened menu at the start of the row, near its bottom, and tolerates missing input", () => {
  assert.deepStrictEqual(M.anchorFor({ left: 100, bottom: 300, width: 400, height: 80 }), { x: 124, y: 292 });
  assert.deepStrictEqual(M.anchorFor({ left: 10, bottom: 50, width: 20, height: 8 }), { x: 20, y: 48 });
  assert.deepStrictEqual(M.anchorFor(null), { x: 0, y: 0 });
});

console.log("\n" + passed + " passed, " + failed + " failed");
process.exit(failed ? 1 : 0);
