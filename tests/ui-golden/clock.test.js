/**
 * tests/ui-golden/clock.test.js - M7: Clock.
 * Browser prototype over the DOM-free model (tests/unit/clock.test.js covers the arithmetic with a fake clock). These tests use Playwright's
 * controlled browser time (page.clock) to drive the Clock, Stopwatch and Timer tabs, tab-sleep jumps, completion alerts, navigation away and
 * back, and the real Action Broker link: an Aura timer.start shows up in the Clock, completes, and is cancelled by Undo.
 * The clock is installed BEFORE the page loads so performance.now() and Date agree from the start.
 */
const { test, expect } = require("@playwright/test");

const BASE_URL = "http://localhost:3000";
const T0 = new Date("2026-11-12T14:02:30+05:30");
const ev = (page, fn, arg) => page.evaluate(fn, arg);

async function start(page) {
  const errors = [];
  page.on("console", (m) => { if (m.type() === "error") errors.push(m.text()); });
  page.on("pageerror", (e) => errors.push(String(e)));
  page.__errors = errors;
  await page.addInitScript(() => {                                   // spies: Clock must never ask for Notification permission or make sound
    window.__notif = 0; window.__audio = 0;
    try { const N = window.Notification; if (N) { window.Notification = function () { window.__notif++; }; window.Notification.requestPermission = () => { window.__notif++; return Promise.resolve("denied"); }; Object.defineProperty(window.Notification, "permission", { get: () => N.permission }); } } catch (e) { /* ignore */ }
    window.AudioContext = function () { window.__audio++; }; window.webkitAudioContext = window.AudioContext;
  });
  await page.clock.install({ time: T0 });
  await page.goto(BASE_URL);
  await ev(page, () => { window.AuroraSystem.dev.stepDelayMs = 0; });
  await page.clock.pauseAt(new Date(T0.getTime() + 1000));           // from here time moves only when a test moves it: exact, repeatable numbers
}
async function open(page, tab) {
  await ev(page, () => window.AuroraShell.openApp("clock"));
  await expect(page.locator("#screen-clock h1")).toHaveText("Clock");
  if (tab) await page.locator("#tab-clock-" + tab).click();
}
const noErrors = (page) => expect(page.__errors).toEqual([]);
const timerText = (page) => page.locator("#timer-display");
const swText = (page) => page.locator("#sw-display");
const auraTimer = (page, seconds) => ev(page, (s) => window.AuroraSystem.request({ type: "timer.start", arguments: { seconds: s } }, { source: "test" }), seconds);

test.describe("Clock: getting there and the Clock tab", () => {
  test.use({ timezoneId: "Asia/Kolkata", locale: "en-GB" });
  test("is a real screen with three tabs, the Clock tab first, and the prototype notice", async ({ page }) => {
    await start(page); await open(page);
    await expect(page.getByRole("tab")).toHaveText(["Clock", "Stopwatch", "Timer"]);
    await expect(page.locator("#tab-clock-clock")).toHaveAttribute("aria-selected", "true");
    await expect(page.locator("#clock-banner")).toContainText("only while this page is open");
    await noErrors(page);
  });
  test("shows the local time, date and time zone, and moves on by itself", async ({ page }) => {
    await start(page); await open(page);
    await expect(page.locator("#clock-time")).toHaveText("14:02:31");
    await expect(page.locator("#clock-date")).toHaveText("Thursday 12 November 2026");
    await expect(page.locator("#clock-zone")).toHaveText(/^Asia\/(Kolkata|Calcutta) · UTC\+05:30$/);
    await page.clock.runFor(3100);
    await expect(page.locator("#clock-time")).toHaveText("14:02:34");
  });
  test("reduced motion: no seconds, so nothing animates every second", async ({ page }) => {
    await page.emulateMedia({ reducedMotion: "reduce" });
    await start(page); await open(page);
    await expect(page.locator("#clock-time")).toHaveText("14:02");
  });
  test("is reachable from Index search and from Home Continue", async ({ page }) => {
    await start(page);
    await page.keyboard.press("Control+k");
    await page.locator("#index-query").fill("clock");
    await page.locator("#index-results .index-result", { hasText: "Clock" }).first().click();
    await expect(page.locator("#screen-clock")).toHaveClass(/active/);
  });
  test("tabs follow the ARIA tab pattern: arrows, Home and End move and select; one tab stop", async ({ page }) => {
    await start(page); await open(page);
    await page.locator("#tab-clock-clock").focus();
    await page.keyboard.press("ArrowRight"); await expect(page.locator("#tab-clock-stopwatch")).toBeFocused();
    await expect(page.locator("#panel-clock-stopwatch")).toBeVisible(); await expect(page.locator("#panel-clock-clock")).toBeHidden();
    await page.keyboard.press("End"); await expect(page.locator("#tab-clock-timer")).toBeFocused(); await expect(page.locator("#panel-clock-timer")).toBeVisible();
    await page.keyboard.press("ArrowRight"); await expect(page.locator("#tab-clock-clock")).toBeFocused();
    await page.keyboard.press("ArrowLeft"); await expect(page.locator("#tab-clock-timer")).toBeFocused();
    await page.keyboard.press("Home"); await expect(page.locator("#tab-clock-clock")).toBeFocused();
    expect(await ev(page, () => [...document.querySelectorAll("#screen-clock [role=tab]")].map((t) => t.tabIndex))).toEqual([0, -1, -1]);
    await expect(page.locator("#panel-clock-clock")).toHaveAttribute("aria-labelledby", "tab-clock-clock");
  });
});

