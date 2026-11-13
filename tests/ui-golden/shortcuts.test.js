/**
 * tests/ui-golden/shortcuts.test.js - M8-D: keyboard shortcut reference.
 * The sheet lists ONLY shortcuts the prototype really has. This file (1) checks when ? may open it, (2) checks the sheet's content and accessibility, and
 * (3) PROBES one or more listed shortcuts of every group against the real handlers, so a listed-but-fake shortcut fails here. If a row is added to
 * shortcuts.js, the row-count guard fails until a probe is added or updated.
 */
const { test, expect } = require("@playwright/test");

const BASE_URL = "http://localhost:3000";
const ev = (page, fn, arg) => page.evaluate(fn, arg);
const sheet = (page) => page.locator("#shortcuts-dialog");

async function start(page) {
  const errors = [];
  page.on("console", (m) => { if (m.type() === "error") errors.push(m.text()); });
  page.on("pageerror", (e) => errors.push(String(e)));
  page.__errors = errors;
  await page.goto(BASE_URL);
  await ev(page, () => { window.AuroraSystem.dev.stepDelayMs = 0; });
}
const open = (page, app) => ev(page, (a) => window.AuroraShell.openApp(a), app);

test.describe("Shortcut sheet: when ? opens it", () => {
  test("? opens it from Home, as a labelled dialog with Close and the filter focused; Escape closes and focus returns", async ({ page }) => {
    await start(page);
    await page.locator("#dock-home").focus();
    await page.keyboard.press("?");
    await expect(sheet(page)).toBeVisible();
    await expect(page.getByRole("dialog", { name: "Keyboard shortcuts" })).toBeVisible();
    await expect(page.locator("#shortcuts-filter")).toBeFocused();
    await expect(page.locator("#btn-shortcuts-close")).toBeVisible();
    await page.keyboard.press("Escape");
    await expect(sheet(page)).toHaveCount(0);
    await expect(page.locator("#dock-home")).toBeFocused();
    expect(page.__errors).toEqual([]);
  });
  test("it is not opened while typing: a ? typed in the Notes body, the Index query or a Vault search is just text", async ({ page }) => {
    await start(page);
    await open(page, "notes"); await page.locator("#btn-notes-new").click();
    await page.locator("#notes-body").focus(); await page.keyboard.type("why?");
    await expect(page.locator("#notes-body")).toHaveValue("why?"); await expect(sheet(page)).toHaveCount(0);
    await page.keyboard.press("Control+k"); await page.locator("#index-query").fill("");
    await page.keyboard.type("?"); await expect(page.locator("#index-query")).toHaveValue("?"); await expect(sheet(page)).toHaveCount(0);
    await page.keyboard.press("Escape");
    await open(page, "vault"); await page.locator("#vault-search").focus(); await page.keyboard.type("?");
    await expect(page.locator("#vault-search")).toHaveValue("?"); await expect(sheet(page)).toHaveCount(0);
  });
  test("it is not opened with a modifier held, and not while any dialog is open (welcome, review, confirm)", async ({ page }) => {
    await start(page);
    await page.keyboard.press("Control+?"); await page.keyboard.press("Alt+?"); await page.keyboard.press("Meta+?");
    await expect(sheet(page)).toHaveCount(0);
    await ev(page, () => { window.AuroraUI.dialog({ id: "other-dialog", title: "A review", body: "trusted", actions: [{ label: "OK", value: true, id: "other-ok" }] }); });
    await page.keyboard.press("?");
    await expect(sheet(page)).toHaveCount(0);
    expect(await ev(page, () => window.AuroraShortcuts.show())).toBe(false);          // the API refuses too
    await page.locator("#other-ok").click();
    await page.keyboard.press("?");
    await expect(sheet(page)).toBeVisible();
  });
  test("it is not opened over the secure hold-to-confirm delete review", async ({ page }) => {
    await start(page);
    await open(page, "vault");
    await page.locator("#screen-vault .vault-open").first().focus();
    await page.keyboard.press("Delete");
    await expect(page.locator(".scrim .dialog").first()).toBeVisible();
    await page.keyboard.press("?");
    await expect(sheet(page)).toHaveCount(0);
    await page.keyboard.press("Escape");
  });
  test("a dialog cannot be bypassed by the sheet: while it is open ? typed in its filter is text, and Ctrl+K stays blocked", async ({ page }) => {
    await start(page);
    await page.keyboard.press("?");
    await page.keyboard.type("?"); await expect(page.locator("#shortcuts-filter")).toHaveValue("?");
    await expect(sheet(page)).toHaveCount(1);
    await page.keyboard.press("Control+k");
    await expect(page.locator("#index-query")).toBeHidden();
  });
  test("Index has a real entry: typing 'shortcuts' offers it and Enter opens the sheet; unrelated queries do not offer it", async ({ page }) => {
    await start(page);
    await page.keyboard.press("Control+k");
    await page.locator("#index-query").fill("notes");
    await expect(page.locator("#index-results .index-result", { hasText: "Keyboard shortcuts" })).toHaveCount(0);
    await page.locator("#index-query").fill("shortcuts");
    const item = page.locator("#index-results .index-result", { hasText: "Keyboard shortcuts" });
    await expect(item).toHaveCount(1);
    await item.click();
    await expect(sheet(page)).toBeVisible();
  });
});

