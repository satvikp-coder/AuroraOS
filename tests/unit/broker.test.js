/** tests/unit/broker.test.js — Action Broker: proposal/approval/execution boundaries (mock adapters only). */
const assert = require("assert");
const path = require("path");
const B = require(path.resolve(__dirname, "..", "..", "aurora-glass", "prototype", "broker.js"));

let passed = 0, failed = 0;
const queue = [];
function test(name, fn) { queue.push({ name, fn }); }
const defer = () => { let resolve; const promise = new Promise((r) => { resolve = r; }); return { promise, resolve }; };

function setup(perms, extra, opts) {
  const state = { space: "personal", focus: false, openApp: null, timer: null, trail: [], lastSearch: null,
    files: { report: { space: "work" }, notes1: { space: "study" } } };
  const calls = [];
  const permissions = B.createPermissions(Object.assign({
    "apps.open": "allow", "spaces.switch": "allow", "timer.control": "allow", "index.search": "allow",
    "settings.write": "allow", "files.edit": "allow", "files.delete": "allow", "external.send": "allow",
  }, perms));
  const base = B.createMockAdapters(state);
  const adapters = {};
  Object.keys(base).forEach((k) => { adapters[k] = async (a) => { calls.push(k); return base[k](a); }; });
  Object.assign(adapters, extra);
  const t = { now: 1000 };
  let n = 0;
  const broker = B.createBroker(Object.assign({ permissions, adapters, nonce: () => "n" + (++n), now: () => t.now, approvalTtlMs: 1000, previewTtlMs: 5000 }, opts));
  const ui = broker.takeTrustedUI();
  /** Propose, then (as the trusted UI) approve exactly what was shown, then execute. */
  const approve = (id) => { const p = broker.getProposal(id); return ui.approve(id, { digest: p.digest, confirmS3: p.preview.s3Indices }); };
  const propose = (x) => { const r = broker.propose(x); assert.ok(r.ok, JSON.stringify(r)); return r.proposal; };
  const run = async (x) => { const p = propose(x); const a = approve(p.id); assert.ok(a.ok, JSON.stringify(a)); return { id: p.id, res: await broker.execute(p.id) }; };
  return { state, permissions, broker, ui, t, calls, approve, propose, run };
}
const studyPlan = () => B.parseCommand("Prepare my phone for studying.").plan;
const act = (type, args) => ({ type, arguments: args });
const delReport = () => act("file.delete", { id: "report" });