test.describe("Clock: stopwatch", () => {
  test("starts, pauses, resumes and resets from controlled time", async ({ page }) => {
    await start(page); await open(page, "stopwatch");
    await expect(swText(page)).toHaveText("0:00.00");
    await expect(page.locator("#btn-sw-lap")).toBeDisabled(); await expect(page.locator("#btn-sw-reset")).toBeDisabled();
    await page.locator("#btn-sw-start").click();
    await expect(page.locator("#btn-sw-start")).toHaveText("Pause");
    await page.clock.fastForward(5000);
    await page.locator("#btn-sw-start").click();                                         // pause
    await expect(swText(page)).toHaveText("0:05.00");
    await expect(page.locator("#btn-sw-start")).toHaveText("Resume");
    await page.clock.fastForward(60000);                                                  // paused: no change
    await expect(swText(page)).toHaveText("0:05.00");
    await page.locator("#btn-sw-start").click(); await page.clock.fastForward(2500); await page.locator("#btn-sw-start").click();
    await expect(swText(page)).toHaveText("0:07.50");
    await page.locator("#btn-sw-reset").click();
    await expect(swText(page)).toHaveText("0:00.00"); await expect(page.locator("#btn-sw-start")).toHaveText("Start");
    await noErrors(page);
  });
  test("a tab that slept for three hours shows the exact elapsed time at the next look", async ({ page }) => {
    await start(page); await open(page, "stopwatch");
    await page.locator("#btn-sw-start").click();
    await page.clock.fastForward(3 * 3600 * 1000 + 1230);
    await page.locator("#btn-sw-start").click();
    await expect(swText(page)).toHaveText("3:00:01.23");
  });
  test("laps record split and total, newest first, only while running, and Reset clears them", async ({ page }) => {
    await start(page); await open(page, "stopwatch");
    await expect(page.locator("#sw-empty")).toBeVisible();
    await page.locator("#btn-sw-start").click();
    await page.clock.fastForward(1000); await page.locator("#btn-sw-lap").click();
    await page.clock.fastForward(2500); await page.locator("#btn-sw-lap").click();
    const laps = page.locator("#sw-laps .sw-lap");
    await expect(laps).toHaveCount(2); await expect(page.locator("#sw-empty")).toBeHidden();
    await expect(laps.nth(0)).toContainText("Lap 2"); await expect(laps.nth(0)).toContainText("0:02.50"); await expect(laps.nth(0)).toContainText("0:03.50");
    await expect(laps.nth(1)).toContainText("Lap 1"); await expect(laps.nth(1)).toContainText("0:01.00");
    await expect(page.locator("#sw-lap-count")).toHaveText("2 laps");
    await page.locator("#btn-sw-start").click();                                          // pause
    await expect(page.locator("#btn-sw-lap")).toBeDisabled();
    await page.locator("#btn-sw-reset").click();
    await expect(laps).toHaveCount(0); await expect(page.locator("#sw-empty")).toBeVisible();
  });
  test("the stopwatch keeps running while you are on another tab and another screen", async ({ page }) => {
    await start(page); await open(page, "stopwatch");
    await page.locator("#btn-sw-start").click(); await page.clock.fastForward(1000);
    await page.locator("#tab-clock-timer").click(); await ev(page, () => window.AuroraShell.showScreen("screen-home"));
    await page.clock.fastForward(9000);
    await open(page, "stopwatch");
    await page.locator("#btn-sw-start").click();
    await expect(swText(page)).toHaveText("0:10.00");
  });
  test("a rapid burst of Pause/Resume clicks never loses or invents time", async ({ page }) => {
    await start(page); await open(page, "stopwatch");
    await page.locator("#btn-sw-start").click(); await page.clock.fastForward(1000);
    for (let i = 0; i < 20; i++) await page.locator("#btn-sw-start").click();           // even number: ends running
    await page.clock.fastForward(1000);
    await page.locator("#btn-sw-start").click();
    await expect(swText(page)).toHaveText("0:02.00");
  });
});

