/* aurora-glass/prototype/clock-model.js - DOM-free model for the Clock screen (window.AuroraClockModel).
 *
 * SIMULATION ONLY. A stopwatch and a countdown timer computed from an injected monotonic now() (performance.now() in the browser), never
 * from counting ticks, so a throttled or sleeping tab cannot drift: the next read recomputes from the clock. Nothing is stored, and
 * nothing continues after the page is closed. Loadable in the browser and in Node.
 */
(function (root, factory) {
  if (typeof module === "object" && module.exports) module.exports = factory();
  else root.AuroraClockModel = factory();
})(typeof self !== "undefined" ? self : this, function () {
  "use strict";

  const MIN_MS = 1000, MAX_MS = 86400000, MAX_LAPS = 200;
  const PRESETS = Object.freeze([60, 300, 600, 1500]);          // seconds: 1, 5, 10, 25 minutes
  const fail = (code, message) => ({ ok: false, error: { code, message } });
  const pad = (n, w) => String(n).padStart(w || 2, "0");

  // ---------------- pure helpers ----------------
  /** Stopwatch text: m:ss.cc below an hour, h:mm:ss.cc from an hour. Floors (a stopwatch shows what has elapsed). */
  function formatStopwatch(ms) {
    const t = Math.max(0, Math.floor(ms)), cs = Math.floor((t % 1000) / 10), s = Math.floor(t / 1000) % 60, m = Math.floor(t / 60000) % 60, h = Math.floor(t / 3600000);
    return (h ? h + ":" + pad(m) : String(m)) + ":" + pad(s) + "." + pad(cs);
  }
  /** Countdown text: h:mm:ss, rounded UP to a whole second so 0:00:00 appears exactly when the timer finishes. */
  function formatCountdown(ms) {
    const total = Math.max(0, Math.ceil(ms / 1000)), s = total % 60, m = Math.floor(total / 60) % 60, h = Math.floor(total / 3600);
    return h + ":" + pad(m) + ":" + pad(s);
  }
  /** "25 minutes", "1 hour 5 minutes", "45 seconds". */
  function describeDuration(ms) {
    const total = Math.round(ms / 1000), h = Math.floor(total / 3600), m = Math.floor(total / 60) % 60, s = total % 60, out = [];
    if (h) out.push(h + (h === 1 ? " hour" : " hours"));
    if (m) out.push(m + (m === 1 ? " minute" : " minutes"));
    if (s || !out.length) out.push(s + (s === 1 ? " second" : " seconds"));
    return out.join(" ");
  }
  /** Validate hours/minutes/seconds typed by a person (strings or numbers). {ok, ms} | {ok:false, error}. Total must be 1 s .. 24 h. */
  function parseDuration(h, m, s) {
    const part = (v, name, max) => {
      const t = String(v === undefined || v === null ? "" : v).trim();
      if (t === "") return 0;
      if (!/^\d{1,6}$/.test(t)) return fail("bad-" + name, "Enter whole numbers only for " + name + ".");
      const n = Number(t);
      return n > max ? fail("big-" + name, "Use at most " + max + " " + name + ".") : n;
    };
    const hh = part(h, "hours", 24), mm = part(m, "minutes", 1440), ss = part(s, "seconds", 86400);
    for (const p of [hh, mm, ss]) if (typeof p === "object") return p;
    const ms = (hh * 3600 + mm * 60 + ss) * 1000;
    if (ms < MIN_MS) return fail("too-short", "Set at least 1 second.");
    if (ms > MAX_MS) return fail("too-long", "The longest timer is 24 hours.");
    return { ok: true, ms };
  }
  /** "UTC+05:30" from a Date's offset (minutes WEST of UTC, as Date.getTimezoneOffset reports). */
  function utcOffsetLabel(offsetMinutesWest) {
    const east = -Math.round(offsetMinutesWest), sign = east < 0 ? "−" : "+", a = Math.abs(east);
    return "UTC" + sign + pad(Math.floor(a / 60)) + ":" + pad(a % 60);
  }

  // ---------------- the clock ----------------
  /** createClock({ now }) - now() returns monotonic milliseconds. */
  function createClock(opts) {
    const rawNow = (opts && opts.now) || (() => (typeof performance !== "undefined" ? performance.now() : Date.now()));
    let last = rawNow();
    const now = () => { const t = rawNow(); if (t > last) last = t; return last; };       // never goes backwards, even if the source does

    // ---- stopwatch ----
    const sw = { running: false, startedAt: 0, accumulated: 0, laps: [] };
    const swElapsed = () => sw.accumulated + (sw.running ? now() - sw.startedAt : 0);
    const stopwatch = {
      elapsed: swElapsed,
      running: () => sw.running,
      laps: () => sw.laps.map((l) => Object.assign({}, l)),
      start() { if (sw.running) return { ok: true, changed: false }; sw.startedAt = now(); sw.running = true; return { ok: true, changed: true }; },
      pause() { if (!sw.running) return { ok: true, changed: false }; sw.accumulated += now() - sw.startedAt; sw.running = false; return { ok: true, changed: true }; },
      reset() { sw.running = false; sw.startedAt = 0; sw.accumulated = 0; sw.laps = []; return { ok: true }; },
      /** Record a lap while running: {n, split (since the previous lap), total}. */
      lap() {
        if (!sw.running) return fail("not-running", "Start the stopwatch to record laps.");
        if (sw.laps.length >= MAX_LAPS) return fail("too-many-laps", "Up to " + MAX_LAPS + " laps are kept. Reset to start again.");
        const total = swElapsed(), prev = sw.laps.length ? sw.laps[sw.laps.length - 1].total : 0;
        const l = { n: sw.laps.length + 1, split: total - prev, total };
        sw.laps.push(l); return { ok: true, lap: Object.assign({}, l) };
      },
    };

    // ---- timer ----
    const tm = { state: "idle", duration: 5 * 60000, endsAt: 0, remaining: 5 * 60000, finished: 0, overshoot: 0 };
    const tmRemaining = () => (tm.state === "running" ? Math.max(0, tm.endsAt - now()) : tm.remaining);
    const validMs = (d) => Number.isFinite(d) && Math.floor(d) === d && d >= MIN_MS && d <= MAX_MS;
    function begin(duration, elapsed) {
      tm.duration = duration; tm.remaining = duration - elapsed; tm.endsAt = now() + tm.remaining; tm.state = "running"; tm.overshoot = 0;
    }
    const timer = {
      state: () => tm.state, duration: () => tm.duration, remaining: tmRemaining, finishedCount: () => tm.finished, overshoot: () => tm.overshoot,
      progress() { return tm.state === "idle" ? 0 : tm.state === "done" ? 1 : Math.min(1, Math.max(0, 1 - tmRemaining() / tm.duration)); },
      /** Choose the duration while idle or finished (a running or paused timer keeps its own). */
      setDuration(d) {
        if (!validMs(d)) return fail("bad-duration", "A timer must be 1 second to 24 hours (whole milliseconds).");
        if (tm.state === "running" || tm.state === "paused") return fail("busy", "Reset the timer before changing its length.");
        tm.duration = d; tm.remaining = d; tm.state = "idle"; return { ok: true };
      },
      /** Start the chosen duration. Starting again while running or paused is refused, never a second timer. */
      start(d) {
        if (tm.state === "running" || tm.state === "paused") return fail("already-running", "A timer is already " + tm.state + ". Reset it first.");
        const dur = d === undefined ? tm.duration : d;
        if (!validMs(dur)) return fail("bad-duration", "A timer must be 1 second to 24 hours (whole milliseconds).");
        begin(dur, 0); return { ok: true };
      },
      /** Replace whatever is there with a timer that has already run `elapsed` ms (used when an Action Broker timer starts). */
      startFrom(d, elapsed) {
        if (!validMs(d)) return fail("bad-duration", "A timer must be 1 second to 24 hours (whole milliseconds).");
        const e = Number.isFinite(elapsed) ? Math.max(0, elapsed) : 0;
        if (e >= d) return fail("elapsed", "That timer has already finished.");
        begin(d, e); return { ok: true };
      },
      pause() { if (tm.state !== "running") return { ok: true, changed: false }; tm.remaining = tmRemaining(); tm.state = "paused"; return { ok: true, changed: true }; },
      resume() { if (tm.state !== "paused") return { ok: true, changed: false }; tm.endsAt = now() + tm.remaining; tm.state = "running"; return { ok: true, changed: true }; },
      /** Back to idle with the same duration ready. */
      reset() { tm.state = "idle"; tm.remaining = tm.duration; tm.endsAt = 0; tm.overshoot = 0; return { ok: true }; },
      /** Call as often as you like. Returns true exactly once, when a running timer reaches its end (however late this is called). */
      poll() {
        if (tm.state !== "running") return false;
        const t = now();
        if (t < tm.endsAt) return false;
        tm.overshoot = t - tm.endsAt; tm.state = "done"; tm.remaining = 0; tm.finished++; return true;
      },
    };
    return { now, stopwatch, timer };
  }

  return { MIN_MS, MAX_MS, MAX_LAPS, PRESETS, formatStopwatch, formatCountdown, describeDuration, parseDuration, utcOffsetLabel, createClock };
});
