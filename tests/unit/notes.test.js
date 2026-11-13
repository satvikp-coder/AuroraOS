/** tests/unit/notes.test.js - the DOM-free Notes model and its storage envelope (aurora-glass/prototype/notes-model.js). */
const assert = require("assert");
const path = require("path");
const M = require(path.resolve(__dirname, "..", "..", "aurora-glass", "prototype", "notes-model.js"));

let passed = 0, failed = 0;
function test(name, fn) { try { fn(); passed++; console.log("  PASS:", name); } catch (e) { failed++; console.log("  FAIL:", name, "\n   ", e.message); } }

/** Fake storage with failure injection. */
function fakeStorage(init) {
  const data = new Map(Object.entries(init || {}));
  const s = { data, failSet: null, failGet: null, sets: 0,
    getItem(k) { if (s.failGet) throw s.failGet; return data.has(k) ? data.get(k) : null; },
    setItem(k, v) { if (s.failSet) throw s.failSet; s.sets++; data.set(k, String(v)); },
    removeItem(k) { data.delete(k); } };
  return s;
}
const quota = () => { const e = new Error("full"); e.name = "QuotaExceededError"; e.code = 22; return e; };
function clock(start) { let t = start || 1_700_000_000_000; const f = () => t; f.tick = (ms) => { t += ms; return t; }; return f; }
function make(storage, now) { const c = now || clock(); const st = M.createStore({ storage, now: c }); st.load(); return { st, c, storage }; }
const note = (o) => Object.assign({ id: "a1", title: "T", body: "B", pinned: false, space: "study", created: 1, updated: 2 }, o || {});
const envelope = (notes) => JSON.stringify({ v: 1, notes });

console.log("Notes model");

test("constants match the plan: key, version, limits", () => {
  assert.strictEqual(M.KEY, "aurora-glass.notes.v1");
  assert.strictEqual(M.VERSION, 1);
  assert.deepStrictEqual(Object.assign({}, M.LIMITS), { title: 120, body: 20000, notes: 200 });
  assert.ok(Object.isFrozen(M.LIMITS));
});

test("create: defaults, unique ids, timestamps from the injected clock", () => {
  const { st, c } = make(fakeStorage());
  const a = st.create({ space: "work" }), b = st.create({ space: "work" });
  assert.ok(a.ok && b.ok);
  assert.notStrictEqual(a.note.id, b.note.id);
  assert.deepStrictEqual(Object.assign({}, a.note, { id: "x" }), { id: "x", title: "", body: "", pinned: false, space: "work", created: c(), updated: c() });
  assert.strictEqual(st.count(), 2);
});

test("create: a colliding id generator is retried, never duplicated", () => {
  const st = M.createStore({ storage: fakeStorage(), newId: () => "same" }); st.load();
  const a = st.create({}), b = st.create({});
  assert.ok(a.ok && b.ok); assert.notStrictEqual(a.note.id, b.note.id);
});

test("create: refuses bad title, body, space with explicit codes and changes nothing", () => {
  const { st } = make(fakeStorage());
  assert.strictEqual(st.create({ title: "x".repeat(121) }).error.code, "title-too-long");
  assert.strictEqual(st.create({ body: "x".repeat(20001) }).error.code, "body-too-long");
  assert.strictEqual(st.create({ title: 5 }).error.code, "title-type");
  assert.strictEqual(st.create({ body: null }).error.code, "body-type");
  assert.strictEqual(st.create({ space: "Bad Space!" }).error.code, "bad-space");
  assert.strictEqual(st.count(), 0); assert.strictEqual(st.isDirty(), false);
});

test("limits are inclusive: 120-char title and 20,000-char body are accepted", () => {
  const { st } = make(fakeStorage());
  const r = st.create({ title: "t".repeat(120), body: "b".repeat(20000) });
  assert.ok(r.ok);
  assert.ok(st.update(r.note.id, { title: "u".repeat(120) }).ok);
  assert.ok(st.update(r.note.id, { body: "c".repeat(20000) }).ok);
});

