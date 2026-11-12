/**
 * tests/ui-golden/glass-corner.test.js — M1 / S7: Glass Corner (held items) and the Horizon status source.
 * Everything in the popover is read from AuroraSystem (broker receipts and the permission log); nothing is invented.
 */
const { test, expect } = require("@playwright/test");

const BASE_URL = "http://localhost:3000";
const ev = (page, fn, arg) => page.evaluate(fn, arg);
const gc = (page) => page.locator("#glass-corner");
const panel = (page) => page.locator("#gc-panel");

async function load(page) { await page.goto(BASE_URL); await ev(page, () => { window.AuroraSystem.dev.stepDelayMs = 0; }); }
const timer = (page, s) => ev(page, (sec) => window.AuroraSystem.request({ type: "timer.start", arguments: { seconds: sec } }, { source: "test" }), s);

test.describe("Glass Corner: handle and popover", () => {
  test.beforeEach(async ({ page }) => { await load(page); });

  test("is a real button with disclosure semantics", async ({ page }) => {
    expect(await gc(page).evaluate((n) => n.tagName)).toBe("BUTTON");
    await expect(gc(page)).toHaveAttribute("aria-haspopup", "true");
    await expect(gc(page)).toHaveAttribute("aria-controls", "gc-panel");
    await expect(gc(page)).toHaveAttribute("aria-expanded", "false");
    await expect(page.getByRole("button", { name: "Glass Corner" })).toBeVisible();
    await expect(panel(page)).toBeHidden();
  });

  test("opens a labelled region, moves focus into it, and closes on a second press with focus back on the handle", async ({ page }) => {
    await gc(page).click();
    await expect(gc(page)).toHaveAttribute("aria-expanded", "true");
    await expect(panel(page)).toBeVisible();
    await expect(page.getByRole("region", { name: "Held for you" })).toBeVisible();
    await expect(page.locator("#gc-title")).toBeFocused();
    await gc(page).click();
    await expect(panel(page)).toBeHidden();
    await expect(gc(page)).toHaveAttribute("aria-expanded", "false");
    await expect(gc(page)).toBeFocused();
  });

  test("a fresh session holds nothing: empty state, no badge", async ({ page }) => {
    await expect(page.locator("#gc-badge")).toBeHidden();
    await gc(page).click();
    await expect(page.locator("#gc-empty")).toHaveText("Nothing held");
    await expect(panel(page).locator(".gc-item")).toHaveCount(0);
  });

  test("Open Trail is enabled and opens the Trail screen (no 'later milestone' note)", async ({ page }) => {
    await gc(page).click();
    await expect(page.locator("#btn-gc-trail")).toBeEnabled();
    await expect(page.locator("#gc-trail-note")).toHaveCount(0);
    await page.locator("#btn-gc-trail").click();
    await expect(page.locator("#screen-trail h1")).toHaveText("Trail");
  });
});