// ---------------- schema & prototype safety ----------------
test("rejects malformed, unknown, missing, extra and invalid input", () => {
  const { broker } = setup({});
  const bad = [null, 5, "x", [], {}, { type: 1, arguments: {} }, act("rm.rf", {}), { type: "app.open" }, act("app.open", {}),
    act("app.open", { app: "terminal" }), act("app.open", { app: "notes", extra: 1 }), { type: "app.open", arguments: { app: "notes" }, extra: 1 },
    act("timer.start", { seconds: 0 }), act("timer.start", { seconds: 1.5 }), act("timer.start", { seconds: "60" }), act("timer.start", { seconds: 86401 }),
    act("timer.start", { seconds: NaN }), act("timer.start", { seconds: Infinity }), act("focus.set", { enabled: "true" }),
    act("file.delete", { id: "../etc/passwd" }), act("search.query", { query: "a\u0000b" }), act("search.query", { query: "   " }),
    act("search.query", { query: "x‮y" }), act("search.query", { query: "x​y" }), act("search.query", { query: "x".repeat(101) }),
    act("__proto__", {}), act("constructor", {}), act("toString", {}), act("hasOwnProperty", {}),
    act("app.open", { app: { toString: () => "notes" } }), act("app.open", { app: ["notes"] })];
  bad.forEach((a) => assert.strictEqual(broker.propose(a).ok, false, JSON.stringify(a)));
  assert.strictEqual(broker.propose(act("timer.start", { seconds: 1500 })).ok, true);
});
test("prototype pollution payloads are rejected and Object.prototype stays clean", () => {
  const { broker } = setup({});
  [JSON.parse('{"type":"app.open","arguments":{"app":"notes"},"__proto__":{"polluted":1}}'),
   JSON.parse('{"type":"app.open","arguments":{"app":"notes","__proto__":{"polluted":1}}}'),
   JSON.parse('{"steps":[{"type":"app.open","arguments":{"app":"notes"}}],"__proto__":{"polluted":1}}'),
   JSON.parse('{"type":"app.open","arguments":{"app":"notes","constructor":{"prototype":{"polluted":1}}}}')]
    .forEach((p) => assert.strictEqual(broker.propose(p).ok, false));
  assert.strictEqual({}.polluted, undefined);
  assert.strictEqual(B.createPermissions({}).get("__proto__"), "deny");
  assert.strictEqual(B.createPermissions({}).get("constructor"), "deny");
});
test("accessor, proxy and mutated-after-submit payloads cannot change what was reviewed", () => {
  const { broker } = setup({});
  let reads = 0;
  const sneaky = { type: "app.open", get arguments() { reads++; return { app: reads > 1 ? "settings" : "notes" }; } };
  assert.strictEqual(broker.propose(sneaky).ok, false);
  assert.strictEqual(broker.propose(new Proxy({ type: "app.open", arguments: { app: "notes" } }, {})).ok, true); // plain data through a proxy is just data
  const payload = act("app.open", { app: "notes" });
  const p = broker.propose(payload).proposal;
  payload.arguments.app = "settings"; payload.type = "file.delete";
  assert.strictEqual(broker.getProposal(p.id).steps[0].arguments.app, "notes");
});
test("oversized payloads, too many steps and too many live proposals are rejected", () => {
  const { broker } = setup({});
  assert.strictEqual(broker.propose({ steps: Array(13).fill(act("focus.set", { enabled: true })) }).error, "too-many-steps");
  assert.strictEqual(broker.propose(act("search.query", { query: "y".repeat(5000) })).error, "too-large");
  for (let i = 0; i < 100; i++) assert.ok(broker.propose(act("focus.set", { enabled: true })).ok);
  assert.strictEqual(broker.propose(act("focus.set", { enabled: true })).error, "too-many-proposals");
});
test("invalid actions never reach an adapter", async () => {
  const { broker, calls } = setup({});
  assert.strictEqual(broker.propose(act("app.open", { app: "bash" })).ok, false);
  assert.deepStrictEqual(calls, []);
});

