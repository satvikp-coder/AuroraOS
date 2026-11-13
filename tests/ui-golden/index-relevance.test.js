/**
 * tests/ui-golden/index-relevance.test.js - M8-F: Index relevance.
 * Stronger matches first, recent use breaks ties inside a tier, and a conservative typo match exists for APP NAMES only. A typo match is only ever a
 * suggestion: it is never highlighted by default, Enter does not open it, and it ranks last. The ranking itself is unit-tested in tests/unit/index.test.js.
 */
const { test, expect } = require("@playwright/test");

const BASE_URL = "http://localhost:3000";
const ev = (page, fn, arg) => page.evaluate(fn, arg);
const opts = (page) => page.locator("#index-results .index-result");
const appTitles = (page) => page.locator('#index-results .index-result[data-group="apps"] .index-title').allInnerTexts();
const active = (page) => ev(page, () => window.AuroraShell.state.activeScreen);

async function start(page) {
  const errors = [];
  page.on("console", (m) => { if (m.type() === "error") errors.push(m.text()); });
  page.on("pageerror", (e) => errors.push(String(e)));
  page.__errors = errors;
  await page.goto(BASE_URL);
}
async function search(page, q) { await page.keyboard.press("Control+k"); await page.locator("#index-query").fill(q); }

test.describe("Index relevance: typo tolerance for app names", () => {
  test("a near miss on an app name is offered as a suggestion, labelled as one, and is not highlighted", async ({ page }) => {
    await start(page); await search(page, "calculater");
    const first = opts(page).first();
    await expect(first.locator(".index-title")).toHaveText("Calculator");
    await expect(first).toContainText("Did you mean Calculator?");
    await expect(page.locator('#index-results .index-result[aria-selected="true"]')).toHaveCount(0);
    await expect(page.locator("#index-query")).not.toHaveAttribute("aria-activedescendant", /.+/);
    expect(page.__errors).toEqual([]);
  });
  test("Enter does NOT open a typo suggestion; the screen stays on Index", async ({ page }) => {
    await start(page); await search(page, "calculater");
    await page.keyboard.press("Enter");
    expect(await active(page)).toBe("screen-index");
    expect(await ev(page, () => window.AuroraModels.pages.list().some((p) => p.appId === "calculator" && p.state === "running"))).toBe(false);
  });
  test("choosing it on purpose works: ArrowDown highlights it, Enter opens it", async ({ page }) => {
    await start(page); await search(page, "calculater");
    await page.keyboard.press("ArrowDown");
    await expect(opts(page).first()).toHaveAttribute("aria-selected", "true");
    await page.keyboard.press("Enter");
    expect(await active(page)).toBe("screen-calculator");
  });
  test("ArrowUp from nothing highlighted goes to the last suggestion, and a click works", async ({ page }) => {
    await start(page); await search(page, "clok");
    await page.keyboard.press("ArrowUp");
    await expect(opts(page).last()).toHaveAttribute("aria-selected", "true");
    await opts(page).first().click();
    expect(await active(page)).toBe("screen-clock");
  });
  test("the screen reader is told these are suggestions, not results", async ({ page }) => {
    await start(page); await search(page, "calculater");
    await expect(page.locator("#a11y-live")).toHaveText(/No exact match\. 1 suggestion\. Use the arrow keys to choose one\./);
  });
  test("common slips find the right app: missing, extra, swapped and wrong letters", async ({ page }) => {
    await start(page);
    for (const [q, name] of [["clok", "Clock"], ["setings", "Settings"], ["passprot", "Passport"], ["vualt", "Vault"], ["calculatro", "Calculator"]]) {
      await page.keyboard.press("Control+k"); await page.locator("#index-query").fill(q);
      expect(await appTitles(page), q).toContain(name);
    }
  });
  test("typo tolerance is for app names only: a misspelt FILE name finds nothing", async ({ page }) => {
    await start(page); await search(page, "timetabel");
    await expect(page.locator("#index-results .index-result", { hasText: "Exam timetable" })).toHaveCount(0);
    await expect(page.locator("#index-empty")).toContainText("No results");
  });
  test("too short or unrelated queries never produce a guess", async ({ page }) => {
    await start(page);
    for (const q of ["clk", "nxtq", "zebra", "banana", "weather"]) {
      await page.keyboard.press("Control+k"); await page.locator("#index-query").fill(q);
      await expect(page.locator('#index-results .index-result:has-text("Did you mean")'), q).toHaveCount(0);
    }
  });
  test("a real match is the default and a suggestion never displaces it", async ({ page }) => {
    await start(page); await search(page, "notes");
    await expect(opts(page).first()).toHaveAttribute("aria-selected", "true");
    await expect(page.locator('#index-results .index-result:has-text("Did you mean")')).toHaveCount(0);
    await page.keyboard.press("Enter");
    expect(await active(page)).toBe("screen-notes");
  });
});