test("error messages say how far over the limit the text is", () => {
  const { st } = make(fakeStorage());
  assert.match(st.create({ title: "x".repeat(130) }).error.message, /limited to 120 characters \(10 too many\)/);
  assert.match(st.create({ body: "x".repeat(20005) }).error.message, /limited to 20,000 characters \(5 too many\)/);
});

test("the 201st note is refused with a clear error; the 200 stay intact", () => {
  const { st } = make(fakeStorage());
  for (let i = 0; i < 200; i++) assert.ok(st.create({ title: "n" + i }).ok, "note " + i);
  const r = st.create({ title: "one too many" });
  assert.strictEqual(r.ok, false); assert.strictEqual(r.error.code, "too-many"); assert.match(r.error.message, /200/);
  assert.strictEqual(st.count(), 200);
});

test("update: changes fields, bumps updated, leaves created; unchanged values do not bump", () => {
  const { st, c } = make(fakeStorage());
  const n = st.create({ title: "a", body: "b" }).note; st.flush();
  c.tick(5000);
  const u = st.update(n.id, { title: "a", body: "b" });
  assert.ok(u.ok); assert.strictEqual(u.changed, false); assert.strictEqual(st.isDirty(), false);
  const v = st.update(n.id, { body: "bb" });
  assert.ok(v.changed); assert.strictEqual(v.note.updated, c()); assert.strictEqual(v.note.created, n.created); assert.strictEqual(st.isDirty(), true);
});

test("update: a refused field leaves the whole patch unapplied", () => {
  const { st } = make(fakeStorage());
  const n = st.create({ title: "keep", body: "keep" }).note;
  const r = st.update(n.id, { title: "changed", body: "x".repeat(20001) });
  assert.strictEqual(r.ok, false); assert.strictEqual(r.error.code, "body-too-long");
  assert.strictEqual(st.get(n.id).title, "keep");
});

test("update/pin/remove on an unknown id fail cleanly", () => {
  const { st } = make(fakeStorage());
  for (const r of [st.update("nope", { title: "x" }), st.pin("nope"), st.remove("nope")]) { assert.strictEqual(r.ok, false); assert.strictEqual(r.error.code, "not-found"); }
});

test("update ignores unknown patch keys (no id/pinned/created injection)", () => {
  const { st } = make(fakeStorage());
  const n = st.create({ title: "a" }).note;
  st.update(n.id, { id: "evil", pinned: true, created: 0, updated: 0, title: "b" });
  const g = st.get(n.id);
  assert.strictEqual(g.id, n.id); assert.strictEqual(g.pinned, false); assert.strictEqual(g.created, n.created); assert.strictEqual(g.title, "b");
});

test("pin toggles, can be set explicitly, and does not change `updated`", () => {
  const { st, c } = make(fakeStorage());
  const n = st.create({}).note; c.tick(1000);
  assert.strictEqual(st.pin(n.id).note.pinned, true);
  assert.strictEqual(st.pin(n.id).note.pinned, false);
  assert.strictEqual(st.pin(n.id, true).note.pinned, true);
  assert.strictEqual(st.pin(n.id, true).note.pinned, true);
  assert.strictEqual(st.get(n.id).updated, n.updated);
});

test("sort: pinned first, then newest update, ties broken by id", () => {
  const s = M.sortNotes([note({ id: "c", updated: 5 }), note({ id: "b", updated: 9 }), note({ id: "a", updated: 9 }), note({ id: "p", updated: 1, pinned: true }), note({ id: "q", updated: 3, pinned: true })]);
  assert.deepStrictEqual(s.map((n) => n.id), ["q", "p", "a", "b", "c"]);
});

test("sort does not mutate its input", () => {
  const list = [note({ id: "x", updated: 1 }), note({ id: "y", updated: 2 })];
  M.sortNotes(list); assert.deepStrictEqual(list.map((n) => n.id), ["x", "y"]);
});