test.describe("Shortcut sheet: content", () => {
  test("groups are organised, each row names its keys and effect, and the current screen's group comes first", async ({ page }) => {
    await start(page);
    await open(page, "notes");
    await page.keyboard.press("?");
    const titles = await page.locator(".kbd-group-title").allTextContents();
    expect(titles[0]).toBe("On this screen: Notes");
    expect(titles).toEqual(expect.arrayContaining(["Everywhere", "Index", "Dialogs", "Calculator", "Vault (Files)", "Tabs (Clock, Passport)"]));
    await expect(page.locator('[data-group="notes"] .kbd-row').first()).toContainText("New note");
    await page.keyboard.press("Escape");
    for (const [app, first] of [["calculator", "On this screen: Calculator"], ["vault", "On this screen: Vault (Files)"], ["clock", "On this screen: Tabs (Clock, Passport)"], ["passport", "On this screen: Tabs (Clock, Passport)"]]) {
      await open(page, app); await page.keyboard.press("?");
      expect((await page.locator(".kbd-group-title").first().textContent()), app).toBe(first);
      await page.keyboard.press("Escape");
    }
    await ev(page, () => window.AuroraShell.showScreen("screen-home")); await page.keyboard.press("?");
    expect(await page.locator(".kbd-group-title").first().textContent()).toBe("Everywhere");
  });
  test("drift guard: the table has exactly these groups and row counts (add a probe below when this changes)", async ({ page }) => {
    await start(page);
    expect(await ev(page, () => window.AuroraShortcuts.groups())).toEqual([
      { id: "everywhere", title: "Everywhere", rows: 3 }, { id: "index", title: "Index", rows: 3 }, { id: "dialogs", title: "Dialogs", rows: 3 }, { id: "notes", title: "Notes", rows: 5 },
      { id: "calculator", title: "Calculator", rows: 8 }, { id: "vault", title: "Vault (Files)", rows: 5 }, { id: "tabs", title: "Tabs (Clock, Passport)", rows: 2 }]);
  });
  test("Windows/Linux shows Ctrl and never Cmd", async ({ page }) => {
    await page.addInitScript(() => { Object.defineProperty(navigator, "platform", { get: () => "Win32" }); });
    await start(page); await page.keyboard.press("?");
    const kbds = await page.locator(".kbd").allTextContents();
    expect(kbds).toContain("Ctrl"); expect(kbds).not.toContain("Cmd");
  });
  test("a Mac shows Cmd and never Ctrl in the key caps", async ({ page }) => {
    await page.addInitScript(() => { Object.defineProperty(navigator, "platform", { get: () => "MacIntel" }); });
    await start(page); await page.keyboard.press("?");
    const kbds = await page.locator(".kbd").allTextContents();
    expect(kbds).toContain("Cmd"); expect(kbds).not.toContain("Ctrl");
    await expect(page.locator('[data-group="everywhere"] .kbd-row').first()).toHaveAttribute("aria-label", "Cmd plus K: Open Index to search apps, files, settings and commands.");
  });
  test("the filter narrows rows by key, group or effect, and says so when nothing matches", async ({ page }) => {
    await start(page); await page.keyboard.press("?");
    const total = await page.locator(".kbd-row").count();
    await page.locator("#shortcuts-filter").fill("save");
    await expect(page.locator(".kbd-row")).toHaveCount(1); await expect(page.locator(".kbd-row")).toContainText("Save now");
    await page.locator("#shortcuts-filter").fill("calculator");
    expect(await page.locator(".kbd-row").count()).toBe(8);
    await page.locator("#shortcuts-filter").fill("F9"); await expect(page.locator(".kbd-row")).toContainText("Change sign");
    await page.locator("#shortcuts-filter").fill("zzzz<b>");
    await expect(page.locator("#shortcuts-none")).toHaveText("No shortcut matches “zzzz<b>”."); await expect(page.locator(".kbd-row")).toHaveCount(0);
    await expect(page.locator("#shortcuts-list b")).toHaveCount(0);
    await page.locator("#shortcuts-filter").fill("");
    await expect(page.locator(".kbd-row")).toHaveCount(total);
  });
  test("accessibility: key rows have full spoken names, key caps are <kbd>, focus stays inside, Tab reaches Close", async ({ page }) => {
    await start(page); await page.keyboard.press("?");
    await expect(page.locator('[data-group="notes"] .kbd-row').first()).toHaveAttribute("aria-label", "Ctrl plus N: New note.");
    expect(await page.locator("kbd.kbd").count()).toBeGreaterThan(20);
    for (let i = 0; i < 4; i++) { await page.keyboard.press("Tab"); expect(await ev(page, () => !!document.activeElement.closest("#shortcuts-dialog"))).toBe(true); }
    await page.locator("#btn-shortcuts-close").focus(); await page.keyboard.press("Enter");
    await expect(sheet(page)).toHaveCount(0);
  });
});