test.describe("Clock: timer", () => {
  test("presets set the length (1, 5, 10, 25 minutes) and mirror into the h/m/s fields", async ({ page }) => {
    await start(page); await open(page, "timer");
    await expect(timerText(page)).toHaveText("0:05:00");
    for (const [s, text, m] of [[60, "0:01:00", "1"], [300, "0:05:00", "5"], [600, "0:10:00", "10"], [1500, "0:25:00", "25"]]) {
      await page.locator("#btn-timer-preset-" + s).click();
      await expect(timerText(page)).toHaveText(text); await expect(page.locator("#timer-m")).toHaveValue(m);
      await expect(page.locator("#btn-timer-preset-" + s)).toHaveAttribute("aria-pressed", "true");
    }
    await expect(page.locator("#btn-timer-preset-60")).toHaveAccessibleName("1 minute");
    await expect(page.locator("#btn-timer-preset-1500")).toHaveAccessibleName("25 minutes");
  });
  test("runs down from controlled time and finishes exactly at the end with an accessible alert", async ({ page }) => {
    await start(page); await open(page, "timer");
    await page.locator("#btn-timer-preset-60").click(); await page.locator("#btn-timer-start").click();
    await expect(page.locator("#timer-state")).toHaveText("Running"); await expect(page.locator("#btn-timer-start")).toHaveText("Pause");
    await page.clock.fastForward(30000);
    await expect(timerText(page)).toHaveText("0:00:30"); await expect(page.locator("#timer-progress")).toHaveAttribute("aria-valuenow", "50");
    await page.clock.fastForward(29000);
    await expect(timerText(page)).toHaveText("0:00:01"); await expect(page.getByRole("alertdialog")).toHaveCount(0);
    await page.clock.fastForward(1000);
    const dlg = page.getByRole("alertdialog");
    await expect(dlg).toBeVisible(); await expect(dlg).toContainText("Timer finished"); await expect(dlg).toContainText("Your 1 minute timer has finished.");
    await expect(page.locator("#btn-timer-done-dismiss")).toBeFocused();
    await expect(timerText(page)).toHaveText("0:00:00"); await expect(page.locator("#timer-state")).toHaveText("Finished");
    await expect(page.locator("#horizon-text")).toHaveText("Timer finished");
    await page.keyboard.press("Escape"); await expect(dlg).toHaveCount(0);
    await expect(page.locator("#btn-timer-start")).toHaveText("Start again");
    await noErrors(page);
  });
  test("a long jump (tab asleep for an hour) finishes the timer once, with one alert", async ({ page }) => {
    await start(page); await open(page, "timer");
    await page.locator("#btn-timer-preset-300").click(); await page.locator("#btn-timer-start").click();
    await page.clock.fastForward(3600 * 1000);
    await expect(page.getByRole("alertdialog")).toHaveCount(1);
    await page.locator("#btn-timer-done-dismiss").click();
    await page.clock.fastForward(60000);
    await expect(page.getByRole("alertdialog")).toHaveCount(0);
    expect(await ev(page, () => window.AuroraClock.state().timer.state)).toBe("done");
  });
  test("pause freezes the remaining time and resume continues from it", async ({ page }) => {
    await start(page); await open(page, "timer");
    await page.locator("#btn-timer-preset-600").click(); await page.locator("#btn-timer-start").click();
    await page.clock.fastForward(100000);
    await page.locator("#btn-timer-start").click();
    await expect(page.locator("#timer-state")).toHaveText("Paused"); await expect(timerText(page)).toHaveText("0:08:20");
    await page.clock.fastForward(3600 * 1000);
    await expect(timerText(page)).toHaveText("0:08:20"); await expect(page.getByRole("alertdialog")).toHaveCount(0);
    await expect(page.locator("#btn-timer-start")).toHaveText("Resume");
    await page.locator("#btn-timer-start").click(); await page.clock.fastForward(20000);
    await expect(timerText(page)).toHaveText("0:08:00");
  });
  test("reset returns to Ready with the same length; length controls are locked while a timer runs", async ({ page }) => {
    await start(page); await open(page, "timer");
    await page.locator("#btn-timer-preset-60").click(); await page.locator("#btn-timer-start").click();
    for (const sel of ["#btn-timer-preset-300", "#timer-h", "#timer-m", "#timer-s"]) await expect(page.locator(sel)).toBeDisabled();
    await page.clock.fastForward(10000);
    await page.locator("#btn-timer-reset").click();
    await expect(page.locator("#timer-state")).toHaveText("Ready"); await expect(timerText(page)).toHaveText("0:01:00");
    await expect(page.locator("#btn-timer-reset")).toBeDisabled(); await expect(page.locator("#timer-m")).toBeEnabled();
    await page.clock.fastForward(120000); await expect(page.getByRole("alertdialog")).toHaveCount(0);
  });
  test("a custom length from the fields is validated: zero, text, decimals, negative and over 24 h are refused with a reason and Start disabled", async ({ page }) => {
    await start(page); await open(page, "timer");
    const set = async (h, m, s) => { await page.locator("#timer-h").fill(h); await page.locator("#timer-m").fill(m); await page.locator("#timer-s").fill(s); };
    await set("0", "0", "0"); await expect(page.locator("#timer-error")).toHaveText("Set at least 1 second."); await expect(page.locator("#btn-timer-start")).toBeDisabled();
    await expect(page.locator("#timer-m")).toHaveAttribute("aria-invalid", "true");
    await set("0", "abc", "0"); await expect(page.locator("#timer-error")).toContainText("whole numbers");
    await set("0", "1.5", "0"); await expect(page.locator("#timer-error")).toContainText("whole numbers");
    await set("0", "-5", "0"); await expect(page.locator("#timer-error")).toContainText("whole numbers");
    await set("25", "0", "0"); await expect(page.locator("#timer-error")).toContainText("at most 24 hours");
    await set("24", "0", "1"); await expect(page.locator("#timer-error")).toHaveText("The longest timer is 24 hours."); await expect(page.locator("#btn-timer-start")).toBeDisabled();
    await expect(page.locator("#btn-timer-preset-300")).toHaveAttribute("aria-pressed", "false");
    await set("0", "0", "90");                                                            // 90 seconds is fine
    await expect(page.locator("#timer-error")).toBeHidden(); await expect(page.locator("#btn-timer-start")).toBeEnabled(); await expect(timerText(page)).toHaveText("0:01:30");
    await page.locator("#btn-timer-start").click(); await page.clock.fastForward(90000);
    await expect(page.getByRole("alertdialog")).toContainText("Your 1 minute 30 seconds timer has finished.");
  });
  test("the longest allowed timer, 24 hours, starts and counts down", async ({ page }) => {
    await start(page); await open(page, "timer");
    await page.locator("#timer-h").fill("24"); await page.locator("#timer-m").fill("0"); await page.locator("#timer-s").fill("0");
    await expect(timerText(page)).toHaveText("24:00:00");
    await page.locator("#btn-timer-start").click(); await page.clock.fastForward(3600000);
    await expect(timerText(page)).toHaveText("23:00:00");
  });
  test("Start again after finishing runs the same length once more", async ({ page }) => {
    await start(page); await open(page, "timer");
    await page.locator("#btn-timer-preset-60").click(); await page.locator("#btn-timer-start").click(); await page.clock.fastForward(60000);
    await page.locator("#btn-timer-done-dismiss").click();
    await page.locator("#btn-timer-start").click();
    await expect(page.locator("#timer-state")).toHaveText("Running"); await expect(timerText(page)).toHaveText("0:01:00");
    await page.clock.fastForward(60000); await expect(page.getByRole("alertdialog")).toHaveCount(1);
  });
  test("leaving the screen does not stop the timer: remaining time is right on return, and the alert still appears elsewhere", async ({ page }) => {
    await start(page); await open(page, "timer");
    await page.locator("#btn-timer-preset-300").click(); await page.locator("#btn-timer-start").click();
    await ev(page, () => window.AuroraShell.showScreen("screen-home"));
    await page.clock.fastForward(120000);
    await open(page, "timer");
    await expect(timerText(page)).toHaveText("0:03:00"); await expect(page.locator("#timer-state")).toHaveText("Running");
    await ev(page, () => window.AuroraShell.showScreen("screen-home"));
    await page.clock.fastForward(180000);
    await expect(page.getByRole("alertdialog")).toBeVisible();                           // shown on Home, over whatever you were doing
    await page.locator("#btn-timer-done-show").click();
    await expect(page.locator("#screen-clock")).toHaveClass(/active/); await expect(page.locator("#panel-clock-timer")).toBeVisible();
    await expect(page.locator("#timer-state")).toHaveText("Finished");
  });
  test("accessible semantics: timer role, labelled progress bar, named controls", async ({ page }) => {
    await start(page); await open(page, "timer");
    await expect(page.getByRole("timer", { name: "Time remaining" })).toBeVisible();
    await expect(page.getByRole("progressbar", { name: "Timer progress" })).toHaveAttribute("aria-valuemax", "100");
    await expect(page.getByRole("group", { name: "Presets" })).toBeVisible();
    for (const n of ["Hours", "Minutes", "Seconds"]) await expect(page.getByRole("textbox", { name: n })).toBeVisible();
    await expect(page.getByRole("button", { name: "Start" })).toBeVisible();
    await expect(page.getByRole("button", { name: "Reset" })).toBeVisible();
  });
  test("reduced motion: the progress bar does not animate", async ({ page }) => {
    await page.emulateMedia({ reducedMotion: "reduce" });
    await start(page); await open(page, "timer");
    expect(await page.locator(".timer-fill").evaluate((n) => getComputedStyle(n).transitionDuration)).toMatch(/^0s$/);
  });
});

