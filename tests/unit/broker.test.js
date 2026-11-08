/** tests/unit/broker.test.js — Action Broker contract, risk classes, plans, Quick Commands (mock adapters only). */
const assert = require("assert");
const path = require("path");
const B = require(path.resolve(__dirname, "..", "..", "aurora-glass", "prototype", "broker.js"));

let passed = 0, failed = 0;
const queue = [];
function test(name, fn) { queue.push({ name, fn }); }

function setup(perms, extraAdapters) {
  const state = { space: "personal", focus: false, openApp: null, timer: null, trail: [], lastSearch: null,
    files: { report: { space: "work" }, notes1: { space: "study" } } };
  const permissions = B.createPermissions(Object.assign({
    "apps.open": "allow", "spaces.switch": "allow", "timer.control": "allow", "index.search": "allow",
    "settings.write": "allow", "files.edit": "allow", "files.delete": "allow", "external.send": "allow",
  }, perms));
  let n = 0;
  const broker = B.createBroker({ permissions, adapters: Object.assign(B.createMockAdapters(state), extraAdapters), nonce: () => "n" + (++n), now: () => 1000 });
  return { state, permissions, broker };
}
const studyPlan = () => B.parseCommand("Prepare my phone for studying.").plan;

// ---- validation ----
test("rejects malformed, unknown, missing, extra and invalid input", async () => {
  const bad = [null, 5, "x", [], {}, { type: 1, arguments: {} }, { type: "rm.rf", arguments: {} },
    { type: "app.open" }, { type: "app.open", arguments: {} }, { type: "app.open", arguments: { app: "terminal" } },
    { type: "app.open", arguments: { app: "notes", extra: 1 } }, { type: "app.open", arguments: { app: "notes" }, extra: 1 },
    { type: "timer.start", arguments: { seconds: 0 } }, { type: "timer.start", arguments: { seconds: 1.5 } },
    { type: "timer.start", arguments: { seconds: "60" } }, { type: "timer.start", arguments: { seconds: 86401 } },
    { type: "focus.set", arguments: { enabled: "true" } }, { type: "file.delete", arguments: { id: "../etc/passwd" } },
    { type: "search.query", arguments: { query: "a\u0000b" } }, { type: "search.query", arguments: { query: "   " } },
    { type: "__proto__", arguments: {} }, { type: "constructor", arguments: {} }, { type: "toString", arguments: {} }];
  bad.forEach((a) => assert.strictEqual(B.validateAction(a).ok, false, JSON.stringify(a)));
  assert.strictEqual(B.validateAction({ type: "timer.start", arguments: { seconds: 1500 } }).ok, true);
});
test("invalid actions never reach an adapter", async () => {
  let called = 0;
  const { broker } = setup({}, { "app.open": async () => { called++; } });
  const r = await broker.execute({ type: "app.open", arguments: { app: "bash" } });
  assert.strictEqual(r.status, "invalid"); assert.strictEqual(called, 0);
});

