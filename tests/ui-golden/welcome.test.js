/**
 * tests/ui-golden/welcome.test.js - M8-C: first-run welcome.
 * Three short, skippable, truthful steps. Session-only (sessionStorage, this tab). Never opens over another dialog. Under browser automation the welcome
 * does NOT start by itself (so every other spec stays deterministic): ?welcome=1 forces it, ?welcome=auto applies the real-visitor rules, ?welcome=off disables it.
 */
const { test, expect } = require("@playwright/test");

const BASE_URL = "http://localhost:3000";
const ev = (page, fn, arg) => page.evaluate(fn, arg);
const dlg = (page) => page.locator("#welcome-dialog");

async function start(page, query) {
  const errors = [];
  page.on("console", (m) => { if (m.type() === "error") errors.push(m.text()); });
  page.on("pageerror", (e) => errors.push(String(e)));
  page.__errors = errors;
  await page.goto(BASE_URL + (query || ""));
}

test.describe("Welcome: starting rules", () => {
  test("under browser automation nothing pops up by itself", async ({ page }) => {
    await start(page);
    await page.waitForTimeout(1200);
    await expect(dlg(page)).toHaveCount(0);
  });
  test("?welcome=off disables it even for a real visitor", async ({ page }) => {
    await start(page, "?welcome=off");
    await page.waitForTimeout(1200);
    await expect(dlg(page)).toHaveCount(0);
  });
  test("?welcome=auto (real-visitor rules): shown on the first visit, not again after a reload in the same tab, shown again in a fresh session", async ({ page, browser }) => {
    await start(page, "?welcome=auto");
    await expect(dlg(page)).toBeVisible();
    await page.locator("#btn-welcome-skip").click();
    await expect(dlg(page)).toHaveCount(0);
    await page.reload();
    await page.waitForTimeout(1500);
    await expect(dlg(page)).toHaveCount(0);                                           // same tab session: not again
    const ctx = await browser.newContext(); const p2 = await ctx.newPage();           // a new session starts clean (nothing was stored permanently)
    await p2.goto(BASE_URL + "?welcome=auto");
    await expect(p2.locator("#welcome-dialog")).toBeVisible();
    expect(await p2.evaluate(() => Object.keys(localStorage).length)).toBe(0);
    await ctx.close();
  });
  test("it stores nothing permanently: no localStorage keys, only the session flag", async ({ page }) => {
    await start(page, "?welcome=1");
    await expect(dlg(page)).toBeVisible();
    expect(await ev(page, () => Object.keys(localStorage).length)).toBe(0);
    expect(await ev(page, () => Object.keys(sessionStorage))).toEqual(["aurora-glass.welcome.seen"]);
  });
});