test.describe("Clock: Action Broker timer (Aura)", () => {
  test("an Aura timer.start appears in the Clock with the right remaining time and a source label, and only one timer exists", async ({ page }) => {
    await start(page); await open(page, "timer");
    const r = await auraTimer(page, 600);
    expect(r.ok).toBe(true);
    await expect(page.locator("#timer-state")).toHaveText("Running"); await expect(timerText(page)).toHaveText("0:10:00");
    await expect(page.locator("#timer-source")).toBeVisible(); await expect(page.locator("#timer-source")).toHaveText("Started by Aura");
    await expect(page.locator("#timer-m")).toHaveValue("10");
    expect(await ev(page, () => window.AuroraSystem.world.timer.seconds)).toBe(600);
    await page.clock.fastForward(120000);
    await expect(timerText(page)).toHaveText("0:08:00");
    expect(await ev(page, () => window.AuroraClock.state().timer.remaining)).toBe(480000);
  });
  test("started from anywhere: opening the Clock afterwards shows the remaining time, not a fresh timer", async ({ page }) => {
    await start(page);
    await auraTimer(page, 300);                                                          // Clock never opened yet
    await page.clock.fastForward(100000);
    await open(page, "timer");
    await expect(timerText(page)).toHaveText("0:03:20"); await expect(page.locator("#timer-state")).toHaveText("Running");
  });
  test("completion is reported: alert, Horizon status, and the Finished state", async ({ page }) => {
    await start(page); await open(page, "timer");
    await auraTimer(page, 60);
    await page.clock.fastForward(60000);
    await expect(page.getByRole("alertdialog")).toContainText("Your 1 minute timer has finished.");
    await expect(page.locator("#horizon-text")).toHaveText("Timer finished");
    await expect(page.locator("#timer-state")).toHaveText("Finished");
    await expect(page.locator("#timer-source")).toBeVisible();
  });
  test("Undo of the receipt cancels the running Aura timer and the broker's record", async ({ page }) => {
    await start(page); await open(page, "timer");
    await auraTimer(page, 600);
    await page.clock.fastForward(30000);
    const id = await ev(page, () => window.AuroraSystem.receipts().find((x) => x.type === "timer.start").id);
    const u = await ev(page, (i) => window.AuroraSystem.undo(i), id);
    expect(u.ok).toBe(true);
    await expect(page.locator("#timer-state")).toHaveText("Ready"); await expect(page.locator("#timer-source")).toBeHidden();
    await expect(timerText(page)).toHaveText("0:10:00");
    expect(await ev(page, () => window.AuroraSystem.world.timer)).toBeNull();
    await page.clock.fastForward(700000); await expect(page.getByRole("alertdialog")).toHaveCount(0);
  });
  test("Undo from the Trail screen cancels it too", async ({ page }) => {
    await start(page); await open(page, "timer");
    await auraTimer(page, 600);
    await ev(page, () => window.AuroraShell.openApp("trail"));
    await page.locator("#screen-trail .trail-undo").first().click();
    await expect(page.locator("#screen-trail .trail-undo")).toHaveCount(0);        // the broker answered; Trail shows it as used
    await open(page, "timer");
    await expect(page.locator("#timer-state")).toHaveText("Ready");
  });
  test("a second Aura timer replaces the first; undoing it restores the first with the right remaining time", async ({ page }) => {
    await start(page); await open(page, "timer");
    await auraTimer(page, 900); await page.clock.fastForward(60000);
    await auraTimer(page, 300);
    await expect(timerText(page)).toHaveText("0:05:00");
    const ids = await ev(page, () => window.AuroraSystem.receipts().filter((x) => x.type === "timer.start").map((x) => x.id));
    await ev(page, (i) => window.AuroraSystem.undo(i), ids[0]);                          // newest receipt first
    await expect(timerText(page)).toHaveText("0:14:00"); await expect(page.locator("#timer-state")).toHaveText("Running");
  });
  test("undo does not touch a timer the user started themselves afterwards", async ({ page }) => {
    await start(page); await open(page, "timer");
    await auraTimer(page, 600);
    await page.locator("#btn-timer-reset").click();                                      // user takes over...
    await page.locator("#btn-timer-preset-60").click(); await page.locator("#btn-timer-start").click();
    await expect(page.locator("#timer-source")).toBeHidden();
    const id = await ev(page, () => window.AuroraSystem.receipts().find((x) => x.type === "timer.start").id);
    await ev(page, (i) => window.AuroraSystem.undo(i), id);
    await expect(page.locator("#timer-state")).toHaveText("Running"); await expect(timerText(page)).toHaveText("0:01:00");
  });
  test("the same broker timer announced twice does not restart the countdown", async ({ page }) => {
    await start(page); await open(page, "timer");
    await auraTimer(page, 600); await page.clock.fastForward(60000);
    await ev(page, () => window.AuroraSystem.emit("timer", window.AuroraSystem.world.timer));
    await expect(timerText(page)).toHaveText("0:09:00");
  });
  test("an Aura timer is paused and resumed by the Clock like any other; the broker record stays as it was", async ({ page }) => {
    await start(page); await open(page, "timer");
    await auraTimer(page, 600); await page.clock.fastForward(10000);
    await page.locator("#btn-timer-start").click(); await page.clock.fastForward(100000);
    await expect(timerText(page)).toHaveText("0:09:50");
    expect(await ev(page, () => window.AuroraSystem.world.timer.seconds)).toBe(600);
    await page.locator("#btn-timer-start").click(); await page.clock.fastForward(10000);
    await expect(timerText(page)).toHaveText("0:09:40");
  });
  test("a rejected Aura request starts nothing", async ({ page }) => {
    await start(page); await open(page, "timer");
    const r = await ev(page, () => window.AuroraSystem.request({ type: "timer.start", arguments: { seconds: 0 } }, { source: "test" }));
    expect(r.ok).toBe(false);
    await expect(page.locator("#timer-state")).toHaveText("Ready");
    const r2 = await ev(page, () => window.AuroraSystem.request({ type: "timer.start", arguments: { seconds: 90000 } }, { source: "test" }));
    expect(r2.ok).toBe(false); await expect(page.locator("#timer-state")).toHaveText("Ready");
  });
  test("the Clock's own timer does not touch the broker: no receipt, no world timer", async ({ page }) => {
    await start(page); await open(page, "timer");
    const before = await ev(page, () => window.AuroraSystem.receipts().length);
    await page.locator("#btn-timer-preset-60").click(); await page.locator("#btn-timer-start").click(); await page.clock.fastForward(60000);
    expect(await ev(page, () => window.AuroraSystem.receipts().length)).toBe(before);
    expect(await ev(page, () => window.AuroraSystem.world.timer)).toBeNull();
  });
});

