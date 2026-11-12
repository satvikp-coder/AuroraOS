/**
 * tests/ui-golden/shell.test.js — the AuroraShell contract (docs/agents/AURORA_ARCHITECTURE.md sections 3 and 4).
 * Navigation history, app registry and openApp results, Spaces validation, global keys.
 * Browser prototype only; nothing here touches a real device.
 */
const { test, expect } = require("@playwright/test");

const BASE_URL = "http://localhost:3000";
const shell = (page, fn, arg) => page.evaluate(fn, arg);

test.describe("Shell: module chain", () => {
  test("loads every module with no console errors", async ({ page }) => {
    const errors = [];
    page.on("console", (m) => { if (m.type() === "error") errors.push(m.text()); });
    page.on("pageerror", (e) => errors.push(e.message));
    await page.goto(BASE_URL);
    await page.waitForLoadState("networkidle");
    for (const g of ["AuroraUI", "AuroraSystem", "AuroraReview", "AuroraShell", "AuroraModels", "AuroraSettingsModel"]) {
      expect(await page.evaluate((k) => typeof window[k], g), g).toBe("object");
    }
    expect(errors).toEqual([]);
  });

  test("Aura plan is rendered by the review module, not static markup", async ({ page }) => {
    await page.goto(BASE_URL);
    await expect(page.locator("#screen-aura")).toBeEmpty();
    await page.locator("#btn-aura-study").click();
    await expect(page.locator("#screen-aura h1")).toHaveText("Plan");
    await expect(page.locator("#screen-aura .plan-step")).toHaveCount(4);
  });
});

test.describe("Shell: navigation history", () => {
  test.beforeEach(async ({ page }) => { await page.goto(BASE_URL); });

  test("Back returns to the screen you came from, not always Home", async ({ page }) => {
    await page.locator("#btn-quick-center").click();           // Home -> Quick Center
    await page.locator("#btn-quick-settings").click();         // Quick Center -> Settings
    await expect(page.locator("#screen-settings")).toBeVisible();
    await page.locator("#btn-settings-back").click();
    await expect(page.locator("#screen-quick")).toBeVisible(); // not Home
    await page.locator("#btn-quick-back").click();
    await expect(page.locator("#screen-home")).toBeVisible();
  });

  test("Back with no history shows Home, and showing the same screen twice does not stack", async ({ page }) => {
    expect(await shell(page, () => window.AuroraShell.back())).toBe("screen-home");
    await shell(page, () => { const S = window.AuroraShell; S.showScreen("screen-pages"); S.showScreen("screen-pages"); S.showScreen("screen-spaces"); });
    expect(await shell(page, () => window.AuroraShell.back())).toBe("screen-pages");
    expect(await shell(page, () => window.AuroraShell.back())).toBe("screen-home");
  });

  test("showScreen refuses unknown ids and moves focus to the screen heading", async ({ page }) => {
    expect(await shell(page, () => window.AuroraShell.showScreen("screen-nope"))).toBeNull();
    expect(await shell(page, () => window.AuroraShell.showScreen("content"))).toBeNull(); // exists but is not a screen
    await page.locator("#dock-spaces").click();
    await expect(page.locator("#screen-spaces h1")).toBeFocused();
  });

  test("the Dock marks the current screen with aria-current", async ({ page }) => {
    await expect(page.locator("#dock-home")).toHaveAttribute("aria-current", "page");
    await page.locator("#dock-pages").click();
    await expect(page.locator("#dock-pages")).toHaveAttribute("aria-current", "page");
    await expect(page.locator("#dock-home")).not.toHaveAttribute("aria-current", "page");
  });

  test("addScreen creates a screen once and returns the same element", async ({ page }) => {
    const r = await shell(page, () => {
      const S = window.AuroraShell; const a = S.addScreen("screen-demo", "Demo"); const b = S.addScreen("screen-demo", "Other");
      return { same: a === b, label: a.getAttribute("aria-label"), parentId: a.parentElement.id, cls: a.className };
    });
    expect(r).toEqual({ same: true, label: "Demo", parentId: "content", cls: "screen" });
  });
});

