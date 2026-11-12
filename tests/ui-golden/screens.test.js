/**
 * tests/ui-golden/screens.test.js — M1 / S6: Pages, Quick Center and Settings on the shared design system.
 * The existing pages-quick.test.js and settings.test.js still guard every id, role and data-* attribute they use.
 */
const { test, expect } = require("@playwright/test");

const BASE_URL = "http://localhost:3000";
const ev = (page, fn, arg) => page.evaluate(fn, arg);

test.describe("Pages", () => {
  test.beforeEach(async ({ page }) => { await page.goto(BASE_URL); await page.locator("#dock-pages").click(); });

  test("each card has an app tile, a title button, an icon+text state chip, meta, a hint and actions", async ({ page }) => {
    const notes = page.locator('[data-page="page-notes"]');
    await expect(notes.locator(".app-tile")).toHaveCount(1);
    await expect(notes.getByRole("button", { name: "Open Notes" })).toBeVisible();
    await expect(notes.locator(".state-badge")).toHaveText("Running");
    await expect(notes.locator(".state-badge svg.icon")).toHaveCount(1);
    await expect(notes.locator(".page-meta")).toHaveText("Study Space · 180 MB");
    await expect(notes.locator(".page-hint")).toContainText("Pause it to keep your place");
    await expect(page.locator('[data-page="page-clock"] .page-hint')).toContainText("Save it to free the memory");
    await expect(page.locator('[data-page="page-calc"] .page-hint')).toContainText("Resuming needs 60 MB");
    await expect(page.locator('[data-page="page-calc"] .state-badge')).toHaveText("Saved (frozen)");
  });

  test("the memory card says it is simulated and shows the meter", async ({ page }) => {
    await expect(page.locator("#pages-memory .chip")).toHaveText("Simulated");
    await expect(page.locator("#pages-meter")).toHaveAttribute("aria-valuetext", /percent used/);
  });

  test("Close is an icon button that keeps its accessible name", async ({ page }) => {
    const close = page.getByRole("button", { name: "Close Clock" });
    await expect(close).toHaveAttribute("title", "Close");
    expect(await close.evaluate((b) => b.textContent.trim())).toBe("");
    const r = await close.boundingBox();
    expect(Math.min(r.width, r.height)).toBeGreaterThanOrEqual(48);
  });

  test("the title opens the app when it has a screen, and says so honestly when it does not", async ({ page }) => {
    await page.getByRole("button", { name: "Open Notes" }).click();
    await expect(page.locator("#toast")).toContainText("Notes has no screen in this prototype yet");
    await expect(page.locator("#screen-pages")).toBeVisible();
    await ev(page, () => window.AuroraShell.openApp("settings"));
    await ev(page, () => window.AuroraShell.showScreen("screen-pages"));
    await page.getByRole("button", { name: "Open Settings" }).click();
    await expect(page.locator("#screen-settings")).toBeVisible();
  });

  test("the empty state explains how to get a page", async ({ page }) => {
    await ev(page, () => window.AuroraModels.pages.list().forEach((p) => window.AuroraModels.pages.close(p.id)));
    await expect(page.locator("#pages-empty")).toContainText("No pages are open");
    await expect(page.locator("#pages-empty")).toContainText("Open an app from Home or Index");
  });

  test("a page named like markup is text", async ({ page }) => {
    await ev(page, () => window.AuroraModels.pages.open("evil", "<img src=x onerror=1>", "study", 10));
    await expect(page.locator('[data-page="page-evil"] .page-open')).toHaveText("<img src=x onerror=1>");
    await expect(page.locator("#screen-pages img")).toHaveCount(0);
  });

  test("uses the screen header pattern with a Back button", async ({ page }) => {
    await expect(page.locator("#screen-pages .screen-header h1.screen-title")).toHaveText("Pages");
    await page.getByRole("button", { name: "Back" }).first().click();
    await expect(page.locator("#screen-home")).toBeVisible();
  });
});

