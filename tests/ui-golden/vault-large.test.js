/**
 * tests/ui-golden/vault-large.test.js - M8-H: long Vault lists. Over 60 files the list skips layout and paint for rows that are off screen (content-visibility).
 * The normal 10-file Vault must be untouched, and a long one must still behave exactly like a short one: keyboard, selection, search, sort, off-screen rows.
 */
const { test, expect } = require("@playwright/test");

const BASE_URL = "http://localhost:3000";
const ev = (page, fn, arg) => page.evaluate(fn, arg);

async function start(page, extra) {
  const errors = [];
  page.on("console", (m) => { if (m.type() === "error") errors.push(m.text()); });
  page.on("pageerror", (e) => errors.push(String(e)));
  page.__errors = errors;
  await page.goto(BASE_URL);
  await ev(page, (n) => {
    window.AuroraSystem.dev.stepDelayMs = 0;
    const w = window.AuroraSystem.world.files;
    for (let i = 0; i < n; i++) w["bulk-" + i] = { id: "bulk-" + i, name: "File " + (i + 1) + ".txt", kind: "text", space: ["study", "work", "projects", "personal"][i % 4], size: 100 + i, modified: Date.now() - i * 60000, preview: "preview " + i };
    window.AuroraShell.openApp("vault");
  }, extra || 0);
  await expect(page.locator("#screen-vault h1")).toHaveText("Vault");
}
const items = (page) => page.locator("#screen-vault .vault-item");
const cv = (page, n) => items(page).nth(n).evaluate((e) => getComputedStyle(e).contentVisibility);

test.describe("Vault: long lists", () => {
  test("the normal Vault (10 files) is untouched: no large-list class, every row fully rendered", async ({ page }) => {
    await start(page, 0);
    await expect(page.locator("#screen-vault ul.vault-list")).not.toHaveClass(/is-large/);
    expect(await items(page).count()).toBe(10);
    for (let i = 0; i < 10; i++) expect(await cv(page, i), "row " + i).toBe("visible");
  });
  test("exactly 60 files is still a normal list; 61 becomes a large one", async ({ page }) => {
    await start(page, 50);
    expect(await items(page).count()).toBe(60); await expect(page.locator("#screen-vault ul.vault-list")).not.toHaveClass(/is-large/);
    await ev(page, () => { window.AuroraSystem.world.files["extra"] = { id: "extra", name: "Extra.txt", kind: "text", space: "study", size: 1, modified: Date.now(), preview: "x" }; window.AuroraShell.showScreen("screen-home"); window.AuroraShell.openApp("vault"); });
    expect(await items(page).count()).toBe(61); await expect(page.locator("#screen-vault ul.vault-list")).toHaveClass(/is-large/);
  });
  test("a long list (200 files) skips off-screen rows in list and grid views", async ({ page }) => {
    await start(page, 190);
    await expect(page.locator("#screen-vault ul.vault-list")).toHaveClass(/is-large/);
    expect(await cv(page, 0)).toBe("auto"); expect(await cv(page, 150)).toBe("auto");
    await page.locator('[data-fk="view:grid"]').click();
    await expect(page.locator("#screen-vault ul.vault-grid")).toHaveClass(/is-large/);
    expect(await cv(page, 5)).toBe("auto");
  });
  test("off-screen rows still work: End focuses the last row and brings it into view, Space selects it, Ctrl+A selects everything", async ({ page }) => {
    await start(page, 190);
    await page.locator("#screen-vault .vault-open").first().focus();
    await page.keyboard.press("End");
    const last = items(page).last().locator(".vault-open");
    await expect(last).toBeFocused(); await expect(last).toBeInViewport();
    await page.keyboard.press("Space");
    await expect(page.locator("#vault-selcount")).toHaveText("1 selected");
    await page.keyboard.press("Control+a");
    await expect(page.locator("#vault-selcount")).toHaveText("200 selected");
  });
  test("clicking a row far down the list opens its preview", async ({ page }) => {
    await start(page, 190);
    const row = page.locator('#screen-vault [data-file="bulk-150"] .vault-open');
    await row.scrollIntoViewIfNeeded(); await row.click();
    await expect(page.locator("#vault-preview-name")).toHaveText("File 151.txt");
  });
  test("search and sort give the right rows on a long list, including natural number order", async ({ page }) => {
    await start(page, 190);
    await page.locator("#vault-search").fill("File 19");
    const names = await page.locator("#screen-vault .vault-name").allTextContents();
    expect(names.length).toBeGreaterThan(0); expect(names.every((n) => /File 19/.test(n))).toBe(true);
    await page.locator("#vault-search").fill("");
    const all = await page.locator("#screen-vault .vault-name").allTextContents();
    expect(all.indexOf("File 2.txt")).toBeLessThan(all.indexOf("File 10.txt"));                                      // 2 before 10: natural order survived the shared collator
    expect(all.indexOf("File 10.txt")).toBeLessThan(all.indexOf("File 100.txt"));
    expect(page.__errors).toEqual([]);
  });
  test("a long list does not widen the page or hide the toolbar", async ({ page }) => {
    await page.setViewportSize({ width: 360, height: 740 });
    await start(page, 190);
    expect(await ev(page, () => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
    await expect(page.locator("#vault-search")).toBeVisible();
  });
});