// ---------------- trust boundary ----------------
test("trusted UI facet can be taken exactly once; proposer facet has no authority", () => {
  const { broker } = setup({});
  assert.throws(() => broker.takeTrustedUI(), /already-taken/);
  assert.deepStrictEqual(Object.keys(broker.proposer).sort(), ["getProposal", "preview", "propose"]);
  assert.ok(Object.isFrozen(broker.proposer));
  ["approve", "execute", "undo", "cancel", "editStep", "takeTrustedUI"].forEach((k) => assert.strictEqual(broker.proposer[k], undefined));
});
test("forged approval: wrong digest, text, unknown id and tampered view cannot approve", async () => {
  const { broker, ui, calls } = setup({});
  const p = broker.propose(act("file.move", { id: "report", destination: "study" })).proposal;
  assert.strictEqual(ui.approve(p.id, { digest: "yes", confirmS3: [] }).reason, "digest-mismatch");
  assert.strictEqual(ui.approve(p.id, null).reason, "digest-mismatch");
  assert.strictEqual(ui.approve(p.id, {}).reason, "digest-mismatch");
  assert.strictEqual(ui.approve("p999", { digest: p.digest }).reason, "not-found");
  const view = broker.getProposal(p.id); view.state = "APPROVED"; view.history.push("APPROVED"); // tampering with a copy
  assert.deepStrictEqual(await broker.execute(p.id), { ok: false, reason: "approval-required", state: "PREVIEWED" });
  assert.strictEqual((await broker.execute("nope")).reason, "not-found");
  assert.deepStrictEqual(calls, []);
});
test("approval for one action cannot be used for another (digest binding)", async () => {
  const { broker, ui, state } = setup({});
  const a = broker.propose(act("file.move", { id: "report", destination: "study" })).proposal;
  const b = broker.propose(act("file.move", { id: "notes1", destination: "work" })).proposal;
  assert.strictEqual(ui.approve(b.id, { digest: a.digest }).reason, "digest-mismatch");
  assert.ok(ui.approve(a.id, { digest: a.digest }).ok);
  assert.strictEqual((await broker.execute(b.id)).reason, "approval-required");
  assert.strictEqual(state.files.notes1.space, "study");
});
test("replayed approval: a consumed approval cannot run again; new proposal needs new approval", async () => {
  const { broker, run, propose, calls } = setup({});
  const first = await run(act("file.move", { id: "report", destination: "study" }));
  assert.strictEqual(first.res.state, "COMPLETED");
  const n = calls.length;
  assert.strictEqual((await broker.execute(first.id)).reason, "approval-required");
  const again = propose(act("file.move", { id: "report", destination: "study" }));
  assert.strictEqual((await broker.execute(again.id)).reason, "approval-required");
  assert.strictEqual(calls.length, n);
});
test("expired approval and expired preview are refused and never run", async () => {
  const { broker, ui, t, calls, propose, approve } = setup({});
  const p = propose(act("file.move", { id: "report", destination: "study" }));
  assert.ok(approve(p.id).ok);
  t.now += 1001;
  const r = await broker.execute(p.id);
  assert.strictEqual(r.reason, "approval-expired"); assert.strictEqual(broker.getProposal(p.id).state, "EXPIRED");
  const q = propose(act("file.move", { id: "report", destination: "study" }));
  t.now += 5001;
  assert.strictEqual(ui.approve(q.id, { digest: q.digest }).reason, "preview-expired");
  const s = propose(act("app.open", { app: "notes" })); t.now += 5001;
  assert.strictEqual((await broker.execute(s.id)).reason, "preview-expired");
  assert.deepStrictEqual(calls, []);
});
test("modifying or skipping a step after approval invalidates it", async () => {
  const { broker, ui, propose, approve, calls } = setup({});
  const p = propose(studyPlan()); assert.ok(approve(p.id).ok);
  const e = ui.editStep(p.id, 3, act("timer.start", { seconds: 600 }));
  assert.ok(e.ok); assert.notStrictEqual(e.proposal.id, p.id); assert.notStrictEqual(e.proposal.digest, p.digest);
  assert.strictEqual(broker.getProposal(p.id).state, "SUPERSEDED");
  assert.strictEqual((await broker.execute(p.id)).reason, "approval-required");
  assert.strictEqual((await broker.execute(e.proposal.id)).reason, "approval-required"); // successor needs its own approval
  const sk = ui.skipStep(e.proposal.id, 1, true);
  assert.ok(sk.ok); assert.strictEqual(broker.getProposal(e.proposal.id).state, "SUPERSEDED");
  assert.deepStrictEqual(calls, []);
  assert.strictEqual(ui.editStep(p.id, 0, act("app.open", { app: "bash" })).ok, false);
  assert.strictEqual(ui.editStep(sk.proposal.id, 9, act("app.open", { app: "notes" })).error, "bad-index");
});