test.describe("Clock: permissions and persistence", () => {
  test("never asks for Notification permission, never makes sound, never stores anything", async ({ page }) => {
    await start(page); await open(page, "timer");
    await page.locator("#btn-timer-preset-60").click(); await page.locator("#btn-timer-start").click(); await page.clock.fastForward(60000);
    await page.locator("#btn-timer-done-dismiss").click();
    await open(page, "stopwatch"); await page.locator("#btn-sw-start").click();
    expect(await ev(page, () => [window.__notif, window.__audio])).toEqual([0, 0]);
    expect(await ev(page, () => [Object.keys(localStorage).length, Object.keys(sessionStorage).length])).toEqual([0, 0]);
  });
  test("a reload forgets everything, and the screen says timers do not outlive the page", async ({ page }) => {
    await start(page); await open(page, "timer");
    await expect(page.locator("#screen-clock")).toContainText("only while this page is open");
    await page.locator("#btn-timer-preset-60").click(); await page.locator("#btn-timer-start").click();
    await page.reload(); await open(page, "timer");
    await expect(page.locator("#timer-state")).toHaveText("Ready");
  });
});

test.describe("Clock: layouts", () => {
  test.describe("phone 390", () => {
    test.use({ viewport: { width: 390, height: 844 } });
    test("no horizontal overflow and every control is at least 44px, on all three tabs", async ({ page }) => {
      await start(page); await open(page);
      for (const tab of ["clock", "stopwatch", "timer"]) {
        await page.locator("#tab-clock-" + tab).click();
        expect(await ev(page, () => document.documentElement.scrollWidth <= window.innerWidth), tab).toBe(true);
        const small = await page.locator("#panel-clock-" + tab + " button, #panel-clock-" + tab + " input, .clock-tab").evaluateAll((ns) => ns.filter((n) => { const r = n.getBoundingClientRect(); return r.width && r.height && Math.min(r.width, r.height) < 44; }).map((n) => n.id));
        expect(small, tab).toEqual([]);
      }
    });
    test("the finish alert fits the screen with its actions reachable", async ({ page }) => {
      await start(page); await open(page, "timer");
      await page.locator("#btn-timer-preset-60").click(); await page.locator("#btn-timer-start").click(); await page.clock.fastForward(60000);
      const box = await page.locator("#timer-done-dialog").boundingBox();
      expect(box.x).toBeGreaterThanOrEqual(0); expect(box.x + box.width).toBeLessThanOrEqual(390); expect(box.y + box.height).toBeLessThanOrEqual(844);
      await expect(page.locator("#btn-timer-done-dismiss")).toBeInViewport();
    });
  });
  test.describe("text 200% at 360", () => {
    test.use({ viewport: { width: 360, height: 780 } });
    test("a 24-hour display, the laps and the fields stay inside the viewport", async ({ page }) => {
      await start(page); await open(page, "timer");
      await ev(page, () => document.documentElement.setAttribute("data-text-scale", "200"));
      await page.locator("#timer-h").fill("24"); await page.locator("#timer-m").fill("0"); await page.locator("#timer-s").fill("0");
      expect(await ev(page, () => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
      await page.locator("#tab-clock-stopwatch").click(); await page.locator("#btn-sw-start").click(); await page.clock.fastForward(36000000); await page.locator("#btn-sw-lap").click();
      expect(await ev(page, () => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
    });
  });
  test.describe("desktop 1280", () => {
    test.use({ viewport: { width: 1280, height: 800 } });
    test("the panel is a centred column, not stretched across the window", async ({ page }) => {
      await start(page); await open(page, "timer");
      const b = await page.locator("#panel-clock-timer").boundingBox();
      expect(b.width).toBeLessThanOrEqual(720);
    });
  });
});