test.describe("Shell: apps", () => {
  test.beforeEach(async ({ page }) => { await page.goto(BASE_URL); });

  test("registry lists the built-in apps with the specified hues and returns copies", async ({ page }) => {
    const r = await shell(page, () => {
      const S = window.AuroraShell; const list = S.apps(); list[0].name = "HACKED"; list.pop();
      return { ids: S.apps().map((a) => a.id), notes: S.app("notes"), none: S.app("nope"), settingsScreen: S.app("settings").screen, vault: S.app("vault").system };
    });
    expect(r.ids).toEqual(["notes", "files", "calculator", "clock", "settings", "calendar", "passport", "trail", "vault"]);
    expect(r.notes.name).toBe("Notes");
    expect(r.notes.hue).toBe(42);
    expect(r.none).toBeNull();
    expect(r.settingsScreen).toBe("screen-settings");
    expect(r.vault).toBe(true);
  });

  test("registerApp rejects duplicates and bad ids", async ({ page }) => {
    const r = await shell(page, () => {
      const S = window.AuroraShell;
      return [S.registerApp({ id: "notes", name: "Dup" }), S.registerApp({ id: "Bad Id", name: "x" }), S.registerApp({ id: "", name: "x" }), S.registerApp({ id: "demo", name: "Demo" })];
    });
    expect(r).toEqual([false, false, false, true]);
  });

  test("openApp: unknown app", async ({ page }) => {
    expect(await shell(page, () => window.AuroraShell.openApp("nope"))).toEqual({ ok: false, reason: "unknown-app" });
    expect(await shell(page, () => window.AuroraShell.openApp("__proto__"))).toEqual({ ok: false, reason: "unknown-app" });
  });

  test("openApp: a system surface with no screen yet says so and creates no Page", async ({ page }) => {
    const before = await shell(page, () => window.AuroraModels.pages.list().length);
    const r = await shell(page, () => window.AuroraShell.openApp("vault"));
    expect(r).toEqual({ ok: false, reason: "not-available" });
    await expect(page.locator("#toast")).toContainText("Vault arrives in a later milestone");
    expect(await shell(page, () => window.AuroraModels.pages.list().length)).toBe(before);
    await expect(page.locator("#screen-home")).toBeVisible();
  });

  test("openApp: a new app creates a Page and lands on Pages; reopening resumes without 'created'", async ({ page }) => {
    const first = await shell(page, () => window.AuroraShell.openApp("calendar"));
    expect(first).toEqual({ ok: true, id: "page-calendar", created: true });
    await expect(page.locator("#screen-pages")).toBeVisible();
    const again = await shell(page, () => window.AuroraShell.openApp("calendar"));
    expect(again).toEqual({ ok: true, id: "page-calendar", created: false });
  });

  test("openApp: an app with a screen shows it (Settings); background opens do not navigate", async ({ page }) => {
    await shell(page, () => window.AuroraShell.openApp("settings"));
    await expect(page.locator("#screen-settings")).toBeVisible();
    await shell(page, () => window.AuroraShell.showScreen("screen-home"));
    const r = await shell(page, () => window.AuroraShell.openApp("clock", { background: true }));
    expect(r.ok).toBe(true);
    await expect(page.locator("#screen-home")).toBeVisible();
  });

  test("openApp: low memory shows Pages and a toast and creates nothing", async ({ page }) => {
    await shell(page, () => window.AuroraModels.pages.open("hog", "Hog", "study", 1500));
    const n = await shell(page, () => window.AuroraModels.pages.list().length);
    const r = await shell(page, () => window.AuroraShell.openApp("calendar"));
    expect(r).toEqual({ ok: false, reason: "low-memory" });
    await expect(page.locator("#screen-pages")).toBeVisible();
    await expect(page.locator("#toast")).toContainText("Not enough simulated memory");
    expect(await shell(page, () => window.AuroraModels.pages.list().length)).toBe(n);
  });

  test("pinned tools go through openApp", async ({ page }) => {
    await page.getByRole("button", { name: "Open Calendar" }).click();
    await expect(page.locator('[data-page="page-calendar"]')).toBeVisible();
  });
});