test.describe("Quick Center", () => {
  test.beforeEach(async ({ page }) => { await page.goto(BASE_URL); await page.locator("#btn-quick-center").click(); });

  test("tiles show an icon, label, state and what they really do; simulated ones say so", async ({ page }) => {
    const note = (k) => page.locator('[data-setting="' + k + '"] .tile-note');
    await expect(note("wifi")).toHaveText("Simulated");
    await expect(note("bluetooth")).toHaveText("Simulated");
    await expect(note("rotationLock")).toHaveText("Simulated");
    await expect(note("batterySaver")).toHaveText("Uses Clear glass");
    await expect(note("focus")).toHaveText("Shown on Home");
    for (const k of ["wifi", "bluetooth", "batterySaver", "focus", "rotationLock"]) await expect(page.locator('[data-setting="' + k + '"] > svg.icon')).toHaveCount(1);
    await expect(page.locator('[data-setting="wifi"] .tile-state')).toHaveText("On");
  });

  test("sliders carry icons and notes about what they affect", async ({ page }) => {
    await expect(page.locator("#qc-brightness-note")).toHaveText("Dims this prototype only.");
    await expect(page.locator("#qc-volume-note")).toContainText("Simulated");
    await expect(page.locator('label[for="qc-brightness"] svg.icon')).toHaveCount(1);
    await expect(page.locator("#qc-brightness")).toHaveAttribute("aria-describedby", "qc-brightness-note");
  });

  test("privacy rows have icons and unchanged text", async ({ page }) => {
    await expect(page.locator('[data-sensor="microphone"] svg.icon')).toHaveCount(1);
    await expect(page.locator('[data-sensor="microphone"]')).toContainText("Microphone: not in use");
    await page.getByRole("button", { name: "Simulate microphone" }).click();
    await expect(page.locator('[data-sensor="microphone"]')).toContainText("in use (simulated)");
    await expect(page.locator('[data-sensor="microphone"]')).toHaveClass(/active/);
  });

  test("focus mode tile and Home chip share state", async ({ page }) => {
    await page.locator('[data-setting="focus"]').click();
    await page.locator("#screen-quick .screen-header button").first().click();
    await expect(page.locator("#home-focus-chip")).toBeVisible();
  });

  test("Appearance is still a select and still switches the theme", async ({ page }) => {
    await page.locator("#qc-appearance").selectOption("dark");
    await expect(page.locator("html")).toHaveAttribute("data-theme", "dark");
  });

  test("All settings opens Settings and Back returns here", async ({ page }) => {
    await page.getByRole("button", { name: "Open all settings" }).click();
    await expect(page.locator("#screen-settings")).toBeVisible();
    await page.locator("#btn-settings-back").click();
    await expect(page.locator("#screen-quick")).toBeVisible();
  });
});