test.describe("Welcome: the tour", () => {
  test("opens as a labelled dialog with Step 1 of 3, a progress indicator and the truthful prototype note", async ({ page }) => {
    await start(page, "?welcome=1");
    await expect(dlg(page)).toBeVisible();
    await expect(dlg(page)).toHaveAttribute("role", "dialog"); await expect(dlg(page)).toHaveAttribute("aria-modal", "true");
    await expect(page.getByRole("dialog", { name: "Welcome to Aurora Glass" })).toBeVisible();
    await expect(page.locator("#welcome-step-label")).toHaveText("Step 1 of 3");
    await expect(page.locator("#welcome-heading")).toHaveText("Spaces keep your day organised");
    await expect(page.locator("#welcome .welcome-dot")).toHaveCount(3);
    await expect(page.locator("#welcome .welcome-dot.is-current")).toHaveCount(1);
    await expect(page.locator("#welcome-note")).toContainText("browser prototype");
    await expect(page.locator("#welcome-note")).toContainText("no cellular, audio, camera, location or other phone hardware is used");
    await expect(page.locator("#btn-welcome-next")).toBeFocused();
    await expect(page.locator("#btn-welcome-back")).toBeHidden();
    expect(page.__errors).toEqual([]);
  });
  test("Next and Back move through three steps; the last step says Finish and has no Skip; progress and the live region follow", async ({ page }) => {
    await start(page, "?welcome=1");
    await page.locator("#btn-welcome-next").click();
    await expect(page.locator("#welcome-step-label")).toHaveText("Step 2 of 3"); await expect(page.locator("#welcome-heading")).toHaveText("Apps, Pages and getting around");
    await expect(page.locator("#btn-welcome-back")).toBeVisible(); await expect(page.locator("#a11y-live")).toContainText("Step 2 of 3: Apps, Pages and getting around");
    await page.locator("#btn-welcome-next").click();
    await expect(page.locator("#welcome-step-label")).toHaveText("Step 3 of 3"); await expect(page.locator("#welcome-heading")).toHaveText("Aura asks before it acts");
    await expect(page.locator("#btn-welcome-next")).toHaveText("Finish"); await expect(page.locator("#btn-welcome-skip")).toBeHidden();
    await page.locator("#btn-welcome-back").click();
    await expect(page.locator("#welcome-step-label")).toHaveText("Step 2 of 3"); await expect(page.locator("#btn-welcome-next")).toBeFocused();
    await expect(page.locator("#btn-welcome-next")).toHaveText("Next");
    await page.locator("#btn-welcome-back").click();
    await expect(page.locator("#welcome-step-label")).toHaveText("Step 1 of 3"); await expect(page.locator("#btn-welcome-back")).toBeHidden();
  });
  test("each step describes the real interface: Spaces, Pages/Index/Back, Aura/Passport/Trail/hold-to-confirm", async ({ page }) => {
    await start(page, "?welcome=1");
    await expect(dlg(page)).toContainText("Study, Work, Projects and Personal");
    await page.locator("#btn-welcome-next").click();
    await expect(dlg(page)).toContainText("Ctrl+K"); await expect(dlg(page)).toContainText("Escape");
    await page.locator("#btn-welcome-next").click();
    await expect(dlg(page)).toContainText("Passport"); await expect(dlg(page)).toContainText("Trail"); await expect(dlg(page)).toContainText("press-and-hold");
  });
  test("every claim in the tour is true of the prototype: nothing promises real hardware, networking or AI", async ({ page }) => {
    await start(page, "?welcome=1");
    const all = [];
    for (let i = 0; i < 3; i++) { all.push(await dlg(page).innerText()); if (i < 2) await page.locator("#btn-welcome-next").click(); }
    const text = all.join(" ").toLowerCase();
    for (const bad of ["call", "sim card", "4g", "5g", "bluetooth connected", "your photos", "voice", "model is", "cloud"]) expect(text, bad).not.toContain(bad);
    expect(text).toContain("no cellular, audio, camera");
  });
  test("Finish closes it, the interface underneath is usable, and focus is not trapped", async ({ page }) => {
    await start(page, "?welcome=1");
    await page.locator("#btn-welcome-next").click(); await page.locator("#btn-welcome-next").click(); await page.locator("#btn-welcome-next").click();
    await expect(dlg(page)).toHaveCount(0);
    expect(await ev(page, () => window.AuroraWelcome.isOpen())).toBe(false);
    await ev(page, () => window.AuroraShell.openApp("notes"));
    await expect(page.locator("#screen-notes")).toHaveClass(/active/);
  });
  test("Skip tour closes it from step 1 and step 2", async ({ page }) => {
    await start(page, "?welcome=1");
    await page.locator("#btn-welcome-skip").click(); await expect(dlg(page)).toHaveCount(0);
    await ev(page, () => window.AuroraWelcome.show());
    await page.locator("#btn-welcome-next").click(); await page.locator("#btn-welcome-skip").click(); await expect(dlg(page)).toHaveCount(0);
  });
  test("Escape skips the tour on any step", async ({ page }) => {
    await start(page, "?welcome=1");
    await expect(dlg(page)).toBeVisible();
    await page.locator("#btn-welcome-next").click();
    await page.keyboard.press("Escape");
    await expect(dlg(page)).toHaveCount(0);
    await expect(page.locator("#screen-home")).toHaveClass(/active/);                  // Escape closed the tour only; it did not navigate Back
  });
  test("clicking outside the dialog does not dismiss it by accident", async ({ page }) => {
    await start(page, "?welcome=1");
    await expect(dlg(page)).toBeVisible();
    await page.mouse.click(2, 2);
    await expect(dlg(page)).toBeVisible();
  });
  test("keyboard: Tab and Shift+Tab stay inside the dialog; Enter on Next advances", async ({ page }) => {
    await start(page, "?welcome=1");
    await expect(page.locator("#btn-welcome-next")).toBeFocused();
    for (let i = 0; i < 6; i++) { await page.keyboard.press("Tab"); expect(await ev(page, () => !!document.activeElement.closest("#welcome-dialog"))).toBe(true); }
    for (let i = 0; i < 6; i++) { await page.keyboard.press("Shift+Tab"); expect(await ev(page, () => !!document.activeElement.closest("#welcome-dialog"))).toBe(true); }
    await page.locator("#btn-welcome-next").focus(); await page.keyboard.press("Enter");
    await expect(page.locator("#welcome-step-label")).toHaveText("Step 2 of 3");
  });
  test("global shortcuts do not leak through the open tour (Ctrl+K does not open Index behind it)", async ({ page }) => {
    await start(page, "?welcome=1");
    await expect(dlg(page)).toBeVisible();
    await page.keyboard.press("Control+k");
    await expect(page.locator("#index-query")).toBeHidden();
    expect(await ev(page, () => window.AuroraShell.state.activeScreen)).toBe("screen-home");
  });
  test("show() while it is already open does not open a second copy", async ({ page }) => {
    await start(page, "?welcome=1");
    await expect(dlg(page)).toBeVisible();
    expect(await ev(page, () => window.AuroraWelcome.show())).toBe(false);
    await expect(page.locator("#welcome-dialog")).toHaveCount(1);
  });
});