test.describe("Shell: Spaces", () => {
  test.beforeEach(async ({ page }) => { await page.goto(BASE_URL); });

  test("the four built-in Spaces carry their pinned apps", async ({ page }) => {
    const s = await shell(page, () => window.AuroraShell.spaces().map((x) => [x.id, x.builtin, x.pinned.join(",")]));
    expect(s).toEqual([
      ["study", true, "notes,clock,calculator,settings,calendar,files,passport"],
      ["work", true, "files,notes,calendar,settings,passport"],
      ["projects", true, "files,notes,calculator,settings,passport"],
      ["personal", true, "notes,calendar,clock,settings,passport"],
    ]);
  });

  test("addSpace / renameSpace validation table", async ({ page }) => {
    const r = await shell(page, () => {
      const S = window.AuroraShell; const out = {};
      out.empty = S.addSpace("   ");
      out.long = S.addSpace("x".repeat(25));
      out.ok24 = S.addSpace("y".repeat(24)).ok;
      out.dupe = S.addSpace("STUDY");
      out.ctl = S.addSpace("bad\u0007name");
      out.bidi = S.addSpace("evil‮txt");
      out.nonString = S.addSpace(42);
      out.good = S.addSpace("  Reading  ");
      out.renameMissing = S.renameSpace("nope", "A");
      out.renameDupe = S.renameSpace("work", "study");
      out.renameSelfCase = S.renameSpace("work", "WORK");
      out.renamed = S.renameSpace("work", "Office");
      out.names = S.spaces().map((x) => x.name);
      return out;
    });
    expect(r.empty.ok).toBe(false);
    expect(r.long.reason).toMatch(/24 characters/);
    expect(r.ok24).toBe(true);
    expect(r.dupe.reason).toMatch(/already exists/);
    expect(r.ctl.ok).toBe(false);
    expect(r.bidi.ok).toBe(false);
    expect(r.nonString.ok).toBe(false);
    expect(r.good.ok).toBe(true);
    expect(r.renameMissing.ok).toBe(false);
    expect(r.renameDupe.reason).toMatch(/already exists/);
    expect(r.renameSelfCase.ok).toBe(true);   // changing only the case of your own name is allowed
    expect(r.renamed.ok).toBe(true);
    expect(r.names).toContain("Reading");
    expect(r.names).toContain("Office");
  });

  test("at most 8 Spaces", async ({ page }) => {
    const r = await shell(page, () => {
      const S = window.AuroraShell; const res = [];
      for (let i = 1; i <= 6; i++) res.push(S.addSpace("Extra " + i).ok);
      return { res, ninth: S.addSpace("Ninth"), count: S.spaces().length };
    });
    expect(r.res.slice(0, 4)).toEqual([true, true, true, true]);
    expect(r.count).toBe(8);
    expect(r.ninth.ok).toBe(false);
    expect(r.ninth.reason).toMatch(/8 Spaces/);
  });

  test("a Space named like markup renders as text and creates no element", async ({ page }) => {
    const NAME = "<img src=x onerror=1>";   // 21 characters: valid length, hostile content
    expect(await shell(page, (n) => window.AuroraShell.addSpace(n).ok, NAME)).toBe(true);
    await page.locator("#dock-spaces").click();
    await expect(page.locator("#screen-spaces img")).toHaveCount(0);
    await expect(page.locator("#screen-spaces")).toContainText(NAME);
  });

  test("switchSpace: unknown id refused; quiet switch does not toast; listeners fire", async ({ page }) => {
    const r = await shell(page, () => {
      const S = window.AuroraShell; let heard = null; S.onSpaceChange((id) => { heard = id; });
      return { bad: S.switchSpace("nope"), q: S.switchSpace("work", { quiet: true }), heard, active: S.state.activeSpace };
    });
    expect(r.bad).toEqual({ ok: false, reason: "unknown-space" });
    expect(r.q.ok).toBe(true);
    expect(r.heard).toBe("work");
    expect(r.active).toBe("work");
    await expect(page.locator("#toast")).not.toHaveClass(/show/);
  });
});

test.describe("Shell: global keys and Index", () => {
  test.beforeEach(async ({ page }) => { await page.goto(BASE_URL); });

  test("Ctrl+K opens Index with the query field focused", async ({ page }) => {
    await page.keyboard.press("Control+k");
    await expect(page.locator("#screen-index")).toBeVisible();
    await expect(page.locator("#index-query")).toBeFocused();
  });

  test("openIndex(query) sets and renders the query", async ({ page }) => {
    await shell(page, () => window.AuroraShell.openIndex("clock"));
    await expect(page.locator("#index-query")).toHaveValue("clock");
    await expect(page.locator("#index-results")).toContainText("Clock");
  });

  test("Escape closes the Glass Corner first, then goes Back", async ({ page }) => {
    await page.locator("#dock-pages").click();
    const gc = page.locator(".glass-corner");
    await gc.click();
    await expect(gc).toHaveAttribute("aria-expanded", "true");
    await page.keyboard.press("Escape");
    await expect(gc).toHaveAttribute("aria-expanded", "false");
    await expect(page.locator("#screen-pages")).toBeVisible();   // still on Pages
    await page.keyboard.press("Escape");
    await expect(page.locator("#screen-home")).toBeVisible();
  });

  test("Escape closes a dialog without also navigating Back", async ({ page }) => {
    await page.locator("#btn-quick-center").click();
    await page.evaluate(() => { window.__d = window.AuroraUI.dialog({ title: "Test dialog", body: "Hello", actions: [{ label: "Close", value: 1 }] }); });
    await expect(page.locator(".dialog")).toBeVisible();
    await page.keyboard.press("Escape");
    await expect(page.locator(".dialog")).toHaveCount(0);
    await expect(page.locator("#screen-quick")).toBeVisible();
  });

  test("Appearance setters validate their input and reduce-motion sets data-motion", async ({ page }) => {
    await shell(page, () => { const S = window.AuroraShell; S.setAppearance("neon"); S.setTier("weird"); S.setContrast("x"); S.setMotion("reduce"); });
    const a = await page.evaluate(() => { const d = document.documentElement; return [d.dataset.theme, d.dataset.glassTier, d.dataset.contrast, d.dataset.motion]; });
    expect(a).toEqual(["light", "soft", "normal", "reduce"]);
    await shell(page, () => window.AuroraShell.setMotion("normal"));
    expect(await page.evaluate(() => document.documentElement.hasAttribute("data-motion"))).toBe(false);
  });
});
