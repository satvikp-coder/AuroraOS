/** tests/unit/clock.test.js - the DOM-free Clock model (aurora-glass/prototype/clock-model.js) with an injected fake monotonic clock. */
const assert = require("assert");
const path = require("path");
const M = require(path.resolve(__dirname, "..", "..", "aurora-glass", "prototype", "clock-model.js"));

let passed = 0, failed = 0;
function test(name, fn) { try { fn(); passed++; console.log("  PASS:", name); } catch (e) { failed++; console.log("  FAIL:", name, "\n   ", e.message); } }
function fake(start) { let t = start === undefined ? 1000 : start; const f = () => t; f.set = (v) => { t = v; }; f.tick = (ms) => { t += ms; }; return f; }
function make(start) { const c = fake(start); return { c, k: M.createClock({ now: c }) }; }

console.log("Clock model: stopwatch");

test("starts at zero, runs from the injected clock, not from ticks", () => {
  const { c, k } = make(); const s = k.stopwatch;
  assert.strictEqual(s.elapsed(), 0); s.start(); c.tick(1234); assert.strictEqual(s.elapsed(), 1234);
  c.tick(10 * 3600 * 1000);                               // a tab that slept for ten hours: one read recomputes exactly
  assert.strictEqual(s.elapsed(), 1234 + 36000000);
});
test("pause keeps the accumulated time; resume continues from it; time while paused does not count", () => {
  const { c, k } = make(); const s = k.stopwatch;
  s.start(); c.tick(1000); s.pause(); c.tick(5000); assert.strictEqual(s.elapsed(), 1000);
  s.start(); c.tick(500); assert.strictEqual(s.elapsed(), 1500);
});
test("repeated start / pause are harmless no-ops", () => {
  const { c, k } = make(); const s = k.stopwatch;
  assert.strictEqual(s.pause().changed, false);
  s.start(); c.tick(100); assert.strictEqual(s.start().changed, false); c.tick(100); assert.strictEqual(s.elapsed(), 200);
  s.pause(); assert.strictEqual(s.pause().changed, false); assert.strictEqual(s.elapsed(), 200);
});
test("rapid pause/resume does not lose or add time", () => {
  const { c, k } = make(); const s = k.stopwatch; s.start();
  for (let i = 0; i < 1000; i++) { c.tick(1); s.pause(); s.start(); }
  assert.strictEqual(s.elapsed(), 1000);
});
test("laps record split and total; only while running", () => {
  const { c, k } = make(); const s = k.stopwatch;
  assert.strictEqual(s.lap().error.code, "not-running");
  s.start(); c.tick(1000); const a = s.lap().lap; c.tick(2500); const b = s.lap().lap;
  assert.deepStrictEqual(a, { n: 1, split: 1000, total: 1000 }); assert.deepStrictEqual(b, { n: 2, split: 2500, total: 3500 });
  s.pause(); assert.strictEqual(s.lap().error.code, "not-running"); assert.strictEqual(s.laps().length, 2);
});
test("laps survive pause/resume; reset clears time and laps and stops it", () => {
  const { c, k } = make(); const s = k.stopwatch; s.start(); c.tick(10); s.lap(); s.pause(); s.start(); c.tick(10); assert.strictEqual(s.lap().lap.split, 10);
  s.reset(); assert.strictEqual(s.elapsed(), 0); assert.strictEqual(s.laps().length, 0); assert.strictEqual(s.running(), false);
});
test("two laps at the same instant give a zero split, not an error", () => {
  const { c, k } = make(); const s = k.stopwatch; s.start(); c.tick(5); s.lap(); assert.strictEqual(s.lap().lap.split, 0);
});
test("lap list is capped with a clear error; returned laps are copies", () => {
  const { c, k } = make(); const s = k.stopwatch; s.start();
  for (let i = 0; i < M.MAX_LAPS; i++) { c.tick(1); assert.ok(s.lap().ok); }
  assert.strictEqual(s.lap().error.code, "too-many-laps");
  const l = s.laps(); l[0].split = -1; assert.notStrictEqual(s.laps()[0].split, -1);
});
test("a clock source that goes backwards never makes time negative or shrink", () => {
  const { c, k } = make(5000); const s = k.stopwatch; s.start(); c.tick(1000); assert.strictEqual(s.elapsed(), 1000);
  c.set(2000); assert.strictEqual(s.elapsed(), 1000); c.set(7000); assert.strictEqual(s.elapsed(), 2000);
});
test("stopwatch formatting", () => {
  assert.strictEqual(M.formatStopwatch(0), "0:00.00"); assert.strictEqual(M.formatStopwatch(999), "0:00.99");
  assert.strictEqual(M.formatStopwatch(61230), "1:01.23"); assert.strictEqual(M.formatStopwatch(3600000), "1:00:00.00");
  assert.strictEqual(M.formatStopwatch(3723456), "1:02:03.45"); assert.strictEqual(M.formatStopwatch(-5), "0:00.00");
  assert.strictEqual(M.formatStopwatch(100 * 3600000), "100:00:00.00");
});