// ---------------- review policy: permission != approval ----------------
test("S0 and S1 single actions run directly when permitted", async () => {
  const { broker, propose, state } = setup({});
  assert.strictEqual((await broker.execute(propose(act("app.open", { app: "notes" })).id)).state, "COMPLETED");
  assert.strictEqual((await broker.execute(propose(act("focus.set", { enabled: true })).id)).state, "COMPLETED");
  assert.strictEqual(state.focus, true);
});
test("S2 file.move needs explicit review even with files.edit = allow", async () => {
  const { broker, propose, approve, state } = setup({});
  const p = propose(act("file.move", { id: "report", destination: "study" }));
  assert.strictEqual(p.preview.risk, "S2"); assert.strictEqual(p.preview.needsReview, true);
  assert.strictEqual((await broker.execute(p.id)).reason, "approval-required");
  assert.strictEqual(state.files.report.space, "work");
  assert.ok(approve(p.id).ok);
  assert.strictEqual((await broker.execute(p.id)).state, "COMPLETED"); assert.strictEqual(state.files.report.space, "study");
});
test("'ask' permission requires review even for S0", async () => {
  const { broker, propose, approve } = setup({ "apps.open": "ask" });
  const p = propose(act("app.open", { app: "notes" }));
  assert.strictEqual((await broker.execute(p.id)).reason, "approval-required");
  assert.ok(approve(p.id).ok); assert.strictEqual((await broker.execute(p.id)).state, "COMPLETED");
});
test("S3 needs approval AND explicit per-step confirmation; text cannot supply either", async () => {
  const { broker, ui, propose, state } = setup({});
  const p = propose(delReport());
  assert.deepStrictEqual(p.preview.s3Indices, [0]);
  assert.strictEqual((await broker.execute(p.id)).reason, "approval-required");
  assert.strictEqual(ui.approve(p.id, { digest: p.digest }).reason, "s3-confirmation-required");
  assert.strictEqual(ui.approve(p.id, { digest: p.digest, confirmS3: [] }).reason, "s3-confirmation-required");
  assert.strictEqual(ui.approve(p.id, { digest: p.digest, confirmS3: [1] }).reason, "s3-confirmation-required");
  assert.strictEqual(ui.approve(p.id, { digest: p.digest, confirmS3: "yes" }).reason, "s3-confirmation-required");
  assert.ok(state.files.report);
  assert.ok(ui.approve(p.id, { digest: p.digest, confirmS3: [0] }).ok);
  assert.strictEqual((await broker.execute(p.id)).state, "COMPLETED"); assert.strictEqual(state.files.report, undefined);
  assert.deepStrictEqual(state.trail, ["report"]);
});
test("unsupported and denied steps cannot be approved", () => {
  const { broker, ui, permissions } = setup({});
  const s = broker.propose(act("share.send", { id: "report", to: "friend" })).proposal;
  assert.strictEqual(s.preview.steps[0].supported, false);
  assert.strictEqual(ui.approve(s.id, { digest: s.digest, confirmS3: [0] }).reason, "unsupported-step");
  permissions.set("files.edit", "deny");
  const m = broker.propose(act("file.move", { id: "report", destination: "study" })).proposal;
  assert.strictEqual(ui.approve(m.id, { digest: m.digest }).reason, "permission-denied");
});
test("permission revoked after preview blocks approval; revoked after approval blocks the step", async () => {
  const { broker, ui, permissions, propose, approve, state, calls } = setup({});
  const p = propose(act("file.move", { id: "report", destination: "study" }));
  permissions.set("files.edit", "deny");
  assert.strictEqual(approve(p.id).reason, "permission-denied");
  permissions.set("files.edit", "allow");
  const q = propose(act("file.move", { id: "report", destination: "study" })); assert.ok(approve(q.id).ok);
  permissions.set("files.edit", "deny");
  const r = await broker.execute(q.id);
  assert.strictEqual(r.state, "FAILED"); assert.strictEqual(r.steps[0].status, "denied");
  assert.strictEqual(state.files.report.space, "work"); assert.deepStrictEqual(calls, []);
  const d = propose(act("app.open", { app: "notes" })); permissions.set("apps.open", "deny");
  assert.strictEqual((await broker.execute(d.id)).steps[0].status, "denied");
});

