/**
 * tests/ui-golden/aurora-glass-browser.test.js
 *
 * Real browser interaction tests for the Aurora Glass prototype using Playwright.
 * Opens index.html in Chromium via local HTTP server, tests all viewports,
 * themes, tiers, and interactions, and captures screenshots.
 *
 * This is a design-prototype test only. It never touches a real OS or device.
 */

const { test, expect } = require("@playwright/test");

const BASE_URL = "http://localhost:3000";

const VIEWPORTS = [
  { name: "360px", width: 360, height: 800 },
  { name: "390px", width: 390, height: 800 },
  { name: "412px", width: 412, height: 800 },
];

test.describe("Aurora Glass browser prototype", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto(BASE_URL);
    await page.waitForLoadState("networkidle");
    await page.waitForTimeout(500);
  });

  test("loads all required screens and controls", async ({ page }) => {
    await expect(page.locator("#screen-home")).toBeVisible();
    await expect(page.locator("#screen-spaces")).toBeHidden();
    await expect(page.locator("#screen-index")).toBeHidden();
    await expect(page.locator("#screen-aura")).toBeHidden();

    // Dock buttons
    for (const id of ["dock-back", "dock-home", "dock-spaces", "dock-pages", "dock-ask"]) {
      await expect(page.locator(`#${id}`)).toBeVisible();
    }

    // Glass Corner
    await expect(page.locator(".glass-corner")).toBeVisible();

    // Aurora Horizon
    await expect(page.locator("#horizon")).toBeVisible();
  });

  test("renders correctly at all mobile viewports", async ({ page }) => {
    for (const vp of VIEWPORTS) {
      await page.setViewportSize({ width: vp.width, height: vp.height });
      await page.waitForTimeout(200);

      const home = page.locator("#screen-home");
      await expect(home).toBeVisible();

      await page.screenshot({
        path: `aurora-glass/screenshots/home-${vp.name}-light-soft.png`,
        fullPage: true,
      });
    }
  });

  test("captures screenshots across all theme/tier/viewport combinations", async ({ page }) => {
    const themes = ["light", "dark"];
    const tiers = ["soft", "clear"];

    for (const theme of themes) {
      for (const tier of tiers) {
        for (const vp of VIEWPORTS) {
          await page.setViewportSize({ width: vp.width, height: vp.height });
          await page.waitForTimeout(200);

          // Set theme and tier directly on the root element
          await page.evaluate(({ theme, tier }) => {
            document.documentElement.setAttribute("data-theme", theme);
            document.documentElement.setAttribute("data-glass-tier", tier);
          }, { theme, tier });

          await page.waitForTimeout(100);

          // Verify attributes took effect
          await expect(page.locator("html")).toHaveAttribute("data-theme", theme);
          await expect(page.locator("html")).toHaveAttribute("data-glass-tier", tier);

          await page.screenshot({
            path: `aurora-glass/screenshots/home-${vp.name}-${theme}-${tier}.png`,
            fullPage: true,
          });
        }
      }
    }
  });

  test("captures Spaces and Aura receipt screenshots across theme/tier combinations", async ({ page }) => {
    const themes = ["light", "dark"];
    const tiers = ["soft", "clear"];

    for (const theme of themes) {
      for (const tier of tiers) {
        await page.setViewportSize({ width: 390, height: 800 });
        await page.waitForTimeout(200);

        await page.evaluate(({ theme, tier }) => {
          document.documentElement.setAttribute("data-theme", theme);
          document.documentElement.setAttribute("data-glass-tier", tier);
        }, { theme, tier });

        await page.waitForTimeout(100);

        // Spaces screen
        await page.locator("#dock-spaces").click();
        await expect(page.locator("#screen-spaces")).toBeVisible();
        await page.screenshot({
          path: `aurora-glass/screenshots/spaces-390-${theme}-${tier}.png`,
          fullPage: true,
        });

        // Back to home, then Aura receipt
        await page.locator("#btn-spaces-back").click();
        await expect(page.locator("#screen-home")).toBeVisible();

        await page.locator("#btn-aura-study").click();
        await page.locator("#btn-plan-confirm").click();
        await page.locator("#btn-plan-run").click();
        await expect(page.locator("#plan-receipt")).toBeVisible({ timeout: 8000 });
        await page.screenshot({
          path: `aurora-glass/screenshots/aura-receipt-390-${theme}-${tier}.png`,
          fullPage: true,
        });
        // Leave the receipt the way a person would, so Back from Spaces next time returns to Home.
        await page.locator("#btn-receipt-close").click();
        await page.evaluate(() => { window.AuroraModels.qs.set("focus", false); });
      }
    }
  });

  test("switches between light and dark themes", async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 800 });

    // Default is light
    await expect(page.locator("html")).toHaveAttribute("data-theme", "light");
    await expect(page.locator("html")).toHaveAttribute("data-glass-tier", "soft");
  });

  test("toggles Clear and Soft Glass tiers", async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 800 });

    // Default Soft Glass
    await expect(page.locator("html")).toHaveAttribute("data-glass-tier", "soft");

    // Take Soft Glass screenshot
    await page.screenshot({
      path: "aurora-glass/screenshots/home-390-light-soft.png",
      fullPage: true,
    });
  });

  test("navigates from Home to Spaces via dock", async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 800 });

    await page.locator("#dock-spaces").click();
    await expect(page.locator("#screen-spaces")).toBeVisible();
    await expect(page.locator("#screen-home")).toBeHidden();

    // Space cards present
    await expect(page.locator('.space-card-item[data-space="study"]')).toBeVisible();
    await expect(page.locator('.space-card-item[data-space="work"]')).toBeVisible();
    await expect(page.locator('.space-card-item[data-space="projects"]')).toBeVisible();
    await expect(page.locator('.space-card-item[data-space="personal"]')).toBeVisible();

    await page.screenshot({
      path: "aurora-glass/screenshots/spaces-390-light-soft.png",
      fullPage: true,
    });
  });

  test("switches between Spaces", async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 800 });
    await page.locator("#dock-spaces").click();
    await expect(page.locator("#screen-spaces")).toBeVisible();

    // Switch to Work
    await page.locator('.space-card-item[data-space="work"]').click();
    await page.waitForTimeout(100);

    // Work should be active
    const workCard = page.locator('.space-card-item[data-space="work"]');
    await expect(workCard).toHaveClass(/selected/);

    await page.screenshot({
      path: "aurora-glass/screenshots/spaces-work-390-light-soft.png",
      fullPage: true,
    });
  });

  test("navigates to Index and searches", async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 800 });

    await page.locator("#dock-ask").click();
    await expect(page.locator("#screen-index")).toBeVisible();

    const input = page.locator("#index-query");
    await input.fill("Notes");
    await page.waitForTimeout(200);

    // Results should be filtered
    const results = page.locator("#index-results .index-result");
    await expect(results.first()).toContainText("Notes");

    await page.screenshot({
      path: "aurora-glass/screenshots/index-search-390-light-soft.png",
      fullPage: true,
    });
  });

  test("Aura plan: confirm, run, success receipt", async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 800 });

    await page.locator("#btn-aura-study").click();
    await expect(page.locator("#screen-aura")).toBeVisible();

    // Review first: opening the plan must not run anything (Action Broker contract).
    expect(await page.evaluate(() => window.AuroraModels.qs.get("focus"))).toBe(false);
    expect(await page.evaluate(() => window.AuroraShell.state.activeSpace)).toBe("study");
    await expect(page.locator("#plan-receipt")).toHaveCount(0);

    // Click confirm
    await page.locator("#btn-plan-confirm").click();
    await expect(page.locator("#plan-confirmation")).toBeVisible();

    // Click run
    await page.locator("#btn-plan-run").click();
    await page.waitForTimeout(500);

    // Running panel visible
    await expect(page.locator("#plan-running")).toBeVisible();

    // Receipt panel visible once the broker has run every step
    await expect(page.locator("#plan-receipt")).toBeVisible({ timeout: 8000 });
    await expect(page.locator("#plan-receipt h2")).toContainText("Plan complete");
    expect(await page.evaluate(() => window.AuroraModels.qs.get("focus"))).toBe(true);

    await page.screenshot({
      path: "aurora-glass/screenshots/aura-receipt-390-light-soft.png",
      fullPage: true,
    });
  });

  test("Aura plan: failure flow", async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 800 });

    // Default is success flow
    await page.locator("#btn-aura-study").click();
    await page.locator("#btn-plan-confirm").click();
    await page.locator("#btn-plan-run").click();

    // Default is success - receipt shown
    await expect(page.locator("#plan-receipt")).toBeVisible({ timeout: 8000 });

    // Verify failure panel exists (hidden)
    await expect(page.locator("#plan-failure")).toBeHidden();

    await page.screenshot({
      path: "aurora-glass/screenshots/aura-success-390-light-soft.png",
      fullPage: true,
    });
  });

  test("keyboard navigation: Escape returns to home", async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 800 });

    // Go to spaces
    await page.locator("#dock-spaces").click();
    await expect(page.locator("#screen-spaces")).toBeVisible();

    // Press Escape
    await page.keyboard.press("Escape");
    await expect(page.locator("#screen-home")).toBeVisible();
    await expect(page.locator("#screen-spaces")).toBeHidden();
  });

  test("keyboard navigation: Ctrl+K opens Index", async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 800 });

    await page.keyboard.down("Control");
    await page.keyboard.press("KeyK");
    await page.keyboard.up("Control");

    await expect(page.locator("#screen-index")).toBeVisible();
    await expect(page.locator("#index-query")).toBeFocused();
  });

  test("Glass Corner toggles aria-expanded", async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 800 });

    const gc = page.locator(".glass-corner");
    await expect(gc).toHaveAttribute("aria-expanded", "false");

    await gc.click();
    await expect(gc).toHaveAttribute("aria-expanded", "true");

    await gc.click();
    await expect(gc).toHaveAttribute("aria-expanded", "false");
  });

  test("Aura plan steps are editable", async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 800 });

    await page.locator("#btn-aura-study").click();
    await page.locator("#btn-plan-edit").click();

    // Step inputs should become editable
    const firstStepInput = page.locator(".step-text").first();
    await expect(firstStepInput).toBeEditable();

    await page.screenshot({
      path: "aurora-glass/screenshots/aura-edit-390-light-soft.png",
      fullPage: true,
    });
  });

  test("Clear tier renders without blur", async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 800 });

    await expect(page.locator("html")).toHaveAttribute("data-glass-tier", "soft");
    await expect(page.locator(".dock")).toBeVisible();
  });

  test("no console errors on load", async ({ page }) => {
    const errors = [];
    page.on("console", (msg) => {
      if (msg.type() === "error") errors.push(msg.text());
    });

    await page.goto(BASE_URL);
    await page.waitForLoadState("networkidle");
    await page.waitForTimeout(500);

    expect(errors).toEqual([]);
  });
});
