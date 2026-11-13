/* aurora-glass/prototype/clock.js - the Clock screen (window.AuroraClock): Clock, Stopwatch and Timer tabs.
 *
 * SIMULATION ONLY. Everything is computed from a monotonic clock (clock-model.js); nothing is saved and nothing keeps running after the
 * page is closed. The Clock owns the ONE ticking timer. A `timer.start` approved through the Action Broker (AuroraSystem "timer" event)
 * is mirrored into it, and undoing that receipt cancels it again; the Clock UI itself starts timers directly (a plain user action) and
 * does not write the broker's record. No Notification permission is ever requested and no sound is played.
 */
(function () {
  "use strict";
  const UI = window.AuroraUI, Shell = window.AuroraShell, M = window.AuroraClockModel, System = window.AuroraSystem || null;
  if (!UI || !Shell || !M) return;
  const { el, icon } = UI;

  const screen = Shell.addScreen("screen-clock", "Clock");
  const k = M.createClock();
  const sw = k.stopwatch, tm = k.timer;
  const TABS = [["clock", "Clock"], ["stopwatch", "Stopwatch"], ["timer", "Timer"]];
  const ui = { tab: "clock", origin: null, error: "", alertOpen: false, raf: 0, wall: 0, tick: 0 };
  let els = {};

  const live = () => screen.classList.contains("active") && !document.hidden;
  const reduced = () => document.documentElement.getAttribute("data-motion") === "reduce" || (window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches);

  // ---------------- Clock tab ----------------
  function renderWall() {
    if (!els.time) return;
    const d = new Date();
    els.time.textContent = d.toLocaleTimeString([], reduced() ? { hour: "numeric", minute: "2-digit" } : { hour: "numeric", minute: "2-digit", second: "2-digit" });
    els.time.setAttribute("datetime", d.toISOString());
    els.date.textContent = d.toLocaleDateString([], { weekday: "long", day: "numeric", month: "long", year: "numeric" });
    let zone = ""; try { zone = Intl.DateTimeFormat().resolvedOptions().timeZone || ""; } catch (e) { /* no Intl zone */ }
    els.zone.textContent = (zone ? zone.replace(/_/g, " ") + " · " : "") + M.utcOffsetLabel(d.getTimezoneOffset());
  }
  function scheduleWall() {
    clearTimeout(ui.wall); ui.wall = 0;
    if (!live() || ui.tab !== "clock") return;
    const step = reduced() ? 60000 : 1000, now = Date.now();
    ui.wall = setTimeout(() => { renderWall(); scheduleWall(); }, step - (now % step) + 20);   // on the second (or minute) boundary, never a free-running interval
  }

  // ---------------- Stopwatch tab ----------------
  function renderSw() {
    if (!els.swDisplay) return;
    els.swDisplay.textContent = M.formatStopwatch(sw.elapsed());
  }
  function renderSwControls() {
    const running = sw.running(), has = sw.elapsed() > 0;
    els.swStart.querySelector("span").textContent = running ? "Pause" : has ? "Resume" : "Start";
    els.swStart.querySelector(".icon") && els.swStart.replaceChild(icon(running ? "pause" : "play"), els.swStart.querySelector(".icon"));
    els.swLap.disabled = !running; els.swReset.disabled = !has && !running;
    els.swDisplay.dataset.state = running ? "running" : has ? "paused" : "idle";
    UI.clear(els.swLaps);
    const laps = sw.laps();
    els.swEmpty.hidden = laps.length > 0;
    laps.slice().reverse().forEach((l) => els.swLaps.appendChild(el("li", { class: "sw-lap", "data-lap": String(l.n) }, [
      el("span", { class: "sw-lap-n", text: "Lap " + l.n }), el("span", { class: "sw-lap-split", text: M.formatStopwatch(l.split) }), el("span", { class: "sw-lap-total row-meta", text: M.formatStopwatch(l.total) })])));
    els.swLapCount.textContent = laps.length ? laps.length + (laps.length === 1 ? " lap" : " laps") : "";
  }
  function swFrame() {
    ui.raf = 0;
    renderSw();
    if (sw.running() && live() && ui.tab === "stopwatch") ui.raf = requestAnimationFrame(swFrame);
  }
  function kickSw() { if (!ui.raf && sw.running() && live() && ui.tab === "stopwatch") ui.raf = requestAnimationFrame(swFrame); }
  function swToggle() {
    if (sw.running()) { sw.pause(); Shell.announce("Stopwatch paused at " + M.formatStopwatch(sw.elapsed())); } else { sw.start(); Shell.announce("Stopwatch running"); }
    renderSw(); renderSwControls(); kickSw();
  }
  function swLap() {
    const r = sw.lap();
    if (!r.ok) { Shell.showToast(r.error.message, { tone: "warn" }); return; }
    renderSw(); renderSwControls(); Shell.announce("Lap " + r.lap.n + ", " + M.formatStopwatch(r.lap.split));
  }
  function swReset() { sw.reset(); renderSw(); renderSwControls(); Shell.announce("Stopwatch reset"); }

  // ---------------- Timer tab ----------------
  function setInputsFrom(ms) {
    const t = Math.round(ms / 1000);
    els.h.value = String(Math.floor(t / 3600)); els.m.value = String(Math.floor(t / 60) % 60); els.s.value = String(t % 60);
  }
  function renderTimer() {
    if (!els.tDisplay) return;
    const st = tm.state(), rem = tm.remaining();
    els.tDisplay.textContent = M.formatCountdown(rem);
    const p = Math.round(tm.progress() * 100);
    els.tBar.setAttribute("aria-valuenow", String(p)); els.tBar.setAttribute("aria-valuetext", p + " percent elapsed");
    els.tFill.style.width = p + "%";
    els.tDisplay.dataset.state = st;
  }
  function renderTimerControls() {
    const st = tm.state(), editable = st === "idle" || st === "done";
    els.tState.textContent = { idle: "Ready", running: "Running", paused: "Paused", done: "Finished" }[st];
    els.tSource.hidden = !ui.origin;
    els.tStart.querySelector("span").textContent = st === "running" ? "Pause" : st === "paused" ? "Resume" : st === "done" ? "Start again" : "Start";
    const old = els.tStart.querySelector(".icon"); if (old) els.tStart.replaceChild(icon(st === "running" ? "pause" : "play"), old);
    els.tStart.disabled = editable && !!ui.error;
    els.tReset.disabled = st === "idle";
    [els.h, els.m, els.s].forEach((i) => { i.disabled = !editable; });
    els.presets.querySelectorAll("button").forEach((b) => { b.disabled = !editable; b.setAttribute("aria-pressed", String(editable && Number(b.dataset.seconds) * 1000 === tm.duration() && !ui.error)); });
    els.tError.textContent = ui.error; els.tError.hidden = !ui.error;
    [els.h, els.m, els.s].forEach((i) => { if (ui.error) i.setAttribute("aria-invalid", "true"); else i.removeAttribute("aria-invalid"); });
    renderTimer();
  }
  function onDurationInput() {
    const r = M.parseDuration(els.h.value, els.m.value, els.s.value);
    if (!r.ok) { ui.error = r.error.message; renderTimerControls(); return; }
    ui.error = "";
    const w = tm.setDuration(r.ms);
    if (!w.ok) ui.error = w.error.message;
    renderTimerControls();
  }
  function pickPreset(seconds) {
    const r = tm.setDuration(seconds * 1000);
    if (!r.ok) { Shell.showToast(r.error.message, { tone: "warn" }); return; }
    ui.error = ""; setInputsFrom(seconds * 1000); renderTimerControls();
    Shell.announce("Timer set to " + M.describeDuration(seconds * 1000));
  }
  function tStartToggle() {
    const st = tm.state();
    if (st === "running") { tm.pause(); Shell.announce("Timer paused, " + M.describeDuration(tm.remaining()) + " left"); }
    else if (st === "paused") { tm.resume(); Shell.announce("Timer resumed"); }
    else {
      if (ui.error) return;
      if (st === "done") tm.reset();
      const r = tm.start();
      if (!r.ok) { ui.error = r.error.message; renderTimerControls(); return; }
      ui.origin = null;                                         // a timer started here is the user's own, not an Aura one
      Shell.announce("Timer started for " + M.describeDuration(tm.duration()));
    }
    renderTimerControls(); scheduleTick();
  }
  function tReset() { tm.reset(); ui.origin = null; ui.error = ""; setInputsFrom(tm.duration()); renderTimerControls(); scheduleTick(); Shell.announce("Timer reset"); }

  /** Poll the model; schedule the next look. Runs while a timer is running, on this screen or not. */
  function tick() {
    ui.tick = 0;
    if (tm.poll()) onFinished();
    renderTimer(); if (tm.state() !== "running") renderTimerControls();
    scheduleTick();
  }
  function scheduleTick() {
    clearTimeout(ui.tick); ui.tick = 0;
    if (tm.state() !== "running") return;
    const rem = tm.remaining(), active = live() && ui.tab === "timer";
    ui.tick = setTimeout(tick, active ? Math.min(250, rem + 5) : Math.min(1000, rem + 5));
  }
  function onFinished() {
    renderTimerControls();
    const d = M.describeDuration(tm.duration());
    if (System && System.emit) System.emit("status", { text: "Timer finished", running: false });
    if (ui.alertOpen) return;
    ui.alertOpen = true;
    UI.dialog({
      id: "timer-done-dialog", role: "alertdialog", icon: "timer", title: "Timer finished", body: "Your " + d + " timer has finished.", initialFocus: "#btn-timer-done-dismiss",
      actions: [{ label: "Show timer", kind: "outline", value: "show", id: "btn-timer-done-show" }, { label: "Dismiss", kind: "accent", value: "ok", id: "btn-timer-done-dismiss" }],
    }).then((v) => { ui.alertOpen = false; if (v === "show") { Shell.openApp("clock"); selectTab("timer", true); } });
  }

  // ---------------- Action Broker link ----------------
  /** AuroraSystem "timer" event: world.timer is {seconds, startedAt} (wall clock) or null. The Clock stays the only source of ticking. */
  function onWorldTimer(w) {
    if (!w) {                                                           // the receipt was undone (or nothing to restore)
      if (ui.origin && tm.state() !== "idle") { tm.reset(); setInputsFrom(tm.duration()); ui.error = ""; Shell.announce("Timer cancelled"); }
      ui.origin = null; renderTimerControls(); scheduleTick(); return;
    }
    if (!Number.isFinite(w.seconds) || !Number.isFinite(w.startedAt)) return;
    if (ui.origin && ui.origin.startedAt === w.startedAt && ui.origin.seconds === w.seconds) return;     // same timer announced twice
    const r = tm.startFrom(w.seconds * 1000, Date.now() - w.startedAt);
    if (!r.ok) {                                                        // restored to a timer that had already run out: nothing to run
      if (r.error.code === "elapsed") { tm.reset(); ui.origin = null; renderTimerControls(); scheduleTick(); }
      return;
    }
    ui.origin = { startedAt: w.startedAt, seconds: w.seconds };
    ui.error = ""; setInputsFrom(tm.duration());
    renderTimerControls(); scheduleTick();
    Shell.announce("Timer started by Aura for " + M.describeDuration(tm.duration()));
  }

  // ---------------- tabs ----------------
  function selectTab(id, focus) {
    if (!TABS.some((t) => t[0] === id)) return;
    ui.tab = id;
    TABS.forEach(([t]) => {
      const on = t === id, b = els.tabs[t];
      b.setAttribute("aria-selected", String(on)); b.tabIndex = on ? 0 : -1; els.panels[t].hidden = !on;
      if (on && focus) b.focus();
    });
    if (id === "clock") { renderWall(); scheduleWall(); } else clearTimeout(ui.wall);
    if (id === "stopwatch") { renderSw(); renderSwControls(); kickSw(); }
    if (id === "timer") { renderTimerControls(); }
    scheduleTick();
  }
  function onTabKey(e) {
    const i = TABS.findIndex((t) => t[0] === ui.tab); let j = null;
    if (e.key === "ArrowRight") j = (i + 1) % TABS.length; else if (e.key === "ArrowLeft") j = (i + TABS.length - 1) % TABS.length;
    else if (e.key === "Home") j = 0; else if (e.key === "End") j = TABS.length - 1;
    if (j === null) return;
    e.preventDefault(); selectTab(TABS[j][0], true);
  }

  function field(id, label) {
    const input = el("input", { type: "text", inputmode: "numeric", autocomplete: "off", class: "field timer-field", id, value: "0", "aria-label": label, oninput: onDurationInput });
    return el("div", { class: "control-group timer-unit" }, [el("label", { for: id, text: label }), input]);
  }

  function build() {
    UI.clear(screen);
    screen.appendChild(el("header", { class: "screen-header" }, [
      el("button", { type: "button", class: "btn btn-icon ghost", id: "btn-clock-back", "aria-label": "Back", title: "Back", onclick: () => Shell.back() }, [icon("back")]),
      el("div", { class: "screen-heading" }, [el("p", { class: "eyebrow", text: "Time" }), el("h1", { class: "screen-title", text: "Clock" })]),
    ]));
    screen.appendChild(el("p", { class: "callout", id: "clock-banner" }, [icon("info"), el("span", { text: "Browser prototype. Timers and the stopwatch run only while this page is open; nothing is saved." })]));

    els.tabs = {}; els.panels = {};
    const tablist = el("div", { class: "clock-tabs", role: "tablist", "aria-label": "Clock tools", onkeydown: onTabKey });
    TABS.forEach(([id, label]) => {
      els.tabs[id] = el("button", { type: "button", role: "tab", class: "clock-tab", id: "tab-clock-" + id, "aria-controls": "panel-clock-" + id, "aria-selected": "false", tabindex: "-1", text: label, onclick: () => selectTab(id, false) });
      tablist.appendChild(els.tabs[id]);
    });
    screen.appendChild(tablist);

    // Clock
    els.time = el("time", { class: "clock-time", id: "clock-time" });
    els.date = el("p", { class: "clock-date", id: "clock-date" });
    els.zone = el("p", { class: "row-meta", id: "clock-zone" });
    els.panels.clock = el("section", { class: "card opaque clock-panel", id: "panel-clock-clock", role: "tabpanel", "aria-labelledby": "tab-clock-clock" }, [els.time, els.date, els.zone]);

    // Stopwatch
    els.swDisplay = el("p", { class: "clock-big", id: "sw-display", "aria-label": "Stopwatch", text: "0:00.00" });
    els.swStart = el("button", { type: "button", class: "btn accent", id: "btn-sw-start", onclick: swToggle }, [icon("play"), el("span", { text: "Start" })]);
    els.swLap = el("button", { type: "button", class: "btn outline", id: "btn-sw-lap", onclick: swLap }, [icon("flag"), el("span", { text: "Lap" })]);
    els.swReset = el("button", { type: "button", class: "btn outline", id: "btn-sw-reset", onclick: swReset }, [icon("undo"), el("span", { text: "Reset" })]);
    els.swEmpty = el("p", { class: "row-meta", id: "sw-empty", text: "Laps appear here while the stopwatch is running." });
    els.swLaps = el("ol", { class: "sw-laps", id: "sw-laps", "aria-label": "Laps" });
    els.swLapCount = el("span", { class: "row-meta", id: "sw-lap-count" });
    els.panels.stopwatch = el("section", { class: "card opaque clock-panel", id: "panel-clock-stopwatch", role: "tabpanel", "aria-labelledby": "tab-clock-stopwatch", hidden: true }, [
      els.swDisplay, el("div", { class: "clock-actions" }, [els.swStart, els.swLap, els.swReset]),
      el("div", { class: "sw-laps-head" }, [el("h2", { class: "clock-sub", text: "Laps" }), els.swLapCount]), els.swEmpty, els.swLaps]);

    // Timer
    els.tDisplay = el("p", { class: "clock-big", id: "timer-display", role: "timer", "aria-label": "Time remaining", text: "0:05:00" });
    els.tFill = el("div", { class: "timer-fill" });
    els.tBar = el("div", { class: "timer-bar", id: "timer-progress", role: "progressbar", "aria-label": "Timer progress", "aria-valuemin": "0", "aria-valuemax": "100", "aria-valuenow": "0" }, [els.tFill]);
    els.tState = el("span", { class: "chip chip-muted", id: "timer-state", text: "Ready" });
    els.tSource = el("span", { class: "chip chip-info", id: "timer-source", hidden: true, text: "Started by Aura" });
    els.presets = el("div", { class: "trail-chips", role: "group", "aria-label": "Presets", id: "timer-presets" }, M.PRESETS.map((s) => el("button", {
      type: "button", class: "trail-filter", id: "btn-timer-preset-" + s, "data-seconds": String(s), "aria-pressed": "false", "aria-label": M.describeDuration(s * 1000), text: s / 60 + " min", onclick: () => pickPreset(s) })));
    const inputs = [field("timer-h", "Hours"), field("timer-m", "Minutes"), field("timer-s", "Seconds")];
    [els.h, els.m, els.s] = inputs.map((w) => w.querySelector("input"));
    els.tError = el("p", { class: "timer-error", id: "timer-error", role: "alert", hidden: true });
    els.tStart = el("button", { type: "button", class: "btn accent", id: "btn-timer-start", onclick: tStartToggle }, [icon("play"), el("span", { text: "Start" })]);
    els.tReset = el("button", { type: "button", class: "btn outline", id: "btn-timer-reset", onclick: tReset }, [icon("undo"), el("span", { text: "Reset" })]);
    els.panels.timer = el("section", { class: "card opaque clock-panel", id: "panel-clock-timer", role: "tabpanel", "aria-labelledby": "tab-clock-timer", hidden: true }, [
      el("div", { class: "timer-status" }, [els.tState, els.tSource]), els.tDisplay, els.tBar, el("div", { class: "clock-actions" }, [els.tStart, els.tReset]),
      el("h2", { class: "clock-sub", text: "Length" }), els.presets, el("div", { class: "timer-inputs" }, inputs), els.tError,
      el("p", { class: "row-meta", text: "The timer runs only while this page is open. An alert appears here when it finishes." })]);

    TABS.forEach(([id]) => screen.appendChild(els.panels[id]));
    setInputsFrom(tm.duration());
    selectTab(ui.tab, false);
    renderSw(); renderSwControls(); renderTimerControls();
  }

  // ---------------- lifecycle ----------------
  Shell.onScreenShow((id) => {
    if (id === "screen-clock") { renderWall(); renderSw(); renderSwControls(); renderTimerControls(); scheduleWall(); kickSw(); }
    else { clearTimeout(ui.wall); ui.wall = 0; }
    scheduleTick();
  });
  document.addEventListener("visibilitychange", () => {
    if (document.hidden) { clearTimeout(ui.wall); return; }
    if (tm.poll()) onFinished();                                         // the tab was asleep: recompute from the clock now, no drift
    renderWall(); renderSw(); renderTimerControls(); scheduleWall(); kickSw(); scheduleTick();
  });
  if (System && System.on) System.on("timer", onWorldTimer);

  build();
  if (System && System.world && System.world.timer) onWorldTimer(System.world.timer);

  /** Stop everything and return to the first tab (layout matrix and tests; the screen itself never calls it). */
  function resetAll() {
    clearTimeout(ui.tick); sw.reset(); tm.reset(); ui.origin = null; ui.error = ""; setInputsFrom(tm.duration());
    selectTab("clock", false); renderSw(); renderSwControls(); renderTimerControls();
  }

  window.AuroraClock = {
    selectTab, reset: resetAll,
    state: () => ({ tab: ui.tab, origin: ui.origin ? Object.assign({}, ui.origin) : null, alertOpen: ui.alertOpen,
      timer: { state: tm.state(), duration: tm.duration(), remaining: tm.remaining() }, stopwatch: { running: sw.running(), elapsed: sw.elapsed(), laps: sw.laps() } }),
  };
})();