// ---------------- plan approval lifecycle ----------------
test("plans (even all-S0) cannot run without plan-level approval", async () => {
  const { broker, propose, calls, state } = setup({});
  const p = propose({ steps: [act("app.open", { app: "notes" }), act("space.switch", { space: "study" })] });
  assert.strictEqual(p.preview.needsReview, true);
  assert.deepStrictEqual(p.history, ["DRAFT", "VALIDATED", "PREVIEWED"]);
  assert.strictEqual((await broker.execute(p.id)).reason, "approval-required");
  assert.deepStrictEqual(calls, []); assert.strictEqual(state.space, "personal");
});
test("lifecycle: DRAFT -> VALIDATED -> PREVIEWED -> APPROVED -> RUNNING -> COMPLETED", async () => {
  const { broker, propose, approve } = setup({});
  const p = propose(studyPlan()); approve(p.id);
  await broker.execute(p.id);
  assert.deepStrictEqual(broker.getProposal(p.id).history, ["DRAFT", "VALIDATED", "PREVIEWED", "APPROVED", "RUNNING", "COMPLETED"]);
});
test("study plan: approve, run with progress, receipt, then full undo", async () => {
  const { broker, propose, approve, state } = setup({});
  const p = propose(studyPlan()); assert.strictEqual(p.preview.risk, "S1"); assert.strictEqual(p.preview.expectedRollback, "full");
  approve(p.id);
  const events = [];
  const res = await broker.execute(p.id, { onProgress: (e) => events.push(e.index + ":" + e.status) });
  assert.strictEqual(res.ok, true); assert.strictEqual(res.actualRollback, "full");
  assert.deepStrictEqual(events, ["0:running", "0:done", "1:running", "1:done", "2:running", "2:done", "3:running", "3:done"]);
  assert.deepStrictEqual([state.space, state.focus, state.openApp, state.timer], ["study", true, "notes", 1500]);
  const u = await broker.undoProposal(p.id);
  assert.strictEqual(u.ok, true);
  assert.deepStrictEqual([state.space, state.focus, state.openApp, state.timer], ["personal", false, null, null]);
  assert.strictEqual((await broker.undoProposal(p.id)).ok, false); // nothing left to undo
});
test("skipped steps are not run and are shown as skipped", async () => {
  const { broker, ui, propose, approve, state } = setup({});
  const p = propose(studyPlan()); const s = ui.skipStep(p.id, 1, true).proposal;
  assert.strictEqual(s.preview.steps[1].skipped, true);
  approve(s.id);
  const r = await broker.execute(s.id);
  assert.strictEqual(r.steps[1].status, "skipped"); assert.strictEqual(state.focus, false); assert.strictEqual(state.timer, 1500);
  const all = studyPlan(); const q = propose(all); let cur = q;
  for (let i = 0; i < 4; i++) cur = ui.skipStep(cur.id, i, true).proposal;
  assert.strictEqual(approve(cur.id).reason, "nothing-to-run");
});
test("failure at every step position stops the plan and leaves later steps unrun", async () => {
  for (let fail = 0; fail < 4; fail++) {
    const type = studyPlan().steps[fail].type;
    const { broker, propose, approve, state } = setup({}, { [type]: async () => { throw new Error("boom"); } });
    const p = propose(studyPlan()); approve(p.id);
    const res = await broker.execute(p.id);
    assert.strictEqual(res.state, "FAILED"); assert.strictEqual(res.steps[fail].status, "failed");
    assert.strictEqual(res.steps.filter((s) => s.status === "done").length, fail);
    if (fail < 3) assert.strictEqual(state.timer, null);
    assert.strictEqual(res.actualRollback, fail === 0 ? "none" : "full");
  }
});
test("a plan containing S3 needs its own confirmation of that step even when the plan is approved", async () => {
  const { broker, ui, propose, state } = setup({});
  const p = propose({ steps: [act("app.open", { app: "files" }), delReport()] });
  assert.deepStrictEqual(p.preview.s3Indices, [1]);
  assert.strictEqual(ui.approve(p.id, { digest: p.digest }).reason, "s3-confirmation-required");
  assert.strictEqual(ui.approve(p.id, { digest: p.digest, confirmS3: [0] }).reason, "s3-confirmation-required");
  assert.ok(state.files.report);
  assert.ok(ui.approve(p.id, { digest: p.digest, confirmS3: [1] }).ok);
  assert.strictEqual((await broker.execute(p.id)).state, "COMPLETED"); assert.strictEqual(state.files.report, undefined);
});
test("conflicting plan steps are rejected before anything is stored or run", async () => {
  const { broker, calls } = setup({});
  const r = broker.propose({ steps: [act("file.move", { id: "report", destination: "study" }), delReport()] });
  assert.strictEqual(r.error, "conflicting-steps"); assert.deepStrictEqual(calls, []);
  [{}, { steps: [] }, { steps: "x" }, { steps: [{ type: "x", arguments: {} }], extra: 1 }, { steps: [act("focus.set", { enabled: true }), { type: "bad" }] }]
    .forEach((p) => assert.strictEqual(broker.propose(p).ok, false));
});