// ---- permissions & risk ----
test("denied and unknown permissions block execution", async () => {
  const { broker, state, permissions } = setup({ "settings.write": "deny" });
  const r = await broker.execute({ type: "focus.set", arguments: { enabled: true } });
  assert.strictEqual(r.status, "denied"); assert.strictEqual(state.focus, false);
  assert.strictEqual(permissions.set("files.edit", "bogus"), false); // invalid decision rejected
  assert.strictEqual(permissions.get("files.edit"), "allow");
  const fresh = B.createBroker({ permissions: B.createPermissions({}), adapters: B.createMockAdapters({}) });
  assert.strictEqual((await fresh.execute({ type: "app.open", arguments: { app: "notes" } })).status, "denied");
});
test("risk classes: S0/S1/S2/S3 assigned by action type", async () => {
  const { broker } = setup({});
  assert.strictEqual(broker.preview({ type: "app.open", arguments: { app: "notes" } }).risk, "S0");
  assert.strictEqual(broker.preview({ type: "focus.set", arguments: { enabled: true } }).risk, "S1");
  assert.strictEqual(broker.preview({ type: "file.move", arguments: { id: "report", destination: "study" } }).risk, "S2");
  const p = broker.preview({ type: "file.delete", arguments: { id: "report" } });
  assert.strictEqual(p.risk, "S3"); assert.strictEqual(p.needsConfirmation, true);
});
test("S3 never runs silently, even when permission is allow", async () => {
  const { broker, state } = setup({});
  const a = { type: "file.delete", arguments: { id: "report" } };
  assert.strictEqual((await broker.execute(a)).status, "needs-confirmation");
  assert.strictEqual((await broker.execute(a, "yes")).status, "needs-confirmation");        // arbitrary string
  assert.strictEqual((await broker.execute(a, "I confirm")).status, "needs-confirmation"); // model-style text
  assert.strictEqual((await broker.execute(a, true)).status, "needs-confirmation");
  assert.ok(state.files.report);
});
test("confirmation token is bound to the exact action and single use", async () => {
  const { broker, state } = setup({});
  const del = { type: "file.delete", arguments: { id: "report" } };
  const other = { type: "file.delete", arguments: { id: "notes1" } };
  const t = broker.requestConfirmation(del);
  assert.strictEqual((await broker.execute(other, t)).status, "needs-confirmation");
  assert.ok(state.files.notes1);
  const ok = await broker.execute(del, t);
  assert.strictEqual(ok.status, "done"); assert.strictEqual(state.files.report, undefined);
  assert.strictEqual((await broker.execute(del, t)).status, "needs-confirmation"); // not reusable
  assert.strictEqual(broker.requestConfirmation({ type: "nope", arguments: {} }), null);
});
test("'ask' permission requires confirmation even for S0", async () => {
  const { broker } = setup({ "apps.open": "ask" });
  const a = { type: "app.open", arguments: { app: "notes" } };
  assert.strictEqual((await broker.execute(a)).status, "needs-confirmation");
  assert.strictEqual((await broker.execute(a, broker.requestConfirmation(a))).status, "done");
});
test("unsupported action (no adapter) is reported, not faked", async () => {
  const { broker } = setup({});
  const a = { type: "share.send", arguments: { id: "report", to: "someone" } };
  assert.strictEqual((await broker.execute(a, broker.requestConfirmation(a))).status, "unsupported");
  assert.strictEqual(broker.preview(a).supported, false);
});

// ---- undo & receipts ----
test("undo restores state once; non-undoable and unknown receipts refuse", async () => {
  const { broker, state } = setup({});
  const r = await broker.execute({ type: "file.move", arguments: { id: "report", destination: "study" } });
  assert.strictEqual(state.files.report.space, "study"); assert.strictEqual(r.undoable, true);
  assert.deepStrictEqual(await broker.undo(r.id), { ok: true });
  assert.strictEqual(state.files.report.space, "work");
  assert.strictEqual((await broker.undo(r.id)).reason, "already-undone");
  assert.strictEqual((await broker.undo("nope")).reason, "not-found");
  const s = await broker.execute({ type: "search.query", arguments: { query: "x" } });
  assert.strictEqual(s.undoable, false); assert.strictEqual((await broker.undo(s.id)).reason, "not-undoable");
});
test("delete goes to Trail and can be undone", async () => {
  const { broker, state } = setup({});
  const del = { type: "file.delete", arguments: { id: "report" } };
  const r = await broker.execute(del, broker.requestConfirmation(del));
  assert.deepStrictEqual(state.trail, ["report"]);
  await broker.undo(r.id);
  assert.ok(state.files.report); assert.deepStrictEqual(state.trail, []);
});
test("adapter failure yields a failed receipt and no undo", async () => {
  const { broker } = setup({});
  const r = await broker.execute({ type: "file.move", arguments: { id: "ghost", destination: "study" } });
  assert.strictEqual(r.status, "failed"); assert.strictEqual(r.undoable, false);
  assert.ok(broker.receipts().some((x) => x.id === r.id));
});
test("receipts are copies; every attempt is recorded", async () => {
  const { broker } = setup({});
  await broker.execute({ type: "bad" }); await broker.execute({ type: "app.open", arguments: { app: "notes" } });
  const rs = broker.receipts(); assert.strictEqual(rs.length, 2);
  rs[0].status = "hacked"; assert.strictEqual(broker.receipts()[0].status, "invalid");
});

