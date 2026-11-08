/** tests/unit/settings.test.js — the Aurora Glass settings registry (DOM-free, simulation only). */
const assert = require("assert");
const path = require("path");
const S = require(path.resolve(__dirname, "..", "..", "aurora-glass", "prototype", "settings.js"));

let passed = 0, failed = 0;
function test(name, fn) {
  try { fn(); passed++; console.log("  PASS:", name); }
  catch (e) { failed++; console.log("  FAIL:", name, "\n   ", e.message); }
}

console.log("== Settings registry ==");

test("every required section exists and has at least one entry", () => {
  const s = S.createSettings();
  ["appearance", "accessibility", "notifications", "privacy", "battery", "storage", "aura", "developer"].forEach((id) => {
    assert.ok(s.sections.some((x) => x.id === id), id);
    assert.ok(s.list(id, true).length >= 1, id + " has entries");
  });
});
test("ids are unique, each entry belongs to a known section and has an honest effect label", () => {
  const s = S.createSettings(); const seen = new Set();
  s.all().forEach((e) => {
    assert.ok(!seen.has(e.id), "duplicate " + e.id); seen.add(e.id);
    assert.ok(s.sections.some((x) => x.id === e.section), e.id);
    assert.ok(["live", "stored", "locked"].includes(e.effect), e.id);
    if (e.type === "info") assert.strictEqual(e.effect, "locked");
  });
});
test("defaults are valid for their own type", () => {
  const s = S.createSettings();
  s.all().filter((e) => e.type !== "info").forEach((e) => {
    const copy = S.createSettings();
    assert.strictEqual(copy.set(e.id, e.value).ok, true, e.id);
  });
});
test("one registry feeds both views: advanced entries are hidden in the simple list, present in the full list", () => {
  const s = S.createSettings();
  const simple = s.list("privacy", false).map((e) => e.id), full = s.list("privacy", true).map((e) => e.id);
  assert.ok(!simple.includes("privacy.auraHistory")); assert.ok(full.includes("privacy.auraHistory"));
  simple.forEach((id) => assert.ok(full.includes(id)));
  assert.strictEqual(s.entry("privacy.auraHistory").advanced, true);
});
test("set validates type, choices and ranges; failures change nothing", () => {
  const s = S.createSettings();
  assert.deepStrictEqual(s.set("appearance.theme", "dark"), { ok: true, value: "dark" });
  assert.strictEqual(s.set("appearance.theme", "sepia").reason, "invalid-value");
  assert.strictEqual(s.set("appearance.theme", 1).reason, "invalid-value");
  assert.strictEqual(s.get("appearance.theme"), "dark");
  assert.strictEqual(s.set("accessibility.highContrast", "true").reason, "invalid-value");
  assert.strictEqual(s.set("accessibility.highContrast", 1).reason, "invalid-value");
  assert.strictEqual(s.set("accessibility.textSize", 150).reason, "invalid-value");      // choices are strings
  assert.strictEqual(s.set("accessibility.textSize", "150").ok, true);
  assert.strictEqual(s.set("storage.trailDays", 6).reason, "invalid-value");
  assert.strictEqual(s.set("storage.trailDays", 91).reason, "invalid-value");
  assert.strictEqual(s.set("storage.trailDays", 30.5).reason, "invalid-value");
  assert.strictEqual(s.set("storage.trailDays", NaN).reason, "invalid-value");
  assert.strictEqual(s.set("storage.trailDays", "30").reason, "invalid-value");
  assert.strictEqual(s.set("storage.trailDays", 90).ok, true); assert.strictEqual(s.get("storage.trailDays"), 90);
});
test("locked and info entries cannot be changed (S2/S3 approval is not a setting)", () => {
  const s = S.createSettings();
  const before = s.get("aura.approval");
  ["aura.approval", "storage.usage"].forEach((id) => {
    ["Never required", "", true, null].forEach((v) => assert.strictEqual(s.set(id, v).reason, "read-only"));
  });
  assert.strictEqual(s.get("aura.approval"), before);
  assert.ok(/cannot be turned off/i.test(before));
  assert.ok(!(("aura.approval") in s.snapshot()), "info entries are not exported as settings");
});
test("unknown ids and prototype-property names are refused and never pollute", () => {
  const s = S.createSettings();
  ["__proto__", "constructor", "toString", "hasOwnProperty", "", "nope", "appearance", null, undefined, 5, {}].forEach((id) => {
    assert.strictEqual(s.set(id, true).reason, "unknown-setting", String(id));
    assert.strictEqual(s.get(id), undefined, String(id));
    assert.strictEqual(s.entry(id), null, String(id));
  });
  assert.strictEqual({}.polluted, undefined);
});
test("onChange fires only for real changes, with old and new values", () => {
  const s = S.createSettings(); const seen = [];
  s.onChange((id, v, o) => seen.push([id, v, o]));
  s.set("battery.saver", false); s.set("battery.saver", true); s.set("battery.saver", true);
  assert.deepStrictEqual(seen, [["battery.saver", true, false]]);
});
test("reset restores every default and reports each change; locked values are untouched", () => {
  const s = S.createSettings(); let n = 0;
  s.set("appearance.theme", "dark"); s.set("accessibility.textSize", "200"); s.set("storage.trailDays", 10); s.set("aura.mode", "off");
  s.onChange(() => n++);
  s.reset();
  assert.strictEqual(n, 4);
  assert.deepStrictEqual(s.snapshot(), S.createSettings().snapshot());
  assert.ok(/cannot be turned off/i.test(s.get("aura.approval")));
});
test("entries are copies: mutating them never changes the registry", () => {
  const s = S.createSettings();
  const e = s.entry("appearance.theme"); e.value = "dark"; e.options.push("sepia");
  assert.strictEqual(s.get("appearance.theme"), "light");
  assert.deepStrictEqual(s.entry("appearance.theme").options, ["light", "dark"]);
  const snap = s.snapshot(); snap["appearance.theme"] = "dark";
  assert.strictEqual(s.get("appearance.theme"), "light");
});
test("search finds EVERY entry, including advanced ones, by label, section or keyword", () => {
  const s = S.createSettings();
  s.all().forEach((e) => {
    const hit = s.search(e.label).map((x) => x.id);
    assert.ok(hit.includes(e.id), "label search must find " + e.id);
    e.keywords.forEach((k) => assert.ok(s.search(k).map((x) => x.id).includes(e.id), "keyword '" + k + "' must find " + e.id));
  });
  const sectionHit = s.search("battery").map((x) => x.id); assert.ok(sectionHit.includes("battery.saver"));
  assert.ok(s.search("history").map((x) => x.id).includes("privacy.auraHistory"));      // advanced
});
test("search ranking and input handling", () => {
  const s = S.createSettings();
  assert.strictEqual(s.search("theme")[0].id, "appearance.theme");
  assert.strictEqual(s.search("  THEME ")[0].id, "appearance.theme");
  assert.deepStrictEqual(s.search(""), []); assert.deepStrictEqual(s.search("   "), []);
  [null, undefined, 5, {}, []].forEach((q) => assert.deepStrictEqual(s.search(q), []));
  assert.deepStrictEqual(s.search("zzzz-not-a-setting"), []);
  assert.deepStrictEqual(s.search("<img src=x onerror=alert(1)>"), []);                  // text only, never interpreted
  assert.ok(s.search("dark").some((e) => e.id === "appearance.theme"));
  assert.ok(s.search("text").map((e) => e.id).includes("accessibility.textSize"));
});
test("snapshot contains exactly the changeable settings", () => {
  const s = S.createSettings(); const snap = s.snapshot();
  s.all().forEach((e) => assert.strictEqual(e.id in snap, e.type !== "info"));
  JSON.parse(JSON.stringify(snap));
});

console.log(`\n== Summary ==\nPassed: ${passed}\nFailed: ${failed}`);
process.exit(failed ? 1 : 0);