// ---------------- rollback accuracy ----------------
test("expected rollback is not actual rollback: adapter that returns no undo", async () => {
  const { broker, propose, approve } = setup({}, { "focus.set": async () => ({}) }); // claims success, offers no undo
  const p = propose(studyPlan());
  assert.strictEqual(p.preview.expectedRollback, "full"); assert.ok(p.preview.rollbackNote);
  approve(p.id);
  const res = await broker.execute(p.id);
  assert.strictEqual(res.ok, true); assert.strictEqual(res.expectedRollback, "full"); assert.strictEqual(res.actualRollback, "limited");
  const rec = broker.receipts().find((r) => r.type === "focus.set");
  assert.strictEqual(rec.undoState, "unavailable"); assert.strictEqual(rec.undoable, false);
  const u = await broker.undoProposal(p.id);
  assert.strictEqual(u.ok, false); assert.ok(u.steps.some((s) => s.reason === "not-undoable"));
});
test("irreversible and unsupported actions never claim rollback", async () => {
  const send = act("share.send", { id: "report", to: "a friend" });
  const { broker, propose, approve } = setup({}, { "share.send": async () => ({ result: "sent" }) });
  const p = propose({ steps: [act("focus.set", { enabled: true }), send] });
  assert.strictEqual(p.preview.expectedRollback, "limited");
  assert.strictEqual(propose(send).preview.expectedRollback, "none");
  approve(p.id); const res = await broker.execute(p.id);
  assert.strictEqual(res.actualRollback, "limited");
  assert.strictEqual(broker.receipts().find((r) => r.type === "share.send").undoState, "unavailable");
});
test("undo states: used, double sequential, failed undo is reported and not retried", async () => {
  const { broker, run, state } = setup({});
  const { id } = await run(act("file.move", { id: "report", destination: "study" }));
  const rid = broker.receipts()[0].id;
  assert.deepStrictEqual(await broker.undo(rid), { ok: true });
  assert.strictEqual(state.files.report.space, "work");
  assert.strictEqual((await broker.undo(rid)).reason, "already-undone");
  assert.strictEqual(broker.receipts()[0].undoState, "used");
  assert.strictEqual((await broker.undo("nope")).reason, "not-found");
  const f = setup({}, { "focus.set": async () => ({ undo: async () => { throw new Error("cannot undo"); } }) });
  await f.run(act("focus.set", { enabled: true }));
  const frid = f.broker.receipts()[0].id;
  assert.strictEqual((await f.broker.undo(frid)).reason, "undo-failed");
  assert.strictEqual(f.broker.receipts()[0].undoState, "failed");
  assert.strictEqual((await f.broker.undo(frid)).reason, "undo-failed-before");
  assert.ok(id);
});
test("simultaneous double undo runs the undo exactly once", async () => {
  let runs = 0; const gate = defer();
  const { broker, run } = setup({}, { "focus.set": async () => ({ undo: async () => { runs++; await gate.promise; } }) });
  await run(act("focus.set", { enabled: true }));
  const rid = broker.receipts()[0].id;
  const a = broker.undo(rid), b = broker.undo(rid);
  gate.resolve();
  const rs = await Promise.all([a, b]);
  assert.strictEqual(runs, 1);
  assert.deepStrictEqual(rs.map((r) => r.ok).sort(), [false, true]);
  assert.strictEqual(rs.find((r) => !r.ok).reason, "busy");
});
test("delete goes to Trail and can be undone", async () => {
  const { broker, run, state } = setup({});
  await run(delReport()); assert.deepStrictEqual(state.trail, ["report"]);
  await broker.undo(broker.receipts()[0].id);
  assert.ok(state.files.report); assert.deepStrictEqual(state.trail, []);
});