// ---- plans ----
test("study plan runs fully with full rollback, in order", async () => {
  const { broker, state } = setup({});
  const pv = broker.previewPlan(studyPlan());
  assert.strictEqual(pv.rollback, "full"); assert.strictEqual(pv.risk, "S1");
  const res = await broker.runPlan(studyPlan());
  assert.strictEqual(res.ok, true); assert.strictEqual(res.rollback, "full");
  assert.deepStrictEqual([state.space, state.focus, state.openApp, state.timer], ["study", true, "notes", 1500]);
  const undo = await broker.undoPlan(res);
  assert.ok(undo.every((u) => u.ok));
  assert.deepStrictEqual([state.space, state.focus, state.openApp, state.timer], ["personal", false, null, null]);
});
test("repeated execution is safe and deterministic", async () => {
  const { broker, state } = setup({});
  await broker.runPlan(studyPlan()); const again = await broker.runPlan(studyPlan());
  assert.strictEqual(again.ok, true); assert.strictEqual(state.timer, 1500);
  assert.strictEqual(broker.receipts().length, 8);
});
test("permission rejection mid-plan stops there; completed steps remain undoable", async () => {
  const { broker, state } = setup({ "apps.open": "deny" });
  const res = await broker.runPlan(studyPlan());
  assert.strictEqual(res.ok, false); assert.strictEqual(res.stoppedAt, 2);
  assert.strictEqual(res.steps[2].status, "denied"); assert.strictEqual(res.steps.length, 3);
  assert.strictEqual(state.timer, null); assert.strictEqual(res.rollback, "full");
  await broker.undoPlan(res); assert.strictEqual(state.space, "personal"); assert.strictEqual(state.focus, false);
});
test("failure at each step position stops execution and leaves later steps unrun", async () => {
  for (let fail = 0; fail < 4; fail++) {
    const type = studyPlan().steps[fail].type;
    const { broker, state } = setup({}, { [type]: async () => { throw new Error("boom"); } });
    const res = await broker.runPlan(studyPlan());
    assert.strictEqual(res.stoppedAt, fail); assert.strictEqual(res.steps[fail].status, "failed");
    assert.strictEqual(res.steps.length, fail + 1);
    if (fail < 3) assert.strictEqual(state.timer, null);
  }
});
test("skipping steps and cancellation", async () => {
  const { broker, state } = setup({});
  const res = await broker.runPlan(studyPlan(), { skip: new Set([1]) });
  assert.strictEqual(res.steps[1].status, "skipped"); assert.strictEqual(state.focus, false); assert.strictEqual(state.timer, 1500);
  const s2 = setup({}); let n = 0;
  const c = await s2.broker.runPlan(studyPlan(), { isCancelled: () => n++ >= 2 });
  assert.strictEqual(c.ok, false); assert.strictEqual(c.steps[2].status, "cancelled"); assert.strictEqual(s2.state.openApp, null);
});
test("plan with S3 step stops for confirmation; token lets it proceed", async () => {
  const { broker, state } = setup({});
  const del = { type: "file.delete", arguments: { id: "report" } };
  const plan = { steps: [{ type: "app.open", arguments: { app: "files" } }, del] };
  const stopped = await broker.runPlan(plan);
  assert.strictEqual(stopped.steps[1].status, "needs-confirmation"); assert.ok(state.files.report);
  const done = await broker.runPlan(plan, { confirmations: { 1: broker.requestConfirmation(del) } });
  assert.strictEqual(done.ok, true); assert.strictEqual(state.files.report, undefined);
});
test("plan with an irreversible step reports limited/none rollback honestly", async () => {
  const send = { type: "share.send", arguments: { id: "report", to: "a friend" } };
  const { broker } = setup({}, { "share.send": async () => ({ result: "sent" }) });
  const plan = { steps: [{ type: "focus.set", arguments: { enabled: true } }, send] };
  assert.strictEqual(broker.previewPlan(plan).rollback, "limited");
  const res = await broker.runPlan(plan, { confirmations: { 1: broker.requestConfirmation(send) } });
  assert.strictEqual(res.ok, true); assert.strictEqual(res.rollback, "limited");
  assert.strictEqual(broker.previewPlan({ steps: [send] }).rollback, "none");
});
test("invalid and conflicting plans are rejected before anything runs", async () => {
  const { broker, state } = setup({});
  const bad = [null, {}, { steps: [] }, { steps: "x" }, { steps: [{ type: "x", arguments: {} }], extra: 1 },
    { steps: Array(13).fill({ type: "focus.set", arguments: { enabled: true } }) },
    { steps: [{ type: "focus.set", arguments: { enabled: true } }, { type: "bad" }] }];
  for (const p of bad) { const r = await broker.runPlan(p); assert.strictEqual(r.ok, false); assert.deepStrictEqual(r.receipts, []); }
  const conflict = { steps: [{ type: "file.move", arguments: { id: "report", destination: "study" } }, { type: "file.delete", arguments: { id: "report" } }] };
  assert.strictEqual((await broker.runPlan(conflict)).error, "conflicting-steps");
  assert.strictEqual(state.files.report.space, "work"); assert.strictEqual(broker.receipts().length, 0);
});
test("editStep returns a new validated plan and never mutates the original", async () => {
  const { broker } = setup({});
  const p = studyPlan();
  const e = broker.editStep(p, 3, { type: "timer.start", arguments: { seconds: 600 } });
  assert.strictEqual(e.ok, true); assert.strictEqual(e.plan.steps[3].arguments.seconds, 600); assert.strictEqual(p.steps[3].arguments.seconds, 1500);
  assert.strictEqual(broker.editStep(p, 9, p.steps[0]).ok, false);
  assert.strictEqual(broker.editStep(p, 0, { type: "app.open", arguments: { app: "bash" } }).ok, false);
});
test("concurrent executions are serialized; a token cannot be double-spent", async () => {
  const order = [];
  const { broker } = setup({}, { "timer.start": async ({ seconds }) => { order.push("s" + seconds); await new Promise((r) => setTimeout(r, 15)); order.push("e" + seconds); return {}; } });
  await Promise.all([broker.execute({ type: "timer.start", arguments: { seconds: 1 } }), broker.execute({ type: "timer.start", arguments: { seconds: 2 } })]);
  assert.deepStrictEqual(order, ["s1", "e1", "s2", "e2"]);
  const del = { type: "file.delete", arguments: { id: "report" } }; const t = broker.requestConfirmation(del);
  const [a, b] = await Promise.all([broker.execute(del, t), broker.execute(del, t)]);
  assert.deepStrictEqual([a.status, b.status].sort(), ["done", "needs-confirmation"]);
});

