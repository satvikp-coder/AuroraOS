/**
 * tests/unit/system.test.js — aurora-glass/prototype/system.js without a browser.
 * system.js is an IIFE that publishes window.AuroraSystem. It is loaded into a fresh vm context with the real broker
 * and small fakes for the shell and the models, so the permission policy, describe/explain text, the one-time trusted
 * surface and the mock-file adapters are checked as plain code.
 */
const assert = require("assert");
const fs = require("fs");
const path = require("path");
const vm = require("vm");

const PROTO = path.resolve(__dirname, "..", "..", "aurora-glass", "prototype");
const Broker = require(path.join(PROTO, "broker.js"));
const SYSTEM_SRC = fs.readFileSync(path.join(PROTO, "system.js"), "utf8");

let passed = 0, failed = 0;
async function test(name, fn) {
  try { await fn(); passed++; console.log("  PASS:", name); }
  catch (e) { failed++; console.log("  FAIL:", name, "\n   ", e && e.message); }
}
const plain = (x) => JSON.parse(JSON.stringify(x));      // vm contexts have their own Object/Array prototypes

/** A fresh AuroraSystem each time, wired to fakes. */
function load(opts) {
  const o = opts || {};
  const calls = { open: [], switched: [], closed: [] };
  const spaces = o.spaces || [{ id: "study", name: "Study" }, { id: "work", name: "Work" }, { id: "projects", name: "Projects" }, { id: "personal", name: "Personal" }];
  const qsValues = { focus: false };
  const window = {
    AuroraBroker: Broker,
    AuroraShell: o.noSpaces ? {} : {
      state: { activeSpace: "study" },
      spaces: () => spaces,
      openApp: (app) => { calls.open.push(app); return { ok: true, id: "page-" + app, created: true }; },
      switchSpace: (id) => { calls.switched.push(id); return { ok: true }; },
      openIndex: () => {},
    },
    AuroraModels: { pages: { close: (id) => { calls.closed.push(id); } }, qs: { get: (k) => qsValues[k], set: (k, v) => { qsValues[k] = v; } } },
  };
  vm.runInNewContext(SYSTEM_SRC, { window, setTimeout, clearTimeout, Date }, { filename: "system.js" });
  const S = window.AuroraSystem;
  S.dev.stepDelayMs = 0;
  return { S, calls, window };
}

