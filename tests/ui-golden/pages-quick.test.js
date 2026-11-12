/** Browser tests for the simulated Pages and Quick Center screens (real Chromium). */
const { test, expect } = require("@playwright/test");
const BASE_URL = "http://localhost:3000";

test.describe("Pages", () => {
  test.beforeEach(async ({ page }) => { await page.goto(BASE_URL); });

  test("dock opens Pages with states, spaces and memory meter", async ({ page }) => {
    await page.locator("#dock-pages").click();
    await expect(page.locator("#screen-pages")).toBeVisible();
    await expect(page.locator("#dock-pages")).toHaveAttribute("aria-current", "page");
    await expect(page.locator(".page-card")).toHaveCount(4);
    await expect(page.locator('[data-page="page-notes"] .state-badge')).toHaveText(/Running/);
    await expect(page.locator('[data-page="page-calc"] .state-badge')).toHaveText(/Saved/);
    await expect(page.locator("#pages-memory-text")).toHaveText("510 / 2048 MB");
    await expect(page.locator('[data-page="page-files"] .page-meta')).toContainText("Work Space");
  });

  test("pause, save and resume change state and memory; focus is kept", async ({ page }) => {
    await page.locator("#dock-pages").click();
    await page.getByRole("button", { name: "Pause Notes" }).focus();
    await page.keyboard.press("Enter");
    await expect(page.locator('[data-page="page-notes"]')).toHaveAttribute("data-state", "paused");
    await expect(page.getByRole("button", { name: "Resume Notes" })).toBeFocused();
    await page.getByRole("button", { name: "Save Notes and free memory" }).click();
    await expect(page.locator("#pages-memory-text")).toHaveText("330 / 2048 MB");
    await page.getByRole("button", { name: "Resume Notes" }).click();
    await expect(page.locator('[data-page="page-notes"]')).toHaveAttribute("data-state", "running");
    await expect(page.locator("#pages-memory-text")).toHaveText("510 / 2048 MB");
  });

  test("close removes the page; empty state appears", async ({ page }) => {
    await page.locator("#dock-pages").click();
    for (const n of ["Notes", "Clock", "Files", "Calculator"]) await page.getByRole("button", { name: "Close " + n }).click();
    await expect(page.locator(".page-card")).toHaveCount(0);
    await expect(page.locator("#pages-empty")).toBeVisible();
    await expect(page.locator("#pages-memory-text")).toHaveText("0 / 2048 MB");
  });

  test("low-memory warning, refused resume, then Save paused pages recovers", async ({ page }) => {
    await page.locator("#dock-pages").click();
    // Notes 180 + Clock 90 (paused) + Files 240 (paused) = 510 MB; push usage to 1990 of 2048 MB.
    await page.evaluate(() => { const m = window.AuroraModels.pages; m.open("big1", "Big1", "work", 700); m.open("big2", "Big2", "work", 700); m.open("big3", "Big3", "work", 80); });
    await expect(page.locator("#pages-lowmem")).toBeVisible();
    await expect(page.locator("#pages-meter-fill")).toHaveClass(/low/);
    // Resuming the saved Calculator (60 MB) would exceed the budget: it must be refused.
    await page.getByRole("button", { name: "Resume Calculator" }).click();
    await expect(page.locator('[data-page="page-calc"]')).toHaveAttribute("data-state", "saved");
    await page.getByRole("button", { name: "Save paused pages" }).click();
    await page.getByRole("button", { name: "Resume Calculator" }).click();
    await expect(page.locator('[data-page="page-calc"]')).toHaveAttribute("data-state", "running");
  });

  test("pinned tools open a real page entry instead of doing nothing", async ({ page }) => {
    await page.getByRole("button", { name: "Open Calendar" }).click();
    await expect(page.locator("#screen-pages")).toBeVisible();
    await expect(page.locator('[data-page="page-calendar"] .state-badge')).toHaveText(/Running/);
  });

  test("completing the Aura study plan enables focus and opens Notes", async ({ page }) => {
    await page.locator("#btn-aura-study").click();
    await page.locator("#btn-plan-confirm").click();
    await page.locator("#btn-plan-run").click();
    await expect(page.locator("#plan-receipt")).toBeVisible({ timeout: 8000 });
    await page.locator("#btn-receipt-close").click();
    await page.locator("#btn-quick-center").click();
    await expect(page.locator('[data-setting="focus"]')).toHaveAttribute("aria-pressed", "true");
  });
});

test.describe("Quick Center", () => {
  test.beforeEach(async ({ page }) => { await page.goto(BASE_URL); await page.locator("#btn-quick-center").click(); });

  test("opens from Home and goes back", async ({ page }) => {
    await expect(page.locator("#screen-quick")).toBeVisible();
    await page.locator("#btn-quick-back").click();
    await expect(page.locator("#screen-home")).toBeVisible();
  });

  test("toggle tiles flip aria-pressed and visible text, keyboard operable", async ({ page }) => {
    const wifi = page.locator('[data-setting="wifi"]');
    await expect(wifi).toHaveAttribute("aria-pressed", "true");
    await wifi.focus(); await page.keyboard.press("Space");
    await expect(wifi).toHaveAttribute("aria-pressed", "false");
    await expect(wifi).toContainText("Off");
    await expect(wifi).toBeFocused();
  });

  test("brightness slider visibly dims the prototype; volume 0 reads Muted", async ({ page }) => {
    await page.locator("#qc-brightness").fill("0");
    await expect(page.locator("#qc-brightness-out")).toHaveText("0%");
    expect(await page.evaluate(() => getComputedStyle(document.body, "::after").opacity)).toBe("0.5");
    await page.locator("#qc-volume").fill("0");
    await expect(page.locator("#qc-volume-out")).toHaveText("Muted");
  });

  test("appearance changes the real theme; battery saver forces Clear tier and restores", async ({ page }) => {
    await page.locator("#qc-appearance").selectOption("dark");
    await expect(page.locator("html")).toHaveAttribute("data-theme", "dark");
    await expect(page.locator("html")).toHaveAttribute("data-glass-tier", "soft");
    await page.locator('[data-setting="batterySaver"]').click();
    await expect(page.locator("html")).toHaveAttribute("data-glass-tier", "clear");
    await page.locator('[data-setting="batterySaver"]').click();
    await expect(page.locator("html")).toHaveAttribute("data-glass-tier", "soft");
  });

  test("privacy indicators follow simulated sessions", async ({ page }) => {
    await expect(page.locator('[data-sensor="camera"]')).toContainText("not in use");
    await page.getByRole("button", { name: "Simulate camera" }).click();
    await expect(page.locator('[data-sensor="camera"]')).toContainText("in use (simulated)");
    await expect(page.getByRole("button", { name: "Stop camera" })).toBeFocused();
  });

  test("no horizontal overflow at 360px", async ({ page }) => {
    await page.setViewportSize({ width: 360, height: 800 });
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
    await page.locator("#dock-pages").click();
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
  });
});