console.log("Clock model: timer");

test("defaults: idle, 5 minutes ready", () => {
  const { k } = make(); const t = k.timer; assert.strictEqual(t.state(), "idle"); assert.strictEqual(t.duration(), 300000); assert.strictEqual(t.remaining(), 300000); assert.strictEqual(t.progress(), 0);
});
test("start counts down from the clock; finishes exactly at the duration, once", () => {
  const { c, k } = make(); const t = k.timer; t.setDuration(60000); assert.ok(t.start().ok);
  c.tick(59999); assert.strictEqual(t.poll(), false); assert.strictEqual(t.remaining(), 1); assert.strictEqual(t.state(), "running");
  c.tick(1); assert.strictEqual(t.poll(), true); assert.strictEqual(t.state(), "done"); assert.strictEqual(t.remaining(), 0); assert.strictEqual(t.progress(), 1);
  assert.strictEqual(t.poll(), false); assert.strictEqual(t.finishedCount(), 1);
});
test("a huge time jump (tab slept for hours) finishes once and reports the overshoot", () => {
  const { c, k } = make(); const t = k.timer; t.start(60000); c.tick(9 * 3600 * 1000);
  assert.strictEqual(t.poll(), true); assert.strictEqual(t.overshoot(), 9 * 3600 * 1000 - 60000); assert.strictEqual(t.poll(), false); assert.strictEqual(t.finishedCount(), 1);
});
test("pause freezes the remainder; time while paused does not count; resume continues", () => {
  const { c, k } = make(); const t = k.timer; t.start(60000); c.tick(20000); t.pause();
  assert.strictEqual(t.state(), "paused"); assert.strictEqual(t.remaining(), 40000);
  c.tick(3600000); assert.strictEqual(t.remaining(), 40000); assert.strictEqual(t.poll(), false);
  t.resume(); c.tick(39999); assert.strictEqual(t.poll(), false); c.tick(1); assert.strictEqual(t.poll(), true);
});
test("rapid pause/resume neither loses nor gains time", () => {
  const { c, k } = make(); const t = k.timer; t.start(100000);
  for (let i = 0; i < 500; i++) { c.tick(10); t.pause(); t.resume(); }
  assert.strictEqual(t.remaining(), 100000 - 5000);
});
test("pause on a timer that has run out between polls reports 0 and still finishes", () => {
  const { c, k } = make(); const t = k.timer; t.start(1000); c.tick(5000);
  assert.strictEqual(t.remaining(), 0); assert.strictEqual(t.poll(), true);
  assert.strictEqual(t.pause().changed, false); assert.strictEqual(t.state(), "done");
});
test("starting again while running or paused is refused and changes nothing", () => {
  const { c, k } = make(); const t = k.timer; t.start(60000); c.tick(10000);
  assert.strictEqual(t.start(5000).error.code, "already-running"); assert.strictEqual(t.remaining(), 50000);
  t.pause(); assert.strictEqual(t.start().error.code, "already-running"); assert.strictEqual(t.remaining(), 50000);
});
test("resume/pause in the wrong state are no-ops", () => {
  const { k } = make(); const t = k.timer;
  assert.strictEqual(t.resume().changed, false); assert.strictEqual(t.pause().changed, false); assert.strictEqual(t.state(), "idle");
});
test("reset returns to idle with the duration ready, from every state", () => {
  const { c, k } = make(); const t = k.timer;
  t.start(60000); c.tick(100); t.reset(); assert.strictEqual(t.state(), "idle"); assert.strictEqual(t.remaining(), 60000);
  t.start(60000); t.pause(); t.reset(); assert.strictEqual(t.state(), "idle");
  t.start(1000); c.tick(2000); t.poll(); t.reset(); assert.strictEqual(t.state(), "idle"); assert.strictEqual(t.remaining(), 1000);
});
test("a finished timer can be started again for the same duration; a reset never re-fires the old finish", () => {
  const { c, k } = make(); const t = k.timer; t.start(1000); c.tick(1000); assert.ok(t.poll());
  assert.ok(t.start().ok); assert.strictEqual(t.remaining(), 1000); c.tick(1000); assert.ok(t.poll()); assert.strictEqual(t.finishedCount(), 2);
});
test("invalid durations are refused with codes: zero, negative, fractional, NaN, Infinity, too long, wrong type", () => {
  const { k } = make(); const t = k.timer;
  for (const d of [0, -1000, 999, 1500.5, NaN, Infinity, 86400001, "60000", null, {}]) {
    assert.strictEqual(t.start(d).ok, false, String(d)); assert.strictEqual(t.setDuration(d).ok, false, String(d)); assert.strictEqual(t.startFrom(d, 0).ok, false, String(d));
  }
  assert.strictEqual(t.state(), "idle");
  assert.ok(t.start(1000).ok); t.reset(); assert.ok(t.start(86400000).ok);
});
test("zero duration through the parser is refused before it reaches the timer", () => {
  assert.strictEqual(M.parseDuration(0, 0, 0).error.code, "too-short");
});
test("setDuration only while idle or finished", () => {
  const { c, k } = make(); const t = k.timer;
  assert.ok(t.setDuration(90000).ok); assert.strictEqual(t.remaining(), 90000);
  t.start(); assert.strictEqual(t.setDuration(5000).error.code, "busy"); t.pause(); assert.strictEqual(t.setDuration(5000).error.code, "busy");
  t.reset(); t.start(1000); c.tick(1000); t.poll(); assert.ok(t.setDuration(5000).ok); assert.strictEqual(t.state(), "idle");
});
test("startFrom replaces any timer and honours elapsed time; an already-elapsed one is refused", () => {
  const { c, k } = make(); const t = k.timer; t.start(60000); c.tick(1000);
  assert.ok(t.startFrom(1500000, 4000).ok); assert.strictEqual(t.duration(), 1500000); assert.strictEqual(t.remaining(), 1496000); assert.strictEqual(t.state(), "running");
  assert.strictEqual(t.startFrom(1000, 1000).error.code, "elapsed"); assert.strictEqual(t.startFrom(1000, 5000).error.code, "elapsed");
  assert.strictEqual(t.duration(), 1500000);                       // refused: untouched
  assert.ok(t.startFrom(60000, -50).ok); assert.strictEqual(t.remaining(), 60000);   // negative elapsed (clock skew) counts as 0
  assert.ok(t.startFrom(60000, NaN).ok);
});
test("progress is 0..1 and monotonic while running", () => {
  const { c, k } = make(); const t = k.timer; t.start(100000); let p = -1;
  for (let i = 0; i < 100; i++) { c.tick(1000); const q = t.progress(); assert.ok(q >= p && q >= 0 && q <= 1); p = q; }
  assert.strictEqual(t.progress(), 1);
});
test("timer and stopwatch are independent and share only the clock", () => {
  const { c, k } = make(); k.stopwatch.start(); k.timer.start(10000); c.tick(4000); k.stopwatch.reset();
  assert.strictEqual(k.timer.remaining(), 6000); assert.strictEqual(k.stopwatch.elapsed(), 0);
});

