/** tests/unit/trail.test.js — the DOM-free Trail model (aurora-glass/prototype/trail-model.js). */
const assert = require("assert");
const path = require("path");
const M = require(path.resolve(__dirname, "..", "..", "aurora-glass", "prototype", "trail-model.js"));

let passed = 0, failed = 0;
function test(name, fn) { try { fn(); passed++; console.log("  PASS:", name); } catch (e) { failed++; console.log("  FAIL:", name, "\n   ", e.message); } }

let n = 0;
function rc(over) {
  n++;
  const o = Object.assign({ id: "r" + n, at: 1000 + n, type: "app.open", risk: "S0", status: "done", undoState: "available", proposalId: "p" + n, stepIndex: 0, source: "Aura", requestText: null, error: null, errorText: null,
    describe: { title: "Open Notes", kind: "App", icon: "notes" }, action: { type: "app.open", arguments: { app: "notes" } } }, over || {});
  return o;
}
const NOW = new Date(2026, 10, 12, 15, 0, 0).getTime();

console.log("== undoInfo ==");
test("each undo state gives words, and only 'available' is available", () => {
  assert.strictEqual(M.undoInfo(rc({ undoState: "available" })).state, "available");
  assert.strictEqual(M.undoInfo(rc({ undoState: "used" })).why, "Already undone.");
  assert.strictEqual(M.undoInfo(rc({ undoState: "in-progress" })).label, "Undoing…");
  assert.match(M.undoInfo(rc({ undoState: "failed" })).why, /^Undo failed/);
  assert.strictEqual(M.undoInfo(rc({ undoState: "unavailable", type: "search.query" })).why, "Can’t be undone: searching changes nothing.");
  assert.match(M.undoInfo(rc({ undoState: "unavailable", type: "weird" })).why, /^Can’t be undone/);
});
test("a receipt that did not run never offers undo", () => {
  for (const status of ["failed", "denied", "needs-reapproval", "unsupported"]) {
    const u = M.undoInfo(rc({ status, undoState: "available" }));   // even a corrupt 'available' is not offered
    assert.strictEqual(u.state, "none", status);
    assert.strictEqual(M.isUndoable(rc({ status, undoState: "available" })), false);
  }
  assert.strictEqual(M.undoInfo(null).state, "none");
});
test("unknown status is shown as Unknown, never as success", () => {
  for (const s of ["", "__proto__", "constructor", "ok", null, 5]) assert.deepStrictEqual(M.statusInfo(s).slice(0, 1), ["Unknown"]);
});