test.describe("Index relevance: recency and ordering", () => {
  test("with an empty query, the apps you used most recently come first", async ({ page }) => {
    await start(page);
    await ev(page, () => window.AuroraShell.openApp("trail"));
    await ev(page, () => window.AuroraShell.openApp("passport"));
    await page.keyboard.press("Control+k");
    const t = await appTitles(page);
    expect(t.slice(0, 2)).toEqual(["Passport", "Trail"]);
  });
  test("using an app again moves it back to the front", async ({ page }) => {
    await start(page);
    await ev(page, () => { window.AuroraShell.openApp("trail"); window.AuroraShell.openApp("passport"); window.AuroraShell.openApp("trail"); });
    await page.keyboard.press("Control+k");
    expect((await appTitles(page)).slice(0, 2)).toEqual(["Trail", "Passport"]);
  });
  test("opening a result from Index counts as use", async ({ page }) => {
    await start(page); await search(page, "settings");
    await page.keyboard.press("Enter");
    await ev(page, () => window.AuroraShell.showScreen("screen-home"));
    await page.keyboard.press("Control+k");
    expect((await appTitles(page))[0]).toBe("Settings");
  });
  test("recency never lifts a weaker match above a stronger one: tiers stay in order for a one-letter query", async ({ page }) => {
    await start(page);
    // use the app whose name only CONTAINS the letter (a weak match) most recently, then check the strong ones still lead
    const r = await ev(page, async () => {
      const IM = window.AuroraIndexModel, apps = ["spaces", "passport", "files", "settings"];
      apps.forEach((a) => window.AuroraShell.openApp(a));
      document.dispatchEvent(new KeyboardEvent("keydown", { key: "k", ctrlKey: true, bubbles: true }));
      const input = document.getElementById("index-query"); input.value = "s"; input.dispatchEvent(new Event("input", { bubbles: true }));
      await new Promise((res) => setTimeout(res, 50));
      const names = [...document.querySelectorAll('#index-results .index-result[data-group="apps"] .index-title')].map((n) => n.textContent);
      return { names, tiers: names.map((n) => { const t = IM.matchTier("s", n, []); return t < 0 ? IM.TIER.KEYWORD : t; }) };   // -1 = matched only by keyword
    });
    expect(r.names.length).toBeGreaterThan(1);
    expect(r.tiers).toEqual([...r.tiers].sort((a, b) => a - b));
  });
  test("the same query always gives the same order (no randomness)", async ({ page }) => {
    await start(page); await ev(page, () => window.AuroraShell.openApp("clock"));
    await search(page, "c");
    const a = await opts(page).allInnerTexts();
    await ev(page, () => window.AuroraShell.refreshIndex());
    expect(await opts(page).allInnerTexts()).toEqual(a);
    await page.locator("#index-query").fill("ca"); await page.locator("#index-query").fill("c");
    expect(await opts(page).allInnerTexts()).toEqual(a);
  });
  test("an exact name is the default result over a longer name that merely starts with it", async ({ page }) => {
    await start(page);
    await ev(page, () => { const w = window.AuroraSystem.world.files; w["zz-long"] = Object.assign({}, w["timetable"], { id: "zz-long", name: "Clock timetable notes.txt" }); w["zz-exact"] = Object.assign({}, w["timetable"], { id: "zz-exact", name: "Clock" }); });
    await search(page, "clock");
    const titles = await page.locator('#index-results .index-result[data-group="files"] .index-title').allInnerTexts();
    expect(titles.indexOf("Clock")).toBeGreaterThanOrEqual(0);
    expect(titles.indexOf("Clock")).toBeLessThan(titles.indexOf("Clock timetable notes.txt"));
  });
  test("several typed words match in any order: 'mode dark' finds Dark mode-style names", async ({ page }) => {
    await start(page);
    await ev(page, () => { const w = window.AuroraSystem.world.files; w["zz-dm"] = Object.assign({}, w["timetable"], { id: "zz-dm", name: "Dark mode ideas.txt" }); });
    await search(page, "mode dark");
    await expect(page.locator("#index-results .index-result", { hasText: "Dark mode ideas.txt" })).toHaveCount(1);
  });
});

test.describe("Index relevance: nothing else changed", () => {
  test("Aura commands still lead and still need their own flow; Home and Dock still open apps", async ({ page }) => {
    await start(page); await search(page, "start a 10 minute timer");
    await expect(page.locator('#index-results .index-result[data-group="aura"]').first()).toBeVisible();
    await ev(page, () => window.AuroraShell.showScreen("screen-home"));
    await ev(page, () => window.AuroraShell.openApp("notes"));
    expect(await active(page)).toBe("screen-notes");
  });
  test("opening an app before Index has ever been used does not break (recent list is created lazily)", async ({ page }) => {
    await start(page);
    expect(await ev(page, () => window.AuroraShell.openApp("clock").ok)).toBe(true);
    expect(page.__errors).toEqual([]);
  });
});