test("editing a note moves it to the top of its group; pinned notes stay above", () => {
  const { st, c } = make(fakeStorage());
  const a = st.create({ title: "a" }).note; c.tick(10); const b = st.create({ title: "b" }).note; c.tick(10); const p = st.create({ title: "p" }).note; st.pin(p.id);
  assert.deepStrictEqual(st.list().map((n) => n.title), ["p", "b", "a"]);
  c.tick(10); st.update(a.id, { body: "edit" });
  assert.deepStrictEqual(st.list().map((n) => n.title), ["p", "a", "b"]);
});

test("search: case-insensitive over title and body, all words must match, empty matches all", () => {
  const { st } = make(fakeStorage());
  st.create({ title: "Shopping", body: "Milk and EGGS" }); st.create({ title: "Work plan", body: "ship the milk report" }); st.create({ title: "Misc", body: "" });
  assert.deepStrictEqual(st.list({ query: "milk" }).map((n) => n.title).sort(), ["Shopping", "Work plan"]);
  assert.deepStrictEqual(st.list({ query: "eggs milk" }).map((n) => n.title), ["Shopping"]);
  assert.deepStrictEqual(st.list({ query: "SHOPPING" }).map((n) => n.title), ["Shopping"]);
  assert.strictEqual(st.list({ query: "   " }).length, 3);
  assert.strictEqual(st.list({ query: "zzz" }).length, 0);
  assert.strictEqual(st.list({ query: "eggs  milk" }).length, 1);
});

test("search treats the query as text, not a pattern", () => {
  const { st } = make(fakeStorage());
  st.create({ title: "a.b", body: "(x)" }); st.create({ title: "axb", body: "" });
  assert.deepStrictEqual(st.list({ query: "a.b" }).map((n) => n.title), ["a.b"]);
  assert.strictEqual(st.list({ query: "(" }).length, 1);
  assert.strictEqual(st.list({ query: ".*" }).length, 0);
  assert.strictEqual(st.list({ query: "[" }).length, 0);
});

test("space filter: one Space, 'all', or unknown", () => {
  const { st } = make(fakeStorage());
  st.create({ space: "study" }); st.create({ space: "work" }); st.create({ space: "work" });
  assert.strictEqual(st.list({ space: "work" }).length, 2);
  assert.strictEqual(st.list({ space: "all" }).length, 3);
  assert.strictEqual(st.list({}).length, 3);
  assert.strictEqual(st.list({ space: "personal" }).length, 0);
  assert.strictEqual(st.list({ space: "work", query: "zz" }).length, 0);
});

test("remove + restore: one-level undo; second delete replaces the buffer; restore keeps the note intact", () => {
  const { st } = make(fakeStorage());
  const a = st.create({ title: "a", body: "text" }).note, b = st.create({ title: "b" }).note;
  st.pin(a.id);
  assert.ok(st.remove(a.id).ok); assert.strictEqual(st.canUndo(), true); assert.strictEqual(st.undoTitle(), "a");
  assert.ok(st.remove(b.id).ok); assert.strictEqual(st.undoTitle(), "b");
  const r = st.restore(); assert.ok(r.ok); assert.strictEqual(r.note.id, b.id);
  assert.strictEqual(st.canUndo(), false);
  assert.strictEqual(st.restore().error.code, "nothing-to-restore");
  assert.strictEqual(st.get(a.id), null);                       // "a" was overwritten in the one-level buffer
  assert.strictEqual(st.count(), 1);
});

test("restore returns the identical note (pinned, space, times)", () => {
  const { st, c } = make(fakeStorage());
  const n = st.create({ title: "keep", body: "me", space: "work" }).note; st.pin(n.id); c.tick(77); st.update(n.id, { body: "me2" });
  const before = st.get(n.id); st.remove(n.id); st.restore();
  assert.deepStrictEqual(st.get(n.id), before);
});