console.log("== group ==");
test("empty or hostile input gives no groups", () => {
  assert.deepStrictEqual(M.group([]), []);
  assert.deepStrictEqual(M.group(null), []);
  assert.deepStrictEqual(M.group([null, {}, { id: 3 }]), []);
});
test("a single action is its own action group", () => {
  const g = M.group([rc({ id: "a", proposalId: "pa" })]);
  assert.strictEqual(g.length, 1);
  assert.strictEqual(g[0].kind, "action");
  assert.deepStrictEqual(g[0].outcome.slice(0, 1), ["Complete"]);
});
test("receipts of one proposal become one plan, in step order, newest group first", () => {
  const rs = [rc({ id: "c", proposalId: "P", stepIndex: 2, at: 30 }), rc({ id: "b", proposalId: "P", stepIndex: 1, at: 20 }), rc({ id: "a", proposalId: "P", stepIndex: 0, at: 10, requestText: "prepare my phone" }), rc({ id: "z", proposalId: "Q", at: 99 })];
  const g = M.group(rs, (id) => (id === "P" ? { state: "COMPLETED", steps: [1, 2, 3], skips: [] } : null));
  assert.deepStrictEqual(g.map((x) => x.id), ["Q", "P"]);
  const p = g[1];
  assert.deepStrictEqual(p.receipts.map((r) => r.id), ["a", "b", "c"]);
  assert.strictEqual(p.kind, "plan");
  assert.strictEqual(p.requestText, "prepare my phone");
  assert.strictEqual(p.outcome[0], "Complete");
  assert.strictEqual(p.notRun, 0);
});
test("partial, failed and stopped plans are not reported as complete", () => {
  const prop = (state) => () => ({ state, steps: [1, 2, 3], skips: [] });
  const partial = M.group([rc({ proposalId: "P", stepIndex: 0 }), rc({ proposalId: "P", stepIndex: 1, status: "failed", undoState: "unavailable", error: "x" })], prop("FAILED"))[0];
  assert.strictEqual(partial.outcome[0], "Partial");
  assert.strictEqual(partial.notRun, 1);
  const failed1 = M.group([rc({ proposalId: "P", stepIndex: 0, status: "failed", undoState: "unavailable" })], prop("FAILED"))[0];
  assert.strictEqual(failed1.outcome[0], "Failed");
  const stopped = M.group([rc({ proposalId: "P", stepIndex: 0 })], prop("CANCELLED"))[0];
  assert.strictEqual(stopped.outcome[0], "Stopped");
  assert.strictEqual(stopped.notRun, 2);
});
test("a plan whose receipts are all done but whose steps did not all run is not Complete", () => {
  const g = M.group([rc({ proposalId: "P", stepIndex: 0 })], () => ({ state: "FAILED", steps: [1, 2], skips: [] }))[0];
  assert.notStrictEqual(g.outcome[0], "Complete");
});
test("skipped steps are not counted as missing", () => {
  const g = M.group([rc({ proposalId: "P", stepIndex: 0 }), rc({ proposalId: "P", stepIndex: 2 })], () => ({ state: "COMPLETED", steps: [1, 2, 3], skips: [1] }))[0];
  assert.strictEqual(g.notRun, 0);
  assert.strictEqual(g.outcome[0], "Complete");
});
test("blocked single action says Blocked; stale approval says Needs review again", () => {
  assert.strictEqual(M.group([rc({ status: "denied", undoState: "unavailable" })])[0].outcome[0], "Blocked");
  assert.strictEqual(M.group([rc({ status: "needs-reapproval", undoState: "unavailable" })])[0].outcome[0], "Needs review again");
});
test("a throwing getProposal does not break grouping", () => {
  const g = M.group([rc({ proposalId: "P" })], () => { throw new Error("boom"); });
  assert.strictEqual(g.length, 1);
});
test("undoable count only counts available undos of done receipts", () => {
  const g = M.group([rc({ proposalId: "P", stepIndex: 0 }), rc({ proposalId: "P", stepIndex: 1, undoState: "used" }), rc({ proposalId: "P", stepIndex: 2, undoState: "unavailable" })])[0];
  assert.strictEqual(g.undoable, 1);
});