test.describe("Shortcut sheet: every listed group is real (probes against the actual handlers)", () => {
  test("Everywhere: Mod+K opens Index; Esc goes Back; ? shows the list", async ({ page }) => {
    await start(page);
    await page.keyboard.press("Control+k"); await expect(page.locator("#index-query")).toBeVisible(); await page.keyboard.press("Escape");
    await open(page, "clock"); await page.keyboard.press("Escape");                       // Clock has no Esc job of its own: Back
    expect(await ev(page, () => window.AuroraShell.state.activeScreen)).toBe("screen-home");
    await open(page, "calculator"); await page.keyboard.press("Escape");                  // Calculator uses Esc to clear, as its group says
    expect(await ev(page, () => window.AuroraShell.state.activeScreen)).toBe("screen-calculator");
    await page.keyboard.press("?"); await expect(sheet(page)).toBeVisible();
  });
  test("Index: arrows move, Home/End jump after arrows, Enter opens", async ({ page }) => {
    await start(page);
    await page.keyboard.press("Control+k"); await page.locator("#index-query").fill("c");
    const rows = page.locator("#index-results .index-result"); expect(await rows.count()).toBeGreaterThan(2);
    await page.keyboard.press("ArrowDown"); await page.keyboard.press("ArrowDown");
    const active = () => ev(page, () => [...document.querySelectorAll("#index-results .index-result")].findIndex((n) => n.getAttribute("aria-selected") === "true" || n.classList.contains("is-active") || n.id === document.getElementById("index-query").getAttribute("aria-activedescendant")));
    const i2 = await active(); expect(i2).toBeGreaterThan(0);
    await page.keyboard.press("End"); const iEnd = await active(); expect(iEnd).toBe((await rows.count()) - 1);
    await page.keyboard.press("Home"); expect(await active()).toBe(0);
    await page.keyboard.press("Enter");
    await expect(page.locator("#index-query")).toBeHidden();
  });
  test("Dialogs: Esc closes, Tab stays inside, hold-to-confirm answers Space/Enter", async ({ page }) => {
    await start(page);
    await ev(page, () => { window.AuroraUI.dialog({ id: "d1", title: "T", body: "b", actions: [{ label: "A", value: 1, id: "da" }, { label: "B", value: 2, id: "db" }] }); });
    for (let i = 0; i < 5; i++) { await page.keyboard.press("Tab"); expect(await ev(page, () => !!document.activeElement.closest("#d1"))).toBe(true); }
    await page.keyboard.press("Escape"); await expect(page.locator("#d1")).toHaveCount(0);
    await open(page, "vault"); await page.locator("#screen-vault .vault-open").first().focus(); await page.keyboard.press("Delete");
    const hold = page.locator(".dialog .hold-btn").first(); await expect(hold).toBeVisible();
    await hold.focus(); await page.keyboard.down("Space");
    await expect(hold).toHaveClass(/holding/); await page.keyboard.up("Space");
    await expect(hold).not.toHaveClass(/holding/);                                      // released early: nothing confirmed
    await page.keyboard.press("Escape");
  });
  test("Notes: Mod+N, Mod+S, Mod+F, Esc (clears search), arrows/Home/End in the list", async ({ page }) => {
    await start(page);
    await open(page, "notes");
    await page.keyboard.press("Control+n"); await expect(page.locator("#notes-title")).toBeFocused();
    await page.locator("#notes-title").fill("One"); await page.keyboard.press("Control+s"); await expect(page.locator("#notes-status")).toHaveText(/^Saved/);
    await page.keyboard.press("Control+n"); await page.locator("#notes-title").fill("Two"); await page.keyboard.press("Control+s");
    await page.keyboard.press("Control+f"); await expect(page.locator("#notes-search")).toBeFocused();
    await page.keyboard.type("one"); await page.keyboard.press("Escape"); await expect(page.locator("#notes-search")).toHaveValue("");
    await page.locator("#notes-list .note-item").first().focus(); await page.keyboard.press("ArrowDown");
    await expect(page.locator("#notes-list .note-item").nth(1)).toBeFocused(); await page.keyboard.press("Home"); await expect(page.locator("#notes-list .note-item").first()).toBeFocused();
    await page.keyboard.press("End"); await expect(page.locator("#notes-list .note-item").last()).toBeFocused();
  });
  test("Calculator: digits, . , + - * /, Enter =, Backspace, Delete, Esc, %, F9 and _", async ({ page }) => {
    await start(page);
    await open(page, "calculator"); await ev(page, () => window.AuroraCalculator.reset());
    const r = page.locator("#calc-result");
    await page.keyboard.type("12+3"); await page.keyboard.press("Enter"); await expect(r).toHaveText("15");
    await page.keyboard.press("Escape"); await page.keyboard.type("6*7="); await expect(r).toHaveText("42");
    await page.keyboard.press("Escape"); await page.keyboard.type("9-4/2="); await expect(r).toHaveText("2.5");      // immediate execution, no precedence: (9-4)/2
    await page.keyboard.press("Escape"); await page.keyboard.type("1.5"); await page.keyboard.press("Backspace"); await expect(r).toHaveText("1.");
    await page.keyboard.press("Escape"); await page.keyboard.type("2,5"); await expect(r).toHaveText("2.5");
    await page.keyboard.press("Delete"); await expect(r).toHaveText("0");
    await page.keyboard.press("Escape"); await page.keyboard.type("50%"); await expect(r).toHaveText("0.5");
    await page.keyboard.press("Escape"); await page.keyboard.type("8"); await page.keyboard.press("F9"); await expect(r).toHaveText("-8");
    await page.keyboard.press("_"); await expect(r).toHaveText("8");
  });
  test("Vault: arrows/Home/End move, Space selects, Mod+A selects all, Delete opens a REVIEW (nothing deleted), Esc clears", async ({ page }) => {
    await start(page);
    await open(page, "vault");
    const files = page.locator("#screen-vault .vault-open"); const n = await files.count();
    await files.first().focus(); await page.keyboard.press("ArrowDown"); await expect(files.nth(1)).toBeFocused();
    await page.keyboard.press("End"); await expect(files.last()).toBeFocused(); await page.keyboard.press("Home"); await expect(files.first()).toBeFocused();
    await page.keyboard.press("Space"); await expect(page.locator("#screen-vault [role=checkbox][aria-checked=true]")).toHaveCount(1);
    await page.keyboard.press("Control+a"); await expect(page.locator("#screen-vault [role=checkbox][aria-checked=true]")).toHaveCount(n);
    const before = await ev(page, () => window.AuroraSystem.files.list().length);
    await page.keyboard.press("Delete"); await expect(page.locator(".scrim .dialog").first()).toBeVisible();
    expect(await ev(page, () => window.AuroraSystem.files.list().length)).toBe(before);       // only a review is open: nothing was deleted by the key
    await page.keyboard.press("Escape");
    await page.keyboard.press("Escape"); await expect(page.locator("#screen-vault [role=checkbox][aria-checked=true]")).toHaveCount(0);
  });
  test("Tabs: Left/Right and Home/End switch tabs in Clock and in Passport", async ({ page }) => {
    await start(page);
    await open(page, "clock"); await page.locator("#tab-clock-clock").focus();
    await page.keyboard.press("ArrowRight"); await expect(page.locator("#tab-clock-stopwatch")).toHaveAttribute("aria-selected", "true");
    await page.keyboard.press("End"); await expect(page.locator("#tab-clock-timer")).toHaveAttribute("aria-selected", "true");
    await page.keyboard.press("Home"); await expect(page.locator("#tab-clock-clock")).toHaveAttribute("aria-selected", "true");
    await open(page, "passport"); const tabs = page.locator("#screen-passport [role=tab]"); await tabs.first().focus();
    await page.keyboard.press("ArrowRight"); await expect(tabs.nth(1)).toHaveAttribute("aria-selected", "true");
    await page.keyboard.press("End"); await expect(tabs.last()).toHaveAttribute("aria-selected", "true");
    await page.keyboard.press("Home"); await expect(tabs.first()).toHaveAttribute("aria-selected", "true");
  });
});