test("restore is refused at 200 notes", () => {
  const { st } = make(fakeStorage());
  const first = st.create({}).note; st.remove(first.id);
  for (let i = 0; i < 200; i++) st.create({});
  assert.strictEqual(st.restore().error.code, "too-many"); assert.strictEqual(st.count(), 200);
});

test("word count", () => {
  assert.strictEqual(M.wordCount(""), 0); assert.strictEqual(M.wordCount("   \n\t "), 0);
  assert.strictEqual(M.wordCount("one"), 1); assert.strictEqual(M.wordCount("one  two\nthree\tfour"), 4);
  assert.strictEqual(M.wordCount("naïve café — ok"), 4); assert.strictEqual(M.wordCount(null), 0); assert.strictEqual(M.wordCount(undefined), 0);
});

test("snippet collapses whitespace and truncates with an ellipsis", () => {
  assert.strictEqual(M.snippet("a\n\n b   c"), "a b c");
  assert.strictEqual(M.snippet("x".repeat(200), 10), "xxxxxxxxx…");
  assert.strictEqual(M.snippet(""), "");
});

test("stamp: HH:MM today, day + month otherwise", () => {
  const now = new Date(2026, 10, 13, 14, 30).getTime();
  assert.strictEqual(M.stamp(new Date(2026, 10, 13, 9, 5).getTime(), now), "09:05");
  assert.strictEqual(M.stamp(new Date(2026, 10, 12, 23, 59).getTime(), now), "12 Nov");
  assert.strictEqual(M.stamp(new Date(2025, 10, 13, 9, 5).getTime(), now), "13 Nov");
});

console.log("Notes storage");

test("flush writes the versioned envelope under the documented key and load reads it back", () => {
  const s = fakeStorage(); const { st } = make(s);
  const n = st.create({ title: "Hi", body: "there", space: "work" }).note; st.pin(n.id);
  const f = st.flush(); assert.ok(f.ok && f.wrote);
  const raw = JSON.parse(s.data.get("aurora-glass.notes.v1"));
  assert.strictEqual(raw.v, 1); assert.strictEqual(raw.notes.length, 1);
  assert.deepStrictEqual(Object.keys(raw.notes[0]).sort(), ["body", "created", "id", "pinned", "space", "title", "updated"]);
  const again = make(s).st; assert.strictEqual(again.count(), 1);
  assert.deepStrictEqual(again.get(n.id), st.get(n.id));
});

test("flush with nothing changed writes nothing", () => {
  const s = fakeStorage(); const { st } = make(s);
  st.create({}); st.flush(); const n = s.sets;
  assert.strictEqual(st.flush().wrote, false); assert.strictEqual(s.sets, n);
});

test("unicode and emoji survive a round trip", () => {
  const s = fakeStorage(); const { st } = make(s);
  const n = st.create({ title: "नमस्તે 🙂", body: "こんにちは\n\"quotes\" \\ back\u0000nul \ud83d" }).note; st.flush();
  assert.deepStrictEqual(make(s).st.get(n.id), st.get(n.id));
});

test("load: nothing stored -> empty", () => {
  const { st } = make(fakeStorage()); const r = st.load(); assert.strictEqual(r.status, "empty"); assert.strictEqual(st.count(), 0);
});

test("load: malformed JSON starts empty, reports it, and backs up the raw text instead of losing it", () => {
  const s = fakeStorage({ "aurora-glass.notes.v1": "{not json" });
  const st = M.createStore({ storage: s }); const r = st.load();
  assert.strictEqual(r.status, "recovered"); assert.match(r.message, /not valid JSON/); assert.match(r.message, /kept in this browser/);
  assert.strictEqual(st.count(), 0); assert.strictEqual(s.data.get(M.BACKUP_KEY), "{not json");
});

