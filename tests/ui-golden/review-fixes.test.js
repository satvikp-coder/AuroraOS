/**
 * tests/ui-golden/review-fixes.test.js — regression tests for defects found by the independent M1 review.
 * One test per finding; each fails on the code as reviewed.
 */
const { test, expect } = require("@playwright/test");

const BASE_URL = "http://localhost:3000";
const ev = (page, fn, arg) => page.evaluate(fn, arg);
async function start(page, delay) { await page.goto(BASE_URL); await ev(page, (d) => { window.AuroraSystem.dev.stepDelayMs = d; }, delay === undefined ? 20 : delay); }

test("1. focus lands on the Plan heading when the screen opens and survives re-renders", async ({ page }) => {
  await start(page);
  await page.locator("#btn-aura-study").click();
  await expect(page.locator("#screen-aura h1")).toBeFocused();
  await page.getByRole("button", { name: /Skip step 2/ }).focus();
  await page.keyboard.press("Enter");                                           // re-renders the screen
  await expect(page.getByRole("button", { name: /Include step 2/ })).toBeFocused();
  await page.locator("#btn-aura-back").focus();
  await ev(page, () => window.AuroraShell.showScreen("screen-aura"));            // re-open: heading again, never <body>
  await expect(page.locator("#screen-aura h1")).toBeFocused();
});

test("1b. keyboard focus on Back is kept while a plan reports progress", async ({ page }) => {
  await start(page, 250);
  await page.locator("#btn-aura-study").click();
  await page.locator("#btn-plan-confirm").click();
  await page.locator("#btn-plan-run").click();
  await expect(page.locator("#plan-running")).toBeVisible();
  await page.locator("#btn-aura-back").focus();
  await page.waitForTimeout(700);                                                 // several progress re-renders
  await expect(page.locator("#btn-aura-back")).toBeFocused();
});

test("2. Ctrl+K does nothing behind a modal dialog, and stacked dialogs: only the top one handles Escape", async ({ page }) => {
  await start(page);
  await ev(page, () => { window.AuroraSystem.request({ type: "file.move", arguments: { id: "timetable", destination: "work" } }, { source: "t" }); });
  await expect(page.locator("#review-dialog")).toBeVisible();
  await page.keyboard.press("Control+k");
  await expect(page.locator("#screen-index")).toBeHidden();
  await expect(page.locator(".dialog")).toHaveCount(1);
  await ev(page, () => { window.AuroraUI.dialog({ id: "second", title: "Second", body: "x", actions: [{ label: "Close", value: 1 }] }); });
  await expect(page.locator(".dialog")).toHaveCount(2);
  await page.keyboard.press("Escape");
  await expect(page.locator("#second")).toHaveCount(0);
  await expect(page.locator("#review-dialog")).toBeVisible();                     // the lower dialog was NOT closed by the same key press
  await page.keyboard.press("Escape");
  await expect(page.locator(".dialog")).toHaveCount(0);
});

test("3. a single 'open settings' command actually opens Settings; plan steps still do not navigate", async ({ page }) => {
  await start(page);
  await ev(page, () => window.AuroraShell.openIndex("open settings"));
  await page.keyboard.press("Enter");
  await expect(page.locator("#screen-settings")).toBeVisible();
  await ev(page, () => window.AuroraShell.showScreen("screen-home"));
  await page.locator("#btn-aura-study").click();
  await page.locator("#btn-plan-confirm").click();
  await page.locator("#btn-plan-run").click();
  await expect(page.locator("#plan-receipt")).toBeVisible({ timeout: 8000 });     // the Open Notes step did not pull us to Pages
});

test("4. a module can supply the screen of a reserved system app (Vault; Trail has its own screen now, Vault gets one in M4) and it becomes reachable", async ({ page }) => {
  await start(page);
  expect(await ev(page, () => window.AuroraShell.openApp("vault"))).toEqual({ ok: false, reason: "not-available" });
  await ev(page, () => { const s = window.AuroraShell.addScreen("screen-vault", "Vault"); s.textContent = "vault"; });
  expect(await ev(page, () => window.AuroraShell.openApp("vault"))).toMatchObject({ ok: true, id: "vault" });
  await expect(page.locator("#screen-vault")).toBeVisible();
  await page.locator("#glass-corner").click();
  await expect(page.locator("#btn-gc-trail")).toBeEnabled();                      // the Glass Corner notices
});

test("5. starting a second plan while one is running is refused; the receipt stays the running plan's", async ({ page }) => {
  await start(page, 400);
  await page.locator("#btn-aura-study").click();
  await page.locator("#btn-plan-confirm").click();
  await page.locator("#btn-plan-run").click();
  await expect(page.locator("#plan-running")).toBeVisible();
  const r = await ev(page, () => window.AuroraReview.ask("prepare my phone for studying"));
  expect(r).toEqual({ ok: false, reason: "busy" });
  await expect(page.locator("#toast")).toContainText("still running");
  await expect(page.locator("#plan-receipt")).toBeVisible({ timeout: 10000 });
  await page.locator("#btn-plan-undo-all").click();
  await expect(page.locator("#toast")).toContainText("Completed steps were undone.");
  expect(await ev(page, () => window.AuroraSystem.world.timer)).toBeNull();
});

test("6. Undo all after undoing one step is not reported as a failure", async ({ page }) => {
  await start(page);
  await page.locator("#btn-aura-study").click();
  await page.locator("#btn-plan-confirm").click();
  await page.locator("#btn-plan-run").click();
  await expect(page.locator("#plan-receipt")).toBeVisible({ timeout: 8000 });
  await page.getByRole("button", { name: "Undo: Turn Focus mode on" }).click();
  await expect(page.locator("#plan-receipt .chip", { hasText: /^Undone$/ })).toHaveCount(1);
  await page.locator("#btn-plan-undo-all").click();
  await expect(page.locator("#toast")).toContainText("Completed steps were undone.");
  await expect(page.locator("#toast")).not.toContainText("could not");
});

test("7. items that arrive while the Glass Corner is open do not come back as 'new' after closing it", async ({ page }) => {
  await start(page, 0);
  await page.locator("#glass-corner").click();
  await ev(page, () => window.AuroraSystem.request({ type: "timer.start", arguments: { seconds: 60 } }, { source: "t" }));
  await expect(page.locator("#gc-panel [data-receipt]")).toHaveCount(1);
  await page.locator("#glass-corner").click();
  await expect(page.locator("#gc-badge")).toBeHidden();
  await ev(page, () => window.AuroraSystem.request({ type: "timer.start", arguments: { seconds: 90 } }, { source: "t" }));
  await expect(page.locator("#gc-badge")).toHaveText("1");
});