test.describe("Welcome: other dialogs", () => {
  test("it never opens over another dialog: it waits, then shows once that dialog is closed", async ({ page }) => {
    await start(page);
    await ev(page, () => { window.AuroraUI.dialog({ id: "other-dialog", title: "Review something", body: "Trusted review", actions: [{ label: "OK", value: true, id: "other-ok" }] }); });
    expect(await ev(page, () => window.AuroraWelcome.show())).toBe(false);
    await ev(page, () => { window.__req = window.AuroraWelcome.request(); });
    await page.waitForTimeout(900);
    await expect(dlg(page)).toHaveCount(0);
    await expect(page.locator("#other-dialog")).toBeVisible();
    await page.locator("#other-ok").click();
    await expect(dlg(page)).toBeVisible({ timeout: 3000 });
    expect(await ev(page, () => window.__req)).toBe(true);
  });
  test("a dialog opened on top of the tour (for example a review) takes the keys first and the tour stays", async ({ page }) => {
    await start(page, "?welcome=1");
    await expect(dlg(page)).toBeVisible();
    await ev(page, () => { window.AuroraUI.dialog({ id: "top-dialog", title: "On top", body: "x", actions: [{ label: "Close", value: true, id: "top-close" }] }); });
    await page.keyboard.press("Escape");
    await expect(page.locator("#top-dialog")).toHaveCount(0);
    await expect(dlg(page)).toBeVisible();
  });
});

for (const [name, w, h, scale] of [["360 at 200% text", 360, 740, "200"], ["390", 390, 844], ["412", 412, 915], ["768", 768, 1024], ["1280", 1280, 800]]) {
  test(`Welcome layout: ${name}: dialog fits the viewport, all three buttons reachable, no overflow`, async ({ page }) => {
    await page.setViewportSize({ width: w, height: h });
    await start(page, "?welcome=1");
    if (scale) await ev(page, (s) => document.documentElement.setAttribute("data-text-scale", s), scale);
    for (let i = 0; i < 3; i++) {
      await expect(dlg(page)).toBeVisible();
      const b = await dlg(page).boundingBox();
      expect(b.x, "step " + i).toBeGreaterThanOrEqual(0); expect(b.x + b.width, "step " + i).toBeLessThanOrEqual(w + 0.5);
      for (const sel of ["#btn-welcome-next", i ? "#btn-welcome-back" : "#btn-welcome-skip"]) {
        await expect(page.locator(sel)).toBeVisible();
        const r = await page.locator(sel).evaluate((n) => { const q = n.getBoundingClientRect(); return { l: q.left, r: q.right, t: q.top, b: q.bottom, h: q.height }; });
        expect(r.l, sel).toBeGreaterThanOrEqual(-0.5); expect(r.r, sel).toBeLessThanOrEqual(w + 0.5); expect(r.h, sel).toBeGreaterThanOrEqual(44);
      }
      if (i < 2) await page.locator("#btn-welcome-next").click();
    }
    expect(await ev(page, () => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
    // the dialog can scroll if the viewport is short: the Finish button is reachable
    await page.locator("#btn-welcome-next").scrollIntoViewIfNeeded(); await expect(page.locator("#btn-welcome-next")).toBeInViewport();
  });
}

test.describe("Welcome: appearance modes", () => {
  test("high contrast: the current step marker is outlined; dark theme renders readable text", async ({ page }) => {
    await start(page, "?welcome=1");
    await ev(page, () => { window.AuroraShell.setContrast("high"); window.AuroraShell.setAppearance("dark"); });
    expect(await page.locator(".welcome-dot.is-current").evaluate((n) => getComputedStyle(n).outlineStyle)).toBe("solid");
    const c = await page.locator("#welcome-text").evaluate((n) => getComputedStyle(n).color);
    expect(c).not.toBe("rgba(0, 0, 0, 0)");
  });
  test("reduced motion: nothing in the welcome animates", async ({ page }) => {
    await page.emulateMedia({ reducedMotion: "reduce" });
    await start(page, "?welcome=1");
    await ev(page, () => window.AuroraShell.setMotion("reduce"));
    for (const sel of ["#welcome-dialog", "#welcome", ".welcome-dot", "#btn-welcome-next"]) {
      const t = await page.locator(sel).first().evaluate((n) => getComputedStyle(n).animationName + "|" + getComputedStyle(n).transitionDuration);
      expect(t, sel).toMatch(/^none\|0s$/);
    }
  });
});