test.describe("Shortcut sheet: layouts", () => {
  for (const [name, w, h, scale] of [["360 at 200% text", 360, 740, "200"], ["390", 390, 844], ["1280", 1280, 800]]) {
    test(`${name}: fits the viewport, scrolls inside, Close is reachable, no overflow`, async ({ page }) => {
      await page.setViewportSize({ width: w, height: h });
      await start(page);
      if (scale) await ev(page, (s) => document.documentElement.setAttribute("data-text-scale", s), scale);
      await page.keyboard.press("?");
      const b = await sheet(page).boundingBox();
      expect(b.x).toBeGreaterThanOrEqual(0); expect(b.x + b.width).toBeLessThanOrEqual(w + 0.5); expect(b.y + b.height).toBeLessThanOrEqual(h + 0.5);
      await page.locator("#btn-shortcuts-close").scrollIntoViewIfNeeded(); await expect(page.locator("#btn-shortcuts-close")).toBeInViewport();
      expect(await ev(page, () => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
      const tooWide = await page.locator("#shortcuts-dialog .kbd-row").evaluateAll((ns) => ns.filter((n) => n.scrollWidth > n.clientWidth + 1).length);
      expect(tooWide).toBe(0);
    });
  }
  test("high contrast thickens key caps; reduced motion leaves the dialog without animation", async ({ page }) => {
    await page.emulateMedia({ reducedMotion: "reduce" });
    await start(page);
    await ev(page, () => { window.AuroraShell.setContrast("high"); window.AuroraShell.setMotion("reduce"); });
    await page.keyboard.press("?");
    expect(await page.locator(".kbd").first().evaluate((n) => getComputedStyle(n).borderTopWidth)).toBe("2px");
    expect(await page.locator("#shortcuts-dialog").evaluate((n) => getComputedStyle(n).animationName)).toBe("none");
  });
});
