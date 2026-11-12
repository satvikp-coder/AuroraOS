/** tests/unit/passport.test.js — the DOM-free Passport model (aurora-glass/prototype/passport-model.js). */
const assert = require("assert");
const path = require("path");
const M = require(path.resolve(__dirname, "..", "..", "aurora-glass", "prototype", "passport-model.js"));

let passed = 0, failed = 0;
function test(name, fn) { try { fn(); passed++; console.log("  PASS:", name); } catch (e) { failed++; console.log("  FAIL:", name, "\n   ", e.message); } }

const CAT = [
  { id: "apps.open", risk: "S0", decision: "allow" }, { id: "spaces.switch", risk: "S0", decision: "allow" }, { id: "timer.control", risk: "S0", decision: "allow" },
  { id: "index.search", risk: "S0", decision: "allow" }, { id: "settings.write", risk: "S1", decision: "ask" }, { id: "files.edit", risk: "S2", decision: "ask" },
  { id: "files.delete", risk: "S3", decision: "ask" }, { id: "external.send", risk: "S3", decision: "deny" },
];

console.log("== classify / needsConfirm ==");
test("tightening, loosening and no-ops", () => {
  assert.strictEqual(M.classify("allow", "ask"), "tighten");
  assert.strictEqual(M.classify("allow", "deny"), "tighten");
  assert.strictEqual(M.classify("ask", "deny"), "tighten");
  assert.strictEqual(M.classify("deny", "ask"), "loosen");
  assert.strictEqual(M.classify("ask", "allow"), "loosen");
  assert.strictEqual(M.classify("deny", "allow"), "loosen");
  for (const d of M.DECISIONS) assert.strictEqual(M.classify(d, d), "same");
});
test("unknown or hostile decisions are 'same' so nothing is ever treated as a loosening by accident", () => {
  for (const bad of ["", "ALLOW", "constructor", "__proto__", null, undefined, 3, {}]) {
    assert.strictEqual(M.classify(bad, "allow"), "same", String(bad));
    assert.strictEqual(M.classify("ask", bad), "same", String(bad));
    assert.strictEqual(M.needsConfirm(bad, "allow"), false);
  }
});
test("only a move TO Allow needs the confirmation", () => {
  assert.strictEqual(M.needsConfirm("ask", "allow"), true);
  assert.strictEqual(M.needsConfirm("deny", "allow"), true);
  assert.strictEqual(M.needsConfirm("deny", "ask"), false);       // still asks you each time
  assert.strictEqual(M.needsConfirm("allow", "ask"), false);
  assert.strictEqual(M.needsConfirm("allow", "allow"), false);
});

console.log("\n== summarize ==");
test("counts and the sentence", () => {
  assert.deepStrictEqual(M.summarize(CAT), { allow: 4, ask: 3, deny: 1, sentence: "4 allowed, 3 ask each time, 1 blocked" });
  assert.strictEqual(M.summarize([]).sentence, "0 allowed, 0 ask each time, 0 blocked");
  assert.strictEqual(M.summarize(null).sentence, "0 allowed, 0 ask each time, 0 blocked");
  assert.strictEqual(M.summarize([{ decision: "nope" }, { decision: "toString" }]).sentence, "0 allowed, 0 ask each time, 0 blocked");
});

console.log("\n== apps and capabilities ==");
test("every capability in the catalogue is requestable by at least one app, and nothing refers to an unknown capability", () => {
  CAT.forEach((c) => assert.ok(M.appsUsing(c.id).length >= 1, c.id));
  M.APPS.forEach((a) => a.permissions.forEach((p) => assert.ok(M.ALL.includes(p), a.id + " -> " + p)));
});
test("Aura can request everything; Calculator and Calendar need nothing", () => {
  assert.deepStrictEqual(M.appById("aura").permissions, M.ALL);
  assert.deepStrictEqual(M.appById("calculator").permissions, []);
  assert.deepStrictEqual(M.appById("calendar").permissions, []);
  assert.strictEqual(M.appById("nope"), null);
  assert.strictEqual(M.appById("__proto__"), null);
});
test("a change to a capability reaches every app that uses it", () => {
  assert.deepStrictEqual(M.appsUsing("files.delete").sort(), ["aura", "files", "vault"]);
  assert.deepStrictEqual(M.appsUsing("timer.control").sort(), ["aura", "clock"]);
  assert.deepStrictEqual(M.appsUsing("nothing"), []);
});
test("exports are deeply frozen, so one caller cannot change what another sees", () => {
  assert.ok(Object.isFrozen(M.APPS) && Object.isFrozen(M.APPS[0]) && Object.isFrozen(M.APPS[0].permissions));
  assert.ok(Object.isFrozen(M.HARDWARE) && Object.isFrozen(M.HARDWARE[0]) && Object.isFrozen(M.ALL) && Object.isFrozen(M.DECISIONS) && Object.isFrozen(M.DECISION_WORD));
  assert.throws(() => { "use strict"; M.ALL.push("y"); }, TypeError);
  assert.throws(() => { "use strict"; M.APPS[0].permissions.push("x"); }, TypeError);
  assert.ok(!M.ALL.includes("y"));
});
test("hardware rows exist for camera, microphone and location", () => {
  assert.deepStrictEqual(M.HARDWARE.map((h) => h.id), ["camera", "microphone", "location"]);
});

console.log("\n== revoke ==");
test("revokeTargets: non-S0, not already denied", () => {
  assert.deepStrictEqual(M.revokeTargets("notes", CAT), ["files.edit"]);
  assert.deepStrictEqual(M.revokeTargets("files", CAT).sort(), ["files.delete", "files.edit"]);      // external.send already denied
  assert.deepStrictEqual(M.revokeTargets("clock", CAT), []);                                         // S0 only: nothing to revoke
  assert.deepStrictEqual(M.revokeTargets("calculator", CAT), []);
  assert.deepStrictEqual(M.revokeTargets("aura", CAT).sort(), ["files.delete", "files.edit", "settings.write"]);
  assert.deepStrictEqual(M.revokeTargets("nope", CAT), []);
});
test("revokeTargets follows the live decisions", () => {
  const c = CAT.map((x) => (x.id === "files.edit" ? Object.assign({}, x, { decision: "deny" }) : x));
  assert.deepStrictEqual(M.revokeTargets("notes", c), []);
});
test("sharedWith lists the OTHER apps a revoke would also affect", () => {
  assert.deepStrictEqual(M.sharedWith("notes", ["files.edit"]), { "files.edit": ["aura", "files", "vault"] });
  assert.deepStrictEqual(M.sharedWith("vault", ["files.delete"]), { "files.delete": ["aura", "files"] });
  assert.deepStrictEqual(M.sharedWith("aura", ["timer.control"]), { "timer.control": ["clock"] });
  assert.deepStrictEqual(M.sharedWith("notes", []), {});
});

console.log("\n== consequence text ==");
test("every capability has a plain-language consequence", () => {
  M.ALL.forEach((id) => assert.ok(M.consequence(id) !== "do this", id));
  assert.strictEqual(M.consequence("nope"), "do this");
  assert.strictEqual(M.consequence("__proto__"), "do this");
});

console.log("\n== Results ==");
console.log("Passed:", passed);
console.log("Failed:", failed);
process.exit(failed ? 1 : 0);