test.describe("Glass Corner: held items come from AuroraSystem", () => {
  test.beforeEach(async ({ page }) => { await load(page); });

  test("a finished action raises the badge; opening lists the real receipt and clears the badge", async ({ page }) => {
    await timer(page, 600);
    await expect(page.locator("#gc-badge")).toHaveText("1");
    await expect(gc(page)).toHaveAccessibleName("Glass Corner, 1 new");
    await gc(page).click();
    const item = panel(page).locator(".gc-item", { hasText: "Start a timer for 10 minutes" });
    await expect(item).toBeVisible();
    await expect(item.locator(".chip")).toHaveText("Done");
    await expect(item).toContainText("just now");
    await expect(page.locator("#gc-badge")).toBeHidden();
    await expect(gc(page)).toHaveAccessibleName("Glass Corner");
  });

  test("the badge counts receipts and permission changes made since the last open", async ({ page }) => {
    await timer(page, 60);
    await ev(page, () => window.AuroraSystem.permissions.set("files.edit", "allow"));
    await expect(page.locator("#gc-badge")).toHaveText("2");
    await gc(page).click(); await gc(page).click();
    await expect(page.locator("#gc-badge")).toBeHidden();
    await timer(page, 90);
    await expect(page.locator("#gc-badge")).toHaveText("1");
  });

  test("lists permission changes with from and to", async ({ page }) => {
    await ev(page, () => window.AuroraSystem.permissions.set("files.edit", "allow"));
    await gc(page).click();
    await expect(panel(page).locator("[data-permission]")).toContainText("Move and edit files: Ask → Allow");
    await expect(panel(page).getByRole("list", { name: "Permission changes" })).toBeVisible();
  });

  test("shows at most 5 receipts and 3 permission changes, newest first", async ({ page }) => {
    for (let i = 1; i <= 7; i++) await timer(page, i * 60);
    await ev(page, () => { const P = window.AuroraSystem.permissions; ["allow", "deny", "ask", "allow", "deny"].forEach((d) => P.set("settings.write", d)); });
    await gc(page).click();
    await expect(panel(page).locator("[data-receipt]")).toHaveCount(5);
    await expect(panel(page).locator("[data-receipt]").first()).toContainText("7 minutes");
    await expect(panel(page).locator("[data-receipt]").last()).toContainText("3 minutes");
    await expect(panel(page).locator("[data-permission]")).toHaveCount(3);
    await expect(panel(page).locator("[data-permission]").first()).toContainText("Allow → Deny");   // the last change made (ask→allow, allow→deny, deny→ask, ask→allow, allow→deny)
  });

  test("shows an undone action as Undone and a failed one as Failed", async ({ page }) => {
    await timer(page, 120);
    await ev(page, async () => { await window.AuroraSystem.undo(window.AuroraSystem.receipts()[0].id); });
    await ev(page, () => { window.AuroraSystem.dev.failNextType = "timer.start"; });
    await timer(page, 180);
    await gc(page).click();
    await expect(panel(page).locator("[data-receipt]").nth(0).locator(".chip")).toHaveText("Failed");
    await expect(panel(page).locator("[data-receipt]").nth(1).locator(".chip")).toHaveText("Undone");
  });

  test("updates live while open and keeps focus where it was", async ({ page }) => {
    await gc(page).click();
    await expect(page.locator("#gc-empty")).toBeVisible();
    await timer(page, 300);
    await expect(panel(page).locator("[data-receipt]")).toHaveCount(1);
    await expect(page.locator("#gc-empty")).toHaveCount(0);
    await expect(page.locator("#gc-title")).toBeFocused();
    await expect(page.locator("#gc-badge")).toBeHidden();       // open means seen
  });

  test("titles are plain text", async ({ page }) => {
    await ev(page, () => window.AuroraSystem.request({ type: "search.query", arguments: { query: "<img src=x onerror=1>" } }, { source: "test" }));
    await gc(page).click();
    await expect(panel(page)).toContainText("<img src=x onerror=1>");
    await expect(panel(page).locator("img")).toHaveCount(0);
  });

  test("a held item raised by an Aura plan appears after the plan runs", async ({ page }) => {
    await ev(page, () => { window.AuroraSystem.dev.stepDelayMs = 20; });
    await page.locator("#btn-aura-study").click();
    await page.locator("#btn-plan-confirm").click();
    await page.locator("#btn-plan-run").click();
    await expect(page.locator("#plan-receipt")).toBeVisible({ timeout: 8000 });
    await expect(page.locator("#gc-badge")).toHaveText("4");
    await gc(page).click();
    await expect(panel(page).locator("[data-receipt]")).toHaveCount(4);
    await expect(panel(page)).toContainText("Switch to Study Space");
  });
});

test.describe("Glass Corner: dismissal", () => {
  test.beforeEach(async ({ page }) => { await load(page); });

  test("Escape closes it first (staying on the screen) and returns focus to the handle", async ({ page }) => {
    await page.locator("#dock-pages").click();
    await gc(page).click();
    await page.keyboard.press("Escape");
    await expect(panel(page)).toBeHidden();
    await expect(gc(page)).toBeFocused();
    await expect(page.locator("#screen-pages")).toBeVisible();
    await page.keyboard.press("Escape");
    await expect(page.locator("#screen-home")).toBeVisible();
  });

  test("an outside click closes it without stealing focus from what was clicked", async ({ page }) => {
    await gc(page).click();
    await page.locator("#dock-spaces").click();
    await expect(panel(page)).toBeHidden();
    await expect(page.locator("#screen-spaces")).toBeVisible();
    await expect(gc(page)).not.toBeFocused();                     // focus went to the new screen, not back to the handle
    await expect(page.locator("#screen-spaces h1")).toBeFocused();
  });

  test("a click inside the popover does not close it", async ({ page }) => {
    await timer(page, 60);
    await gc(page).click();
    await panel(page).locator(".gc-item").first().click();
    await expect(panel(page)).toBeVisible();
  });

  test("a dialog's Escape closes the dialog, not the popover underneath", async ({ page }) => {
    await gc(page).click();
    await ev(page, () => { window.AuroraUI.dialog({ title: "Test", body: "x", actions: [{ label: "Close", value: 1 }] }); });
    await page.keyboard.press("Escape");
    await expect(page.locator(".dialog")).toHaveCount(0);
    await expect(panel(page)).toBeVisible();
  });

  test("turning the Glass Corner off in Settings hides the handle and closes the popover", async ({ page }) => {
    await gc(page).click();
    await ev(page, () => window.AuroraSettingsModel.set("notifications.glassCorner", false));
    await expect(gc(page)).toBeHidden();
    await expect(panel(page)).toBeHidden();
    await ev(page, () => window.AuroraSettingsModel.set("notifications.glassCorner", true));
    await expect(gc(page)).toBeVisible();
    await expect(panel(page)).toBeHidden();
  });
});