test("load: wrong shapes and versions are unusable as a whole and backed up", () => {
  for (const raw of ["[]", "null", "42", '"s"', JSON.stringify({ v: 2, notes: [] }), JSON.stringify({ v: 0, notes: [] }), JSON.stringify({ notes: [] }), JSON.stringify({ v: 1 }), JSON.stringify({ v: 1, notes: {} })]) {
    const s = fakeStorage({ "aurora-glass.notes.v1": raw }); const st = M.createStore({ storage: s }); const r = st.load();
    assert.strictEqual(r.status, "recovered", raw); assert.strictEqual(st.count(), 0, raw); assert.strictEqual(s.data.get(M.BACKUP_KEY), raw, raw);
  }
  const newer = M.createStore({ storage: fakeStorage({ "aurora-glass.notes.v1": JSON.stringify({ v: 2, notes: [] }) }) }).load();
  assert.match(newer.message, /newer version/);
});

test("load: invalid notes are dropped one by one and counted; valid ones survive", () => {
  const good = note({ id: "good" });
  const bad = [null, 5, "s", [], {}, note({ id: "" }), note({ id: "a b" }), note({ id: "x".repeat(65) }), note({ id: 7 }), note({ title: 5 }), note({ body: null }),
    note({ title: "t".repeat(121) }), note({ body: "b".repeat(20001) }), note({ pinned: "yes" }), note({ space: "Has Caps" }), note({ space: "" }), note({ space: 3 }),
    note({ created: -1 }), note({ created: 1.5 }), note({ created: "1" }), note({ updated: NaN }), note({ updated: Infinity }), note({ updated: 1e30 }), note({ id: "__proto__x", created: null })];
  const s = fakeStorage({ "aurora-glass.notes.v1": envelope([good].concat(bad)) });
  const st = M.createStore({ storage: s }); const r = st.load();
  assert.strictEqual(r.status, "recovered"); assert.strictEqual(r.dropped, bad.length); assert.strictEqual(st.count(), 1); assert.strictEqual(st.get("good").title, "T");
  assert.ok(s.data.has(M.BACKUP_KEY));
});

test("load: duplicate ids keep the first; more than 200 keeps 200", () => {
  const dup = M.createStore({ storage: fakeStorage({ "aurora-glass.notes.v1": envelope([note({ id: "d", title: "first" }), note({ id: "d", title: "second" })]) }) });
  const r = dup.load(); assert.strictEqual(r.dropped, 1); assert.strictEqual(dup.get("d").title, "first");
  const many = []; for (let i = 0; i < 205; i++) many.push(note({ id: "n" + i }));
  const big = M.createStore({ storage: fakeStorage({ "aurora-glass.notes.v1": envelope(many) }) }); const r2 = big.load();
  assert.strictEqual(big.count(), 200); assert.strictEqual(r2.dropped, 5);
});

test("load: extra fields on a note are stripped, not stored", () => {
  const s = fakeStorage({ "aurora-glass.notes.v1": envelope([Object.assign(note({ id: "e" }), { evil: "<script>", __proto__: { x: 1 } })]) });
  const st = M.createStore({ storage: s }); st.load(); assert.strictEqual(st.get("e").evil, undefined);
  st.update("e", { body: "z" }); st.flush();
  assert.ok(!/evil|script/.test(s.data.get("aurora-glass.notes.v1")));
});

test("load never throws: storage whose getItem throws -> unavailable, notes still work in memory", () => {
  const s = fakeStorage(); s.failGet = new Error("blocked");
  const st = M.createStore({ storage: s }); const r = st.load();
  assert.strictEqual(r.status, "unavailable"); assert.strictEqual(r.ok, false); assert.strictEqual(st.hasStorage(), false);
  assert.ok(st.create({ title: "still here" }).ok); assert.strictEqual(st.count(), 1);
  const f = st.flush(); assert.strictEqual(f.ok, false); assert.match(f.error.message, /unavailable/); assert.strictEqual(st.isDirty(), true);
});

test("load: no storage at all (null) is the same unavailable state", () => {
  const st = M.createStore({ storage: null }); assert.strictEqual(st.load().status, "unavailable");
  st.create({}); assert.strictEqual(st.flush().ok, false); assert.strictEqual(st.count(), 1);
});