test.describe("Settings: restyle and the simulated failure switch", () => {
  const ctl = (page, id) => page.locator('[data-setting="' + id + '"] [data-fk="' + id + '"]');
  async function openSettings(page) { await page.goto(BASE_URL); await page.getByRole("button", { name: "Open Settings" }).click(); await expect(page.locator("#screen-settings")).toBeVisible(); }
  async function runStudyPlan(page) {
    await ev(page, () => window.AuroraShell.showScreen("screen-home"));
    await page.locator("#btn-aura-study").click();
    await page.locator("#btn-plan-confirm").click();
    await page.locator("#btn-plan-run").click();
  }

  test("groups are opaque cards and the screen uses the screen header", async ({ page }) => {
    await openSettings(page);
    await expect(page.locator("#screen-settings .screen-header h1.screen-title")).toHaveText("Settings");
    expect(await page.locator(".settings-group").first().evaluate((c) => c.classList.contains("opaque"))).toBe(true);
  });

  test("the failure switch is an advanced setting: hidden by default, findable through Index", async ({ page }) => {
    await openSettings(page);
    await expect(page.locator('[data-setting="developer.failAuraStep"]')).toHaveCount(0);
    await ev(page, () => window.AuroraShell.openIndex("fail"));
    await expect(page.locator("#index-results .index-result", { hasText: "Make the next Aura plan fail" })).toBeVisible();
    await page.keyboard.press("Enter");
    await expect(ctl(page, "developer.failAuraStep")).toBeFocused();
    await expect(page.locator('[data-setting="developer.failAuraStep"]')).toContainText("(simulated)");
  });

  test("turning it on arms the simulated failure; turning it off disarms it", async ({ page }) => {
    await openSettings(page);
    await ev(page, () => window.AuroraSettingsModel.set("developer.showAdvanced", true));
    await ctl(page, "developer.failAuraStep").click();
    expect(await ev(page, () => window.AuroraSystem.dev.failNextType)).toBe("app.open");
    await ctl(page, "developer.failAuraStep").click();
    expect(await ev(page, () => window.AuroraSystem.dev.failNextType)).toBeNull();
  });

  test("the armed failure breaks step 3, is explained, leaves steps 1-2 undoable, and switches itself off", async ({ page }) => {
    await openSettings(page);
    await ev(page, () => { window.AuroraSettingsModel.set("developer.showAdvanced", true); window.AuroraSettingsModel.set("developer.failAuraStep", true); });
    await runStudyPlan(page);
    await expect(page.locator("#plan-failure")).toBeVisible({ timeout: 8000 });
    await expect(page.locator("#plan-failure")).toContainText("Plan did not finish");
    await expect(page.locator('#plan-failure [data-status="failed"]')).toContainText("Simulated failure");
    const r = await ev(page, () => window.AuroraSystem.receipts().map((x) => x.type + ":" + x.status + ":" + x.undoState).reverse());
    expect(r[0]).toMatch(/^space\.switch:done:available/);
    expect(r[1]).toMatch(/^focus\.set:done:available/);
    expect(r[2]).toMatch(/^app\.open:failed:/);
    expect(await ev(page, () => window.AuroraSettingsModel.get("developer.failAuraStep"))).toBe(false);   // used once, now off
    expect(await ev(page, () => window.AuroraSystem.dev.failNextType)).toBeNull();
    // the next plan succeeds
    await page.locator("#btn-receipt-close").click();
    await runStudyPlan(page);
    await expect(page.locator("#plan-receipt")).toBeVisible({ timeout: 8000 });
  });

  test("Reset all settings disarms the failure switch", async ({ page }) => {
    await openSettings(page);
    await ev(page, () => { window.AuroraSettingsModel.set("developer.showAdvanced", true); window.AuroraSettingsModel.set("developer.failAuraStep", true); });
    await page.locator("#btn-settings-reset").click();
    await page.locator("#btn-settings-reset-ok").click();
    expect(await ev(page, () => window.AuroraSystem.dev.failNextType)).toBeNull();
  });

  test("the reset confirmation is the shared modal dialog: scrim, trapped focus, Cancel changes nothing", async ({ page }) => {
    await openSettings(page);
    await ctl(page, "appearance.theme").selectOption("dark");
    await page.locator("#btn-settings-reset").click();
    await expect(page.locator(".scrim .dialog#settings-reset-confirm")).toBeVisible();
    await expect(page.locator("#settings-reset-confirm")).toHaveAttribute("aria-modal", "true");
    await expect(page.locator("#settings-reset-confirm .dialog-title")).toHaveText("Reset all settings?");
    await page.locator("#btn-settings-reset-cancel").click();
    await expect(page.locator("#settings-reset-confirm")).toHaveCount(0);
    await expect(page.locator("html")).toHaveAttribute("data-theme", "dark");
    await expect(page.locator("#btn-settings-reset")).toBeFocused();
  });

  test("no legacy header or inline confirm markup remains anywhere", async ({ page }) => {
    await page.goto(BASE_URL);
    expect(await ev(page, () => document.querySelectorAll(".home-header, .home-title, .icon-btn, .confirm-panel").length)).toBe(0);
  });
});