console.log("Clock model: input parsing and text");

test("parseDuration: valid combinations", () => {
  assert.strictEqual(M.parseDuration(0, 1, 0).ms, 60000); assert.strictEqual(M.parseDuration("1", "30", "15").ms, 5415000);
  assert.strictEqual(M.parseDuration("", "", "45").ms, 45000); assert.strictEqual(M.parseDuration(24, 0, 0).ms, 86400000);
  assert.strictEqual(M.parseDuration(0, 90, 0).ms, 5400000); assert.strictEqual(M.parseDuration(" 0 ", " 2 ", "").ms, 120000);
  assert.strictEqual(M.parseDuration(undefined, 5, null).ms, 300000);
});
test("parseDuration: refuses junk with specific codes", () => {
  const code = (...a) => M.parseDuration(...a).error.code;
  assert.strictEqual(code("", "", ""), "too-short"); assert.strictEqual(code(0, 0, 0), "too-short");
  assert.strictEqual(code(25, 0, 0), "big-hours"); assert.strictEqual(code(24, 0, 1), "too-long"); assert.strictEqual(code(0, 1441, 0), "big-minutes");
  assert.strictEqual(code(0, 0, 86401), "big-seconds");
  for (const bad of ["-1", "1.5", "abc", "1e3", "0x10", "+5", "٣", "5 5", "9999999"]) assert.strictEqual(M.parseDuration(0, bad, 0).ok, false, bad);
  assert.match(M.parseDuration(0, "x", 0).error.message, /whole numbers/);
});
test("parseDuration: never returns a value outside 1 s .. 24 h", () => {
  for (let i = 0; i < 2000; i++) {
    const r = M.parseDuration(Math.floor(Math.random() * 40) - 5, Math.floor(Math.random() * 2000) - 5, Math.floor(Math.random() * 100000) - 5);
    if (r.ok) assert.ok(r.ms >= 1000 && r.ms <= 86400000 && r.ms % 1000 === 0);
  }
});
test("countdown text rounds up and shows 0:00:00 only at the end", () => {
  assert.strictEqual(M.formatCountdown(300000), "0:05:00"); assert.strictEqual(M.formatCountdown(299001), "0:05:00"); assert.strictEqual(M.formatCountdown(299000), "0:04:59");
  assert.strictEqual(M.formatCountdown(1), "0:00:01"); assert.strictEqual(M.formatCountdown(0), "0:00:00"); assert.strictEqual(M.formatCountdown(-100), "0:00:00");
  assert.strictEqual(M.formatCountdown(86400000), "24:00:00"); assert.strictEqual(M.formatCountdown(3661000), "1:01:01");
});
test("describeDuration wording", () => {
  assert.strictEqual(M.describeDuration(60000), "1 minute"); assert.strictEqual(M.describeDuration(1500000), "25 minutes");
  assert.strictEqual(M.describeDuration(5400000), "1 hour 30 minutes"); assert.strictEqual(M.describeDuration(1000), "1 second");
  assert.strictEqual(M.describeDuration(45000), "45 seconds"); assert.strictEqual(M.describeDuration(3661000), "1 hour 1 minute 1 second"); assert.strictEqual(M.describeDuration(86400000), "24 hours");
});
test("presets are the documented 1, 5, 10 and 25 minutes", () => { assert.deepStrictEqual(Array.from(M.PRESETS), [60, 300, 600, 1500]); assert.ok(Object.isFrozen(M.PRESETS)); });
test("UTC offset label, east and west, half-hour zones", () => {
  assert.strictEqual(M.utcOffsetLabel(-330), "UTC+05:30"); assert.strictEqual(M.utcOffsetLabel(0), "UTC+00:00"); assert.strictEqual(M.utcOffsetLabel(300), "UTC−05:00");
  assert.strictEqual(M.utcOffsetLabel(-765), "UTC+12:45"); assert.strictEqual(M.utcOffsetLabel(-0), "UTC+00:00");
});

console.log("\n" + passed + " passed, " + failed + " failed");
process.exit(failed ? 1 : 0);