test("quota failure: flush reports 'storage is full', keeps every note and the dirty flag, and the next flush retries and succeeds", () => {
  const s = fakeStorage(); const { st } = make(s);
  const n = st.create({ title: "precious", body: "do not lose" }).note;
  s.failSet = quota();
  const f = st.flush();
  assert.strictEqual(f.ok, false); assert.strictEqual(f.error.code, "not-saved"); assert.match(f.error.message, /full/);
  assert.strictEqual(st.isDirty(), true); assert.strictEqual(st.get(n.id).body, "do not lose"); assert.strictEqual(st.lastError(), "browser storage is full");
  assert.strictEqual(s.data.has(M.KEY), false);
  s.failSet = null;
  const g = st.flush(); assert.ok(g.ok && g.wrote); assert.strictEqual(st.isDirty(), false); assert.strictEqual(st.lastError(), null);
  assert.strictEqual(make(s).st.get(n.id).body, "do not lose");
});

test("other write failures map to readable reasons", () => {
  assert.match(M.storageReason(Object.assign(new Error(), { name: "SecurityError" })), /blocked/);
  assert.match(M.storageReason({ code: 22 }), /full/);
  assert.match(M.storageReason({ name: "NS_ERROR_DOM_QUOTA_REACHED" }), /full/);
  assert.match(M.storageReason(new Error("x")), /unavailable/);
  assert.match(M.storageReason(null), /unavailable/);
});

test("a failed write does not corrupt what was stored before", () => {
  const s = fakeStorage(); const { st } = make(s);
  const n = st.create({ title: "v1" }).note; st.flush();
  const before = s.data.get(M.KEY);
  st.update(n.id, { title: "v2" }); s.failSet = quota(); st.flush();
  assert.strictEqual(s.data.get(M.KEY), before);
});

test("an unreadable backup that cannot be written is reported honestly", () => {
  const s = fakeStorage({ "aurora-glass.notes.v1": "garbage" }); s.failSet = quota();
  const r = M.createStore({ storage: s }).load();
  assert.match(r.message, /backup copy could not be made/);
});

test("markup in notes is only ever data: it round-trips unchanged and is never interpreted by the model", () => {
  const evil = '<img src=x onerror=alert(1)></script><script>alert(2)</script>"\'`${1+1}{{7*7}}';
  const s = fakeStorage(); const { st } = make(s);
  const n = st.create({ title: evil.slice(0, 120), body: evil }).note; st.flush();
  const back = make(s).st.get(n.id); assert.strictEqual(back.body, evil);
  assert.strictEqual(st.list({ query: "onerror" }).length, 1);
});

test("keys like __proto__ / constructor in patches or fields do not pollute anything", () => {
  const { st } = make(fakeStorage());
  const n = st.create({ title: "x" }).note;
  st.update(n.id, JSON.parse('{"__proto__":{"polluted":1},"constructor":{"x":1},"title":"y"}'));
  assert.strictEqual(({}).polluted, undefined); assert.strictEqual(st.get(n.id).title, "y");
});

test("get returns copies: mutating one does not change the store", () => {
  const { st } = make(fakeStorage());
  const n = st.create({ title: "a" }).note; const g = st.get(n.id); g.title = "hacked"; g.pinned = true;
  assert.strictEqual(st.get(n.id).title, "a"); assert.strictEqual(st.get(n.id).pinned, false);
  const l = st.list(); l[0].title = "hacked"; assert.strictEqual(st.get(n.id).title, "a");
});

test("round trip of 200 maximum-size notes stays valid JSON and reloads whole", () => {
  const s = fakeStorage(); const { st } = make(s);
  for (let i = 0; i < 200; i++) st.create({ title: "t".repeat(120), body: "b".repeat(20000), space: i % 2 ? "work" : "study" });
  assert.ok(st.flush().ok);
  assert.strictEqual(make(s).st.count(), 200);
});

console.log("\n" + passed + " passed, " + failed + " failed");
process.exit(failed ? 1 : 0);