// ---- Quick Commands ----
test("Quick Commands parse to typed, schema-valid actions", () => {
  const cases = [
    ["Open Notes", { type: "app.open", arguments: { app: "notes" } }],
    ["open the calculator app", { type: "app.open", arguments: { app: "calculator" } }],
    ["Start a 25 minute timer", { type: "timer.start", arguments: { seconds: 1500 } }],
    ["start a 90 second timer.", { type: "timer.start", arguments: { seconds: 90 } }],
    ["start an 1 hour timer", { type: "timer.start", arguments: { seconds: 3600 } }],
    ["Go to Study", { type: "space.switch", arguments: { space: "study" } }],
    ["switch to the work space", { type: "space.switch", arguments: { space: "work" } }],
    ["Turn on focus mode", { type: "focus.set", arguments: { enabled: true } }],
    ["turn off focus", { type: "focus.set", arguments: { enabled: false } }],
    ["Show my files", { type: "app.open", arguments: { app: "files" } }],
    ["Search for my report", { type: "search.query", arguments: { query: "my report" } }],
  ];
  cases.forEach(([text, want]) => {
    const r = B.parseCommand(text); assert.strictEqual(r.ok, true, text);
    assert.deepStrictEqual(r.action, want, text); assert.strictEqual(B.validateAction(r.action).ok, true, text);
  });
  const plan = B.parseCommand("Prepare my phone for studying.");
  assert.strictEqual(plan.kind, "plan"); assert.strictEqual(plan.plan.steps.length, 4);
});
test("Quick Commands reject unsupported, dangerous or out-of-range text", () => {
  ["", "   ", "rm -rf /", "open terminal", "delete all my files", "start a 0 minute timer", "start a 9999 hour timer",
    "go to narnia", "format the disk && open notes", "open notes; open files", 42, null, undefined, "x".repeat(300)]
    .forEach((t) => assert.strictEqual(B.parseCommand(t).ok, false, String(t)));
});
test("parser never emits S3 actions, so text alone cannot reach deletion or sending", () => {
  ["delete report", "send my report to bob", "remove everything"].forEach((t) => assert.strictEqual(B.parseCommand(t).ok, false));
});
test("parser output carries no permission or confirmation fields", () => {
  const r = B.parseCommand("Open Notes"); assert.deepStrictEqual(Object.keys(r.action).sort(), ["arguments", "type"]);
});

(async () => {
  console.log("== Action Broker ==");
  for (const t of queue) {
    try { await t.fn(); passed++; console.log("  PASS:", t.name); }
    catch (e) { failed++; console.log("  FAIL:", t.name, "\n   ", e.message); }
  }
  console.log(`\n== Summary ==\nPassed: ${passed}\nFailed: ${failed}`);
  process.exit(failed ? 1 : 0);
})();