(async () => {
  console.log("== AuroraSystem: permission policy ==");

  await test("defaults match the catalogue and S3 permissions cannot be pre-approved", () => {
    const { S } = load();
    const cat = plain(S.permissions.catalog());
    const byId = Object.fromEntries(cat.map((p) => [p.id, p]));
    assert.strictEqual(byId["apps.open"].decision, "allow");
    assert.strictEqual(byId["settings.write"].decision, "ask");
    assert.strictEqual(byId["files.delete"].decision, "ask");
    assert.strictEqual(byId["external.send"].decision, "deny");
    assert.strictEqual(byId["files.delete"].canAllow, false);
    assert.strictEqual(byId["external.send"].canAllow, false);
    assert.strictEqual(byId["files.edit"].canAllow, true);
    assert.strictEqual(byId["external.send"].supported, false, "sending outside the device has no adapter");
    assert.strictEqual(byId["files.delete"].supported, true);
  });

  await test("set(): S3 never Allow, unknown ids, inherited names and bad values are refused", () => {
    const { S } = load();
    const P = S.permissions;
    assert.deepStrictEqual(plain(P.set("files.delete", "allow")), { ok: false, reason: "always-ask" });
    assert.deepStrictEqual(plain(P.set("external.send", "allow")), { ok: false, reason: "always-ask" });
    for (const id of ["nope", "__proto__", "constructor", "toString", "", null, 5]) assert.strictEqual(P.set(id, "allow").reason, "unknown-permission", String(id));
    for (const d of ["yes", "ALLOW", "", null, undefined, 1, {}]) assert.strictEqual(P.set("files.edit", d).reason, "invalid-decision", String(d));
    assert.strictEqual(P.log().length, 0, "refused changes are not logged");
  });

  await test("set(): accepted changes are logged with from/to; unchanged is a no-op", () => {
    const { S } = load();
    const P = S.permissions;
    assert.deepStrictEqual(plain(P.set("files.edit", "ask")), { ok: true, unchanged: true });
    assert.strictEqual(P.set("files.edit", "allow").ok, true);
    assert.strictEqual(P.set("files.edit", "deny").ok, true);
    assert.strictEqual(P.set("files.delete", "deny").ok, true);
    assert.deepStrictEqual(plain(P.log().map((e) => [e.permission, e.from, e.to])), [["files.edit", "ask", "allow"], ["files.edit", "allow", "deny"], ["files.delete", "ask", "deny"]]);
    P.log()[0].to = "tampered";
    assert.strictEqual(P.log()[0].to, "allow", "log() returns copies");
  });

  await test("resetAll() restores defaults through the same policy (and logs each change)", () => {
    const { S } = load();
    S.permissions.set("files.edit", "allow"); S.permissions.set("settings.write", "deny");
    S.permissions.resetAll();
    assert.strictEqual(S.permissions.get("files.edit"), "ask");
    assert.strictEqual(S.permissions.get("settings.write"), "ask");
    assert.strictEqual(S.permissions.log().length, 4);
  });

  await test("catalog() and defaults() return copies", () => {
    const { S } = load();
    S.permissions.catalog()[0].label = "x"; S.permissions.defaults()["apps.open"] = "deny";
    assert.notStrictEqual(S.permissions.catalog()[0].label, "x");
    assert.strictEqual(S.permissions.defaults()["apps.open"], "allow");
  });

  console.log("\n== AuroraSystem: describe / explain / spaceName ==");

  await test("describe() titles for every action type", () => {
    const { S } = load();
    const d = (type, a) => plain(S.describe({ type, arguments: a }));
    assert.strictEqual(d("app.open", { app: "notes" }).title, "Open Notes");
    assert.strictEqual(d("space.switch", { space: "work" }).title, "Switch to Work Space");
    assert.strictEqual(d("timer.start", { seconds: 600 }).title, "Start a timer for 10 minutes");
    assert.strictEqual(d("timer.start", { seconds: 60 }).title, "Start a timer for 1 minute");
    assert.strictEqual(d("timer.start", { seconds: 90 }).title, "Start a timer for 90 seconds");
    assert.strictEqual(d("timer.start", { seconds: 7200 }).title, "Start a timer for 2 hours");
    assert.strictEqual(d("search.query", { query: "cats" }).title, "Search for “cats”");
    assert.strictEqual(d("focus.set", { enabled: true }).title, "Turn Focus mode on");
    assert.strictEqual(d("focus.set", { enabled: false }).title, "Turn Focus mode off");
    assert.strictEqual(d("file.move", { id: "timetable", destination: "work" }).title, "Move “Exam timetable.txt” to Work");
    assert.strictEqual(d("file.delete", { id: "timetable" }).title, "Delete “Exam timetable.txt”");
    assert.strictEqual(d("share.send", { id: "timetable", to: "me" }).title, "Send “Exam timetable.txt” to me");
    assert.strictEqual(d("nope", {}).title, "Unknown action");
    assert.strictEqual(plain(S.describe(null)).title, "Unknown action");
  });

  await test("describe() keeps hostile text as text and never throws on odd input", () => {
    const { S } = load();
    assert.strictEqual(plain(S.describe({ type: "search.query", arguments: { query: "<img src=x onerror=1>" } })).title, "Search for “<img src=x onerror=1>”");
    assert.doesNotThrow(() => { S.describe({ type: "file.delete" }); S.describe({}); S.describe(undefined); S.describe({ type: "file.delete", arguments: { id: "__proto__" } }); });
  });

  await test("space names follow a renamed Space and fall back to the built-in name", () => {
    const renamed = load({ spaces: [{ id: "study", name: "Exams" }, { id: "work", name: "Work" }] }).S;
    assert.strictEqual(renamed.spaceName("study"), "Exams");
    assert.strictEqual(plain(renamed.describe({ type: "space.switch", arguments: { space: "study" } })).title, "Switch to Exams Space");
    assert.strictEqual(plain(renamed.describe({ type: "file.move", arguments: { id: "timetable", destination: "study" } })).title, "Move “Exam timetable.txt” to Exams");
    const noShell = load({ noSpaces: true }).S;
    assert.strictEqual(noShell.spaceName("study"), "Study");
    assert.strictEqual(noShell.spaceName("projects"), "Projects");
    assert.strictEqual(noShell.spaceName("space-custom"), "space-custom");
    assert.strictEqual(noShell.spaceName("__proto__"), "__proto__");
  });

  await test("explain() gives a sentence for known codes and hides internals for unknown ones", () => {
    const { S } = load();
    assert.strictEqual(S.explain("simulated-failure"), "Simulated failure (developer option).");
    assert.strictEqual(S.explain("no-such-file"), "That file no longer exists.");
    assert.strictEqual(S.explain("permission-denied:files.delete"), "Blocked by Passport: “Delete files” is set to Deny.");
    assert.strictEqual(S.explain("permission-changed:settings.write"), "“Change settings” now needs your approval. Review it again.");
    assert.strictEqual(S.explain("permission-denied:unknown.thing"), "Blocked by Passport: “unknown.thing” is set to Deny.");
    assert.strictEqual(S.explain(""), "Something went wrong.");
    assert.strictEqual(S.explain(null), "Something went wrong.");
    assert.strictEqual(S.explain(42), "Something went wrong.");
    assert.strictEqual(S.explain("toString"), "Could not complete (toString).");
    assert.strictEqual(S.explain("x".repeat(100)), "Could not complete (" + "x".repeat(40) + ").");
  });

  console.log("\n== AuroraSystem: trusted surface and the mock world ==");

  await test("request() has no review surface until review.js takes it", async () => {
    const { S } = load();
    assert.deepStrictEqual(plain(await S.request({ type: "app.open", arguments: { app: "notes" } })), { ok: false, reason: "no-review-surface" });
  });

  await test("the trusted facet can be taken exactly once; the public proposer cannot approve or execute", () => {
    const { S } = load();
    const t = S.takeTrusted(() => {});
    assert.throws(() => S.takeTrusted(() => {}), /trusted-surface-already-taken/);
    assert.ok(Object.isFrozen(t));
    for (const k of ["approve", "execute", "cancel", "undo"]) assert.strictEqual(S.proposer[k], undefined, "proposer." + k);
    assert.ok(typeof S.proposer.propose === "function" && typeof S.proposer.preview === "function");
  });

  await test("a run produces a receipt joined with its action, description and source; undo restores the world", async () => {
    const { S, calls } = load();
    const t = S.takeTrusted(() => {});
    const p = S.proposer.propose({ type: "timer.start", arguments: { seconds: 90 } }).proposal;
    const res = await t.execute(p.id, {});
    assert.strictEqual(res.ok, true);
    t.noteRun(p, res, { source: "unit", requestText: "start 90s" });
    assert.strictEqual(S.world.timer.seconds, 90);
    const r = S.receipts()[0];
    assert.strictEqual(r.describe.title, "Start a timer for 90 seconds");
    assert.strictEqual(r.source, "unit");
    assert.strictEqual(r.requestText, "start 90s");
    assert.strictEqual(r.status, "done");
    assert.strictEqual(r.undoState, "available");
    const u = await S.undo(r.id);
    assert.strictEqual(u.ok, true);
    assert.strictEqual(S.world.timer, null);
    assert.strictEqual(S.receipts()[0].undoState, "used");
    const again = await S.undo(r.id);
    assert.strictEqual(again.ok, false);
    assert.strictEqual(again.text, "This was already undone.");
    assert.deepStrictEqual(calls.closed, []);
  });

  await test("app.open undo closes the page only when this run created it", async () => {
    const { S, calls } = load();
    const t = S.takeTrusted(() => {});
    const p = S.proposer.propose({ type: "app.open", arguments: { app: "calendar" } }).proposal;
    const res = await t.execute(p.id, {});
    t.noteRun(p, res, {});
    assert.deepStrictEqual(calls.open, ["calendar"]);
    await S.undo(S.receipts()[0].id);
    assert.deepStrictEqual(calls.closed, ["page-calendar"]);
  });

  await test("file.delete moves a mock file to Trail; undo restores it; a vanished Trail entry makes undo fail honestly", async () => {
    const { S } = load();
    const t = S.takeTrusted(() => {});
    const delta = async () => {
      const p = S.proposer.propose({ type: "file.delete", arguments: { id: "timetable" } }).proposal;
      assert.deepStrictEqual(plain(p.preview.s3Indices), [0]);
      assert.strictEqual(t.ui.approve(p.id, { digest: p.digest, confirmS3: p.preview.s3Indices }).ok, true);
      const res = await t.execute(p.id, {}); t.noteRun(p, res, {}); return res;
    };
    assert.strictEqual((await delta()).ok, true);
    assert.strictEqual(S.files.get("timetable"), null);
    assert.deepStrictEqual(plain(S.files.deleted().map((f) => f.id)), ["timetable"]);
    assert.strictEqual((await S.undo(S.receipts()[0].id)).ok, true);
    assert.strictEqual(S.files.get("timetable").name, "Exam timetable.txt");
    assert.strictEqual(S.files.deleted().length, 0);
    await delta();
    delete S.world.deleted["timetable"];
    const bad = await S.undo(S.receipts()[0].id);
    assert.strictEqual(bad.ok, false);
    assert.strictEqual(S.receipts()[0].undoState, "failed");
    assert.strictEqual(S.files.get("timetable"), null);
  });

  await test("S3 needs the confirmation of exactly the S3 steps; an S3 approve without it is refused", () => {
    const { S } = load();
    const t = S.takeTrusted(() => {});
    const p = S.proposer.propose({ type: "file.delete", arguments: { id: "timetable" } }).proposal;
    assert.strictEqual(t.ui.approve(p.id, { digest: p.digest }).ok, false);
    assert.strictEqual(t.ui.approve(p.id, { digest: p.digest, confirmS3: [] }).ok, false);
    assert.strictEqual(t.ui.approve(p.id, { digest: "wrong", confirmS3: [0] }).ok, false);
  });

  await test("ids that name inherited properties never resolve to a file", async () => {
    const { S } = load();
    const t = S.takeTrusted(() => {});
    for (const id of ["constructor", "toString", "hasOwnProperty", "valueOf"]) {
      assert.strictEqual(S.files.get(id), null, id);
      const p = S.proposer.propose({ type: "file.move", arguments: { id, destination: "work" } }).proposal;
      t.ui.approve(p.id, { digest: p.digest });
      const res = await t.execute(p.id, {});
      assert.strictEqual(res.ok, false, id);
      t.noteRun(p, res, {});
      assert.strictEqual(S.receipts()[0].status, "failed", id);
      assert.strictEqual(S.receipts()[0].errorText, "That file no longer exists.", id);
    }
  });

  await test("a permission set to Deny after approval makes execution refuse and nothing changes", async () => {
    const { S } = load();
    const t = S.takeTrusted(() => {});
    const p = S.proposer.propose({ type: "file.move", arguments: { id: "timetable", destination: "work" } }).proposal;
    assert.strictEqual(t.ui.approve(p.id, { digest: p.digest }).ok, true);
    S.permissions.set("files.edit", "deny");
    const res = await t.execute(p.id, {});
    assert.strictEqual(res.ok, false);
    assert.strictEqual(S.files.get("timetable").space, "study");
    assert.strictEqual(S.receipts().length === 0 || S.receipts()[0].status !== "done", true);
  });

  await test("share.send has no adapter: it previews as unsupported and cannot be executed", async () => {
    const { S } = load();
    const t = S.takeTrusted(() => {});
    const p = S.proposer.propose({ type: "share.send", arguments: { id: "timetable", to: "me" } }).proposal;
    assert.strictEqual(p.preview.steps[0].supported, false);
    t.ui.approve(p.id, { digest: p.digest, confirmS3: p.preview.s3Indices });
    const res = await t.execute(p.id, {});
    assert.strictEqual(res.ok, false);
  });

  await test("files API returns copies and the file table has no prototype", () => {
    const { S } = load();
    const f = S.files.get("timetable"); f.name = "x";
    assert.strictEqual(S.files.get("timetable").name, "Exam timetable.txt");
    S.files.list()[0].name = "y";
    assert.notStrictEqual(S.files.list()[0].name, "y");
    assert.strictEqual(Object.getPrototypeOf(S.world.files), null);
    assert.strictEqual(S.files.get(null), null);
    assert.strictEqual(S.files.get(5), null);
  });

  await test("a throwing event listener does not stop the others", () => {
    const { S } = load();
    let heard = 0;
    S.on("ping", () => { throw new Error("boom"); });
    S.on("ping", () => { heard++; });
    assert.doesNotThrow(() => S.emit("ping", 1));
    assert.strictEqual(heard, 1);
  });

  await test("the developer failure hook fails exactly one matching step", async () => {
    const { S } = load();
    const t = S.takeTrusted(() => {});
    S.dev.failNextType = "timer.start";
    const p1 = S.proposer.propose({ type: "timer.start", arguments: { seconds: 60 } }).proposal;
    const r1 = await t.execute(p1.id, {}); t.noteRun(p1, r1, {});
    assert.strictEqual(r1.ok, false);
    assert.strictEqual(S.receipts()[0].errorText, "Simulated failure (developer option).");
    assert.strictEqual(S.dev.failNextType, null);
    const p2 = S.proposer.propose({ type: "timer.start", arguments: { seconds: 61 } }).proposal;
    assert.strictEqual((await t.execute(p2.id, {})).ok, true);
  });

  console.log("\n== Results ==");
  console.log("Passed:", passed);
  console.log("Failed:", failed);
  process.exit(failed ? 1 : 0);
})();