// ---------------- concurrency & cancellation ----------------
test("two simultaneous plans: second is rejected as busy, never interleaved; recovers afterwards", async () => {
  const gate = defer(); const order = [];
  const { broker, propose, approve } = setup({}, { "space.switch": async ({ space }) => { order.push("space:" + space); await gate.promise; return { undo() {} }; } });
  const p1 = propose(studyPlan()), p2 = propose(studyPlan()); approve(p1.id); approve(p2.id);
  const r1 = broker.execute(p1.id);
  const r2 = await broker.execute(p2.id);
  assert.deepStrictEqual(r2, { ok: false, reason: "busy" });
  assert.strictEqual(broker.getProposal(p2.id).state, "APPROVED"); // untouched, still approved
  gate.resolve(); assert.strictEqual((await r1).state, "COMPLETED");
  assert.strictEqual((await broker.execute(p2.id)).state, "COMPLETED");
  assert.deepStrictEqual(order, ["space:study", "space:study"]);
});
test("a single action, an undo and a second approval-consumption are refused during an active plan", async () => {
  const gate = defer();
  const { broker, propose, approve, run } = setup({}, { "app.open": async () => { await gate.promise; return { undo() {} }; } });
  await run(act("focus.set", { enabled: true })); const rid = broker.receipts()[0].id;
  const p = propose(studyPlan()); approve(p.id);
  const single = propose(act("search.query", { query: "x" }));
  const running = broker.execute(p.id);
  await new Promise((r) => setTimeout(r, 5));
  assert.strictEqual((await broker.execute(single.id)).reason, "busy");
  assert.strictEqual((await broker.undo(rid)).reason, "busy");
  assert.strictEqual((await broker.execute(p.id)).reason, "busy"); // same approval cannot be consumed twice
  gate.resolve(); assert.strictEqual((await running).state, "COMPLETED");
  assert.strictEqual((await broker.execute(single.id)).state, "COMPLETED");
});
test("concurrent approval consumption: exactly one of two simultaneous executes runs", async () => {
  const { broker, propose, approve, calls } = setup({});
  const p = propose(act("file.move", { id: "report", destination: "study" })); approve(p.id);
  const rs = await Promise.all([broker.execute(p.id), broker.execute(p.id)]);
  assert.deepStrictEqual(rs.map((r) => r.ok).sort(), [false, true]);
  assert.strictEqual(calls.filter((c) => c === "file.move").length, 1);
});
test("cancellation while an adapter is pending: step finishes, later steps do not run, state CANCELLED", async () => {
  const gate = defer();
  const { broker, propose, approve, state } = setup({}, { "focus.set": async () => { await gate.promise; return { undo() {} }; } });
  const p = propose(studyPlan()); approve(p.id);
  const running = broker.execute(p.id);
  await new Promise((r) => setTimeout(r, 5));
  assert.deepStrictEqual(broker.cancel(p.id), { ok: true, pending: true });
  gate.resolve();
  const res = await running;
  assert.strictEqual(res.state, "CANCELLED"); assert.strictEqual(res.ok, false);
  assert.strictEqual(state.timer, null); assert.strictEqual(state.openApp, null);
  assert.strictEqual(res.actualRollback, "full");
  assert.strictEqual((await broker.undoProposal(p.id)).ok, true);
});
test("cancel before run, and cancel of finished proposals", async () => {
  const { broker, propose, approve } = setup({});
  const p = propose(studyPlan()); approve(p.id);
  assert.deepStrictEqual(broker.cancel(p.id), { ok: true });
  assert.strictEqual((await broker.execute(p.id)).reason, "approval-required");
  assert.strictEqual(broker.cancel(p.id).reason, "already-cancelled");
  assert.strictEqual(broker.cancel("nope").reason, "not-found");
});
test("a throwing adapter or observer never wedges the broker", async () => {
  const { broker, propose, approve } = setup({}, { "timer.start": async () => { throw new Error("x"); } });
  const p = propose(studyPlan()); approve(p.id);
  const res = await broker.execute(p.id, { onProgress: () => { throw new Error("observer"); } });
  assert.strictEqual(res.state, "FAILED");
  const q = propose(act("app.open", { app: "files" }));
  assert.strictEqual((await broker.execute(q.id)).state, "COMPLETED"); // not stuck busy
});
test("reject, repeated execution of equal plans, and receipt immutability", async () => {
  const { broker, ui, propose, approve } = setup({});
  const r = propose(studyPlan()); assert.ok(ui.reject(r.id).ok);
  assert.strictEqual((await broker.execute(r.id)).reason, "approval-required");
  for (let i = 0; i < 2; i++) { const p = propose(studyPlan()); approve(p.id); assert.strictEqual((await broker.execute(p.id)).state, "COMPLETED"); }
  const rs = broker.receipts(); assert.strictEqual(rs.length, 8);
  rs[0].status = "hacked"; rs[0].undoState = "available";
  assert.strictEqual(broker.receipts()[0].status, "done");
  const view = broker.getProposal(r.id); view.steps[0].arguments.space = "work";
  assert.strictEqual(broker.getProposal(r.id).steps[0].arguments.space, "study");
});