console.log("== filter ==");
const SET = () => M.group([
  rc({ id: "open", proposalId: "A", describe: { title: "Open Notes", kind: "App", icon: "notes" }, risk: "S0", source: "Aura" }),
  rc({ id: "mv", proposalId: "B", type: "file.move", risk: "S2", describe: { title: "Move “Budget Q4.csv” to Study", kind: "File", icon: "move" }, source: "Vault" }),
  rc({ id: "del", proposalId: "C", type: "file.delete", risk: "S3", status: "denied", undoState: "unavailable", errorText: "Blocked by Passport: “Delete files” is set to Deny.", describe: { title: "Delete “Timetable”", kind: "File", icon: "trash" }, source: "Vault" }),
  rc({ id: "old", proposalId: "D", undoState: "used", describe: { title: "Turn Focus mode on", kind: "Setting", icon: "focus" }, risk: "S1" }),
  rc({ id: "srch", proposalId: "E", type: "search.query", undoState: "unavailable", describe: { title: "Search for “x”", kind: "Search", icon: "search" } }),
]);
const ids = (gs) => [].concat(...gs.map((g) => g.receipts.map((r) => r.id))).sort();
test("each status filter", () => {
  assert.deepStrictEqual(ids(M.filter(SET(), { status: "all" })), ["del", "mv", "old", "open", "srch"]);
  assert.deepStrictEqual(ids(M.filter(SET(), { status: "done" })), ["mv", "old", "open", "srch"]);
  assert.deepStrictEqual(ids(M.filter(SET(), { status: "problem" })), ["del"]);
  assert.deepStrictEqual(ids(M.filter(SET(), { status: "undoable" })), ["mv", "open"]);
  assert.deepStrictEqual(ids(M.filter(SET(), { status: "undone" })), ["old"]);
});
test("risk filter and combined filters", () => {
  assert.deepStrictEqual(ids(M.filter(SET(), { risk: "S2" })), ["mv"]);
  assert.deepStrictEqual(ids(M.filter(SET(), { risk: "S3", status: "problem" })), ["del"]);
  assert.deepStrictEqual(ids(M.filter(SET(), { risk: "S3", status: "done" })), []);
  assert.deepStrictEqual(ids(M.filter(SET(), { risk: "S2", status: "undoable", query: "budget" })), ["mv"]);
  assert.deepStrictEqual(ids(M.filter(SET(), { risk: "S2", status: "undoable", query: "nothing" })), []);
});
test("search matches title, source, request text, error text and status words, case-insensitively", () => {
  assert.deepStrictEqual(ids(M.filter(SET(), { query: "BUDGET" })), ["mv"]);
  assert.deepStrictEqual(ids(M.filter(SET(), { query: "vault" })), ["del", "mv"]);
  assert.deepStrictEqual(ids(M.filter(SET(), { query: "set to deny" })), ["del"]);
  assert.deepStrictEqual(ids(M.filter(SET(), { query: "blocked" })), ["del"]);
  assert.deepStrictEqual(ids(M.filter(SET(), { query: "  focus  " })), ["old"]);
});
test("a plan stays when one step matches and reports matched/total", () => {
  const gs = M.group([rc({ id: "a", proposalId: "P", stepIndex: 0, requestText: "study" }), rc({ id: "b", proposalId: "P", stepIndex: 1, status: "failed", undoState: "unavailable", requestText: "study" })], () => ({ state: "FAILED", steps: [1, 2], skips: [] }));
  const f = M.filter(gs, { status: "problem" });
  assert.strictEqual(f.length, 1);
  assert.strictEqual(f[0].matched, 1);
  assert.strictEqual(f[0].total, 2);
});
test("bad filter values fall back to 'all' and never hide history", () => {
  for (const bad of ["__proto__", "constructor", null, 7, {}]) assert.strictEqual(ids(M.filter(SET(), { status: bad, risk: bad })).length, 5);
  assert.strictEqual(ids(M.filter(SET(), { query: 5 })).length, 5);
  assert.strictEqual(ids(M.filter(SET())).length, 5);
});
test("filtering does not mutate the groups", () => {
  const gs = SET(); M.filter(gs, { status: "problem" });
  assert.strictEqual(ids(gs).length, 5);
});

console.log("== day grouping ==");
test("dayLabel and byDay", () => {
  assert.strictEqual(M.dayLabel(NOW - 3600e3, NOW), "Today");
  assert.strictEqual(M.dayLabel(new Date(2026, 10, 11, 23, 59).getTime(), NOW), "Yesterday");
  assert.notStrictEqual(M.dayLabel(new Date(2026, 10, 9, 10).getTime(), NOW), "Yesterday");
  const gs = [{ at: NOW - 1000 }, { at: NOW - 2000 }, { at: NOW - 2 * 86400e3 }];
  const d = M.byDay(gs, NOW);
  assert.strictEqual(d.length, 2);
  assert.strictEqual(d[0].groups.length, 2);
});

console.log("== restore ==");
const del = (over) => rc(Object.assign({ type: "file.delete", risk: "S3", action: { type: "file.delete", arguments: { id: "timetable" } } }, over));
test("restoreReceipt finds the newest available delete receipt for that file only", () => {
  const rs = [del({ id: "d1", at: 10 }), del({ id: "d2", at: 20 }), del({ id: "d3", at: 30, action: { type: "file.delete", arguments: { id: "other" } } })];
  assert.strictEqual(M.restoreReceipt("timetable", rs).id, "d2");
  assert.strictEqual(M.restoreReceipt("other", rs).id, "d3");
});
test("no valid receipt means no restore and a reason", () => {
  assert.strictEqual(M.restoreReceipt("timetable", []), null);
  assert.strictEqual(M.restoreReceipt("timetable", [del({ undoState: "used" })]), null);
  assert.strictEqual(M.restoreReceipt("timetable", [del({ undoState: "failed" })]), null);
  assert.strictEqual(M.restoreReceipt("timetable", [del({ status: "denied", undoState: "available" })]), null);
  assert.strictEqual(M.restoreReceipt("timetable", [rc({ type: "file.move", action: { type: "file.move", arguments: { id: "timetable" } } })]), null);
  assert.match(M.restoreWhy("timetable", []), /No delete receipt/);
  assert.match(M.restoreWhy("timetable", [del({ undoState: "used" })]), /already undone/);
  assert.match(M.restoreWhy("timetable", [del({ undoState: "failed" })]), /failed/);
});

console.log(`\nTrail model: ${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