test.describe("Glass Corner: appearance and layout", () => {
  for (const [w, h] of [[360, 740], [390, 844], [768, 1024], [1280, 800]]) {
    for (const mode of ["default", "200%"]) {
      test(`stays inside the viewport and scrolls when long at ${w}px (${mode} text)`, async ({ page }) => {
        await page.setViewportSize({ width: w, height: h });
        await load(page);
        for (let i = 1; i <= 7; i++) await timer(page, i * 60);
        await ev(page, () => { const P = window.AuroraSystem.permissions; ["allow", "deny", "ask"].forEach((d) => P.set("settings.write", d)); });
        if (mode === "200%") await ev(page, () => document.documentElement.setAttribute("data-text-scale", "200"));
        await gc(page).click();
        const r = await ev(page, () => {
          const p = document.getElementById("gc-panel").getBoundingClientRect(), vw = document.documentElement.clientWidth, vh = innerHeight;
          return { left: p.left, right: p.right, top: p.top, h: p.height, vw, vh, sw: document.documentElement.scrollWidth, scrolls: document.getElementById("gc-panel").scrollHeight > document.getElementById("gc-panel").clientHeight };
        });
        expect(r.left).toBeGreaterThanOrEqual(0);
        expect(r.right).toBeLessThanOrEqual(r.vw + 1);
        expect(r.h).toBeLessThanOrEqual(r.vh * 0.7 + 1);
        expect(r.sw).toBeLessThanOrEqual(r.vw);
        if (mode === "200%") expect(r.scrolls).toBe(true);
        const trail = await page.locator("#btn-gc-trail").boundingBox();
        expect(trail).not.toBeNull();
      });
    }
  }

  test("the popover and handle are opaque in Clear and high contrast", async ({ page }) => {
    await load(page);
    await ev(page, () => { window.AuroraShell.setTier("clear"); window.AuroraShell.setContrast("high"); });
    await gc(page).click();
    const alpha = (c) => { const m = c.match(/rgba?\(([^)]+)\)/)[1].split(",").map((x) => parseFloat(x)); return m.length === 4 ? m[3] : 1; };
    expect(alpha(await panel(page).evaluate((n) => getComputedStyle(n).backgroundColor))).toBe(1);
    expect(alpha(await gc(page).evaluate((n) => getComputedStyle(n).backgroundColor))).toBe(1);
    expect(await panel(page).evaluate((n) => getComputedStyle(n).backdropFilter)).toBe("none");
  });

  test("renders in dark soft without console errors, and the badge is not colour-only (a number)", async ({ page }) => {
    const errors = []; page.on("pageerror", (e) => errors.push(e.message)); page.on("console", (m) => { if (m.type() === "error") errors.push(m.text()); });
    await load(page);
    await ev(page, () => { window.AuroraShell.setAppearance("dark"); });
    await timer(page, 60);
    await expect(page.locator("#gc-badge")).toHaveText(/^\d/);
    await gc(page).click();
    expect(errors).toEqual([]);
  });
});

test.describe("Horizon: one status source", () => {
  test("the Home rail and the Dock line are driven by the same status event", async ({ page }) => {
    await load(page);
    await ev(page, () => window.AuroraSystem.emit("status", { text: "Running 4 steps", running: true, done: 1, total: 4 }));
    await expect(page.locator("#horizon-meter")).toHaveAttribute("aria-valuenow", "25");
    expect(await page.locator("#dock-horizon-fill").evaluate((n) => n.style.width)).toBe("25%");
    await ev(page, () => window.AuroraSystem.emit("status", { text: "Running 4 steps", running: true, done: 3, total: 4 }));
    await expect(page.locator("#horizon-meter")).toHaveAttribute("aria-valuenow", "75");
    expect(await page.locator("#dock-horizon-fill").evaluate((n) => n.style.width)).toBe("75%");
    expect(await page.locator('[id*="horizon"]').evaluateAll((n) => n.map((e) => e.id).filter((i) => /count|counter/.test(i)))).toEqual([]);
  });

  test("ignores malformed status events", async ({ page }) => {
    await load(page);
    await ev(page, () => { const S = window.AuroraSystem; S.emit("status", null); S.emit("status", { text: 5 }); S.emit("status", {}); });
    await expect(page.locator("#horizon-text")).toHaveText("Ready");
  });

  test("returns to Ready after an action finishes", async ({ page }) => {
    await page.clock.install({ time: new Date("2026-11-12T10:00:00") });
    await page.goto(BASE_URL);
    await ev(page, () => window.AuroraSystem.emit("status", { text: "Plan complete · 4 steps", running: false, done: 4, total: 4 }));
    await expect(page.locator("#horizon-text")).toHaveText("Plan complete · 4 steps");
    await page.clock.runFor(13000);
    await expect(page.locator("#horizon-text")).toHaveText("Ready");
  });
});