// ---------------- Quick Commands ----------------
test("Quick Commands parse to typed, schema-valid actions that the broker accepts as proposals", () => {
  const { broker } = setup({});
  const cases = [
    ["Open Notes", act("app.open", { app: "notes" })], ["open the calculator app", act("app.open", { app: "calculator" })],
    ["Start a 25 minute timer", act("timer.start", { seconds: 1500 })], ["start a 90 second timer.", act("timer.start", { seconds: 90 })],
    ["start an 1 hour timer", act("timer.start", { seconds: 3600 })], ["Go to Study", act("space.switch", { space: "study" })],
    ["switch to the work space", act("space.switch", { space: "work" })], ["Turn on focus mode", act("focus.set", { enabled: true })],
    ["turn off focus", act("focus.set", { enabled: false })], ["Show my files", act("app.open", { app: "files" })],
    ["Search for my report", act("search.query", { query: "my report" })],
  ];
  cases.forEach(([text, want]) => {
    const r = B.parseCommand(text); assert.strictEqual(r.ok, true, text);
    assert.deepStrictEqual(r.action, want, text); assert.strictEqual(broker.proposer.propose(r.action).ok, true, text);
  });
  const plan = B.parseCommand("Prepare my phone for studying.");
  assert.strictEqual(plan.kind, "plan"); assert.strictEqual(broker.proposer.propose(plan.plan).ok, true);
});
test("Quick Commands reject unsupported, dangerous or out-of-range text and never emit S3", () => {
  ["", "   ", "rm -rf /", "open terminal", "delete all my files", "delete report", "send my report to bob", "remove everything",
    "start a 0 minute timer", "start a 9999 hour timer", "go to narnia", "format the disk && open notes", "open notes; open files",
    42, null, undefined, "x".repeat(300)].forEach((t) => assert.strictEqual(B.parseCommand(t).ok, false, String(t)));
  const r = B.parseCommand("Open Notes"); assert.deepStrictEqual(Object.keys(r.action).sort(), ["arguments", "type"]);
});

(async () => {
  console.log("== Action Broker ==");
  for (const t of queue) {
    try { await t.fn(); passed++; console.log("  PASS:", t.name); }
    catch (e) { failed++; console.log("  FAIL:", t.name, "\n   ", e.stack.split("\n").slice(0, 4).join("\n    ")); }
  }
  console.log(`\n== Summary ==\nPassed: ${passed}\nFailed: ${failed}`);
  process.exit(failed ? 1 : 0);
})();
