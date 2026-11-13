/**
 * tests/ui-golden/contextmenu.test.js - M8-E: the shared context menu (right-click, Shift+F10 / Menu key, long-press) in Vault and Pages.
 * The menu is a launcher only: Delete from the menu still opens the trusted review and needs the hold; nothing here approves anything. Real clicks, keys
 * and pointer events; the model is unit-tested in tests/unit/contextmenu.test.js.
 */
const { test, expect } = require("@playwright/test");

const BASE_URL = "http://localhost:3000";
const ev = (page, fn, arg) => page.evaluate(fn, arg);

async function start(page) {
  const errors = [];
  page.on("console", (m) => { if (m.type() === "error") errors.push(m.text()); });
  page.on("pageerror", (e) => errors.push(String(e)));
  page.__errors = errors;
  await page.goto(BASE_URL);
  await ev(page, () => { window.AuroraSystem.dev.stepDelayMs = 0; });
}
async function openVault(page) { await ev(page, () => window.AuroraShell.openApp("vault")); await expect(page.locator("#screen-vault h1")).toHaveText("Vault"); }
const menu = (page) => page.locator("#ctx-menu");
const entries = (page) => menu(page).getByRole("menuitem");
const fileRow = (page, n) => page.locator("#screen-vault .vault-item").nth(n || 0);
const fileBtn = (page, n) => fileRow(page, n).locator(".vault-open");
const fileCount = (page) => ev(page, () => window.AuroraSystem.files.list().length);
const labels = (page) => entries(page).locator(".ctx-label").allTextContents();
const dialog = (page) => page.locator("#review-dialog");
const active = (page) => ev(page, () => window.AuroraShell.state.activeScreen);
const inViewport = async (page) => {
  const b = await menu(page).boundingBox(), v = page.viewportSize();
  return b.x >= 0 && b.y >= 0 && b.x + b.width <= v.width + 0.5 && b.y + b.height <= v.height + 0.5;
};
async function touch(page, selector, type, dx, dy) {
  await page.evaluate(([sel, t, ox, oy]) => {
    const n = document.querySelector(sel), r = n.getBoundingClientRect(), x = r.left + r.width / 2 + (ox || 0), y = r.top + r.height / 2 + (oy || 0);
    (t === "pointerdown" ? n : n).dispatchEvent(new PointerEvent(t, { bubbles: true, cancelable: true, pointerType: "touch", clientX: x, clientY: y, isPrimary: true }));
  }, [selector, type, dx, dy]);
}

test.describe("Context menu: opening", () => {
  test("right-click on a Vault file opens a labelled menu with Preview, Select, Move and Delete; focus is on the first item", async ({ page }) => {
    await start(page); await openVault(page);
    await fileBtn(page).click({ button: "right" });
    await expect(menu(page)).toBeVisible();
    await expect(menu(page)).toHaveAttribute("role", "menu");
    await expect(menu(page)).toHaveAttribute("aria-label", /^Actions for .+/);
    expect(await labels(page)).toEqual(["Preview", "Select", "Move file…", "Delete file…"]);
    await expect(entries(page).first()).toBeFocused();
    expect(page.__errors).toEqual([]);
  });
  test("Shift+F10 opens it from the keyboard, anchored at the row, and Escape returns focus to that row without leaving Vault", async ({ page }) => {
    await start(page); await openVault(page);
    await fileBtn(page, 1).focus();
    await page.keyboard.press("Shift+F10");
    await expect(menu(page)).toBeVisible();
    const [m, r] = await Promise.all([menu(page).boundingBox(), fileRow(page, 1).boundingBox()]);
    expect(m.x).toBeGreaterThanOrEqual(r.x - 1); expect(m.x).toBeLessThan(r.x + 80);                                  // starts at the row's start
    expect(m.y + m.height).toBeGreaterThan(r.y - 40); expect(m.y).toBeLessThan(r.y + r.height + 40);                   // and touches the row vertically (it may flip above near the bottom edge)
    await page.keyboard.press("Escape");
    await expect(menu(page)).toHaveCount(0);
    await expect(fileBtn(page, 1)).toBeFocused();
    expect(await active(page)).toBe("screen-vault");                                                                 // that Escape closed the menu only
  });
  test("the Menu key opens it too", async ({ page }) => {
    await start(page); await openVault(page);
    await fileBtn(page).focus(); await page.keyboard.press("ContextMenu");
    await expect(menu(page)).toBeVisible(); await expect(menu(page).getByRole("menuitem")).toHaveCount(4);
  });
  test("a long press on touch opens it; a quick tap, or a press that moves (a scroll), does not", async ({ page }) => {
    await start(page); await openVault(page);
    const sel = "#screen-vault .vault-item .vault-open";
    await touch(page, sel, "pointerdown"); await page.waitForTimeout(150); await touch(page, sel, "pointerup");
    await page.waitForTimeout(600); await expect(menu(page)).toHaveCount(0);                                         // tap
    await touch(page, sel, "pointerdown"); await touch(page, sel, "pointermove", 0, 30); await page.waitForTimeout(700);
    await expect(menu(page)).toHaveCount(0);                                                                         // moved: cancelled
    await touch(page, sel, "pointerdown"); await page.waitForTimeout(700);
    await expect(menu(page)).toBeVisible();                                                                          // held
    expect(await inViewport(page)).toBe(true);
  });
  test("the release after a long press is not a tap: it does not open the preview", async ({ page }) => {
    await start(page); await openVault(page);
    const sel = "#screen-vault .vault-item .vault-open";
    await touch(page, sel, "pointerdown"); await page.waitForTimeout(700); await expect(menu(page)).toBeVisible();
    await page.evaluate((s) => document.querySelector(s).click(), sel);                                              // the synthetic click that follows a release
    await expect(page.locator("#vault-preview .vault-preview-head")).toHaveCount(0);
  });
  test("right-clicking something that is not an item (the heading, the toolbar) opens nothing", async ({ page }) => {
    await start(page); await openVault(page);
    await page.locator("#screen-vault h1").click({ button: "right" });
    await expect(menu(page)).toHaveCount(0);
  });
  test("it never opens over a dialog", async ({ page }) => {
    await start(page); await openVault(page);
    await page.keyboard.press("?");
    await expect(page.locator("#shortcuts-dialog")).toBeVisible();
    await page.evaluate(() => document.querySelector("#screen-vault .vault-open").dispatchEvent(new MouseEvent("contextmenu", { bubbles: true, cancelable: true, clientX: 50, clientY: 50, button: 2 })));
    await expect(menu(page)).toHaveCount(0);
  });
  test("the native browser menu is suppressed on an item, and a second right-click replaces the first menu (never two)", async ({ page }) => {
    await start(page); await openVault(page);
    await fileBtn(page).click({ button: "right" }); await expect(menu(page)).toHaveCount(1);
    await page.mouse.click(5, 5, { button: "right" });
    await expect(menu(page)).toHaveCount(0);
    await fileBtn(page, 1).click({ button: "right" }); await expect(menu(page)).toHaveCount(1);
    await expect(page.locator(".ctx-layer")).toHaveCount(1);
  });
});

test.describe("Context menu: keyboard operation", () => {
  test("arrows wrap, Home and End jump, a typed letter jumps to a match, Tab closes", async ({ page }) => {
    await start(page); await openVault(page);
    await fileBtn(page).focus(); await page.keyboard.press("Shift+F10");
    await expect(entries(page).nth(0)).toBeFocused();
    await page.keyboard.press("ArrowDown"); await expect(entries(page).nth(1)).toBeFocused();
    await page.keyboard.press("End"); await expect(entries(page).nth(3)).toBeFocused();
    await page.keyboard.press("ArrowDown"); await expect(entries(page).nth(0)).toBeFocused();                        // wraps
    await page.keyboard.press("ArrowUp"); await expect(entries(page).nth(3)).toBeFocused();
    await page.keyboard.press("Home"); await expect(entries(page).nth(0)).toBeFocused();
    await page.keyboard.press("m"); await expect(entries(page).nth(2)).toBeFocused();                                // Move
    await page.keyboard.press("Tab");
    await expect(menu(page)).toHaveCount(0); await expect(fileBtn(page)).toBeFocused();
  });
  test("Enter runs the focused item: Select turns on selection for that file and focus returns to the list", async ({ page }) => {
    await start(page); await openVault(page);
    await fileBtn(page).focus(); await page.keyboard.press("Shift+F10");
    await page.keyboard.press("ArrowDown"); await page.keyboard.press("Enter");
    await expect(menu(page)).toHaveCount(0);
    await expect(page.locator("#vault-selcount")).toHaveText("1 selected");
    await expect(fileBtn(page)).toBeFocused();
  });
  test("keys typed in the menu do not leak to the page (Ctrl+K and ? are inert while it is open)", async ({ page }) => {
    await start(page); await openVault(page);
    await fileBtn(page).focus(); await page.keyboard.press("Shift+F10");
    await page.keyboard.press("?"); await expect(page.locator("#shortcuts-dialog")).toHaveCount(0);
    await expect(menu(page)).toBeVisible();
  });
});

test.describe("Context menu: it is a launcher, never an authority", () => {
  test("Delete from the menu opens the trusted review; Cancel leaves every file; a plain click on the review does not delete", async ({ page }) => {
    await start(page); await openVault(page);
    const before = await fileCount(page);
    await fileBtn(page).click({ button: "right" });
    await entries(page).filter({ hasText: "Delete file…" }).click();
    await expect(dialog(page)).toBeVisible();
    expect(await fileCount(page)).toBe(before);
    await page.locator(".hold-alt").click();                                                                         // one press of the two-step alternative is not enough
    expect(await fileCount(page)).toBe(before);
    await page.locator("#btn-review-cancel").click();
    expect(await fileCount(page)).toBe(before);
  });
  test("only the real two-step confirmation deletes, and the file goes to Trail and can be undone", async ({ page }) => {
    await start(page); await openVault(page);
    const before = await fileCount(page);
    await fileBtn(page).click({ button: "right" });
    await entries(page).filter({ hasText: "Delete file…" }).click();
    await expect(dialog(page)).toBeVisible();
    await page.locator(".hold-alt").click(); await page.locator(".hold-alt").click();
    await expect.poll(() => fileCount(page)).toBe(before - 1);
    await expect(page.locator("#btn-vault-undo")).toBeVisible();
  });
  test("Move from the menu opens the destination picker, not an immediate move", async ({ page }) => {
    await start(page); await openVault(page);
    const before = await ev(page, () => JSON.stringify(window.AuroraSystem.files.list().map((f) => f.space)));
    await fileBtn(page).click({ button: "right" });
    await entries(page).filter({ hasText: "Move file…" }).click();
    await expect(page.locator(".scrim")).toBeVisible();
    expect(await ev(page, () => JSON.stringify(window.AuroraSystem.files.list().map((f) => f.space)))).toBe(before);
    await page.keyboard.press("Escape");
  });
  test("when Passport denies deleting, Delete is shown disabled with the reason, and neither click nor Enter runs it", async ({ page }) => {
    await start(page); await openVault(page);
    await ev(page, () => window.AuroraSystem.permissions.set("files.delete", "deny"));
    await fileBtn(page).click({ button: "right" });
    const del = entries(page).filter({ hasText: "Delete file…" });
    await expect(del).toHaveAttribute("aria-disabled", "true");
    await expect(menu(page)).toContainText("Passport blocks deleting files");
    await del.click({ force: true }); await expect(dialog(page)).toHaveCount(0); await expect(menu(page)).toBeVisible();
    await del.focus(); await page.keyboard.press("Enter"); await expect(dialog(page)).toHaveCount(0);
    expect(await ev(page, () => window.AuroraSystem.permissions.get("files.delete"))).toBe("deny");                  // and the menu never changed the permission
  });
  test("a right-click inside a multi-selection acts on the whole selection; elsewhere, on that one file", async ({ page }) => {
    await start(page); await openVault(page);
    await page.locator("#btn-vault-select").click();
    await fileRow(page, 0).locator(".vault-check").click(); await fileRow(page, 1).locator(".vault-check").click();
    await expect(page.locator("#vault-selcount")).toHaveText("2 selected");
    await fileBtn(page, 0).click({ button: "right" }); await expect(menu(page)).toBeVisible();
    expect(await labels(page)).toEqual(["Deselect", "Move 2 files…", "Delete 2 files…"]);
    await page.keyboard.press("Escape"); await expect(menu(page)).toHaveCount(0);
    await fileBtn(page, 3).click({ button: "right" }); await expect(menu(page)).toBeVisible();
    expect(await labels(page)).toEqual(["Preview", "Select", "Move file…", "Delete file…"]);
  });
  test("file names are text, never markup: a hostile name cannot inject into the menu", async ({ page }) => {
    await start(page);
    await ev(page, () => { window.AuroraSystem.world.files["timetable"].name = '<img src=x onerror="window.__x=1">.txt'; });
    await openVault(page);
    await page.locator('#screen-vault [data-file="timetable"] .vault-open').click({ button: "right" });
    await expect(menu(page)).toBeVisible();
    expect(await ev(page, () => document.querySelectorAll("#ctx-menu img, #screen-vault .vault-item img").length)).toBe(0);
    expect(await ev(page, () => window.__x)).toBeUndefined();
  });
});

test.describe("Context menu: outside clicks", () => {
  test("clicking outside only closes it: the control under the click is not activated", async ({ page }) => {
    await page.setViewportSize({ width: 1280, height: 1000 });
    await start(page); await openVault(page);
    await fileBtn(page, 2).click({ button: "right" }); await expect(menu(page)).toBeVisible();
    const pt = await page.evaluate(() => {
      const m = document.getElementById("ctx-menu").getBoundingClientRect(), b = document.getElementById("btn-vault-select").getBoundingClientRect();
      const c = [[b.left + b.width / 2, b.top + b.height / 2], [b.left + 4, b.top + 4], [b.right - 4, b.bottom - 4], [b.right - 4, b.top + 4], [b.left + 4, b.bottom - 4]];
      return c.find(([x, y]) => !(x >= m.left && x <= m.right && y >= m.top && y <= m.bottom)) || null;
    });
    expect(pt).not.toBeNull();
    await page.mouse.click(pt[0], pt[1]);
    await expect(menu(page)).toHaveCount(0);
    await expect(page.locator("#btn-vault-select")).toHaveAttribute("aria-pressed", "false");
  });
});

test.describe("Context menu: Pages", () => {
  async function openPages(page) { await ev(page, () => { window.AuroraShell.openApp("notes"); window.AuroraShell.showScreen("screen-pages"); }); await expect(page.locator("#screen-pages")).toHaveClass(/active/); }
  test("right-click on a Page card offers Open, Pause or Resume, Save when paused, and Close Page", async ({ page }) => {
    await start(page); await openPages(page);
    const card = page.locator("#pages-list .page-card").first();
    await card.locator(".page-open").click({ button: "right" });
    await expect(menu(page)).toHaveAttribute("aria-label", /^Actions for .+/);
    const l = await labels(page); expect(l[0]).toBe("Open"); expect(l).toContain("Close Page"); expect(l.some((x) => x === "Pause" || x === "Resume")).toBe(true);
  });
  test("Pause from the menu does exactly what the Pause button does, then Save appears", async ({ page }) => {
    await start(page); await openPages(page);
    const card = page.locator('#pages-list .page-card[data-state="running"]').first();
    const id = await card.getAttribute("data-page");
    await card.locator(".page-open").click({ button: "right" });
    await entries(page).filter({ hasText: /^Pause/ }).click();
    await expect(page.locator('#pages-list .page-card[data-page="' + id + '"]')).toHaveAttribute("data-state", "paused");
    await page.locator('#pages-list .page-card[data-page="' + id + '"] .page-open').click({ button: "right" });
    expect(await labels(page)).toContain("Save and free memory");
    await entries(page).filter({ hasText: "Save and free memory" }).click();
    await expect(page.locator('#pages-list .page-card[data-page="' + id + '"]')).toHaveAttribute("data-state", "saved");
  });
  test("Close Page removes the Page; Open goes to the app; Shift+F10 works on a card", async ({ page }) => {
    await start(page); await openPages(page);
    const cards = page.locator("#pages-list .page-card"); const n = await cards.count();
    const other = page.locator("#pages-list .page-card").filter({ hasNotText: "Notes" }).first();
    await other.locator(".page-open").focus(); await page.keyboard.press("Shift+F10");
    await entries(page).filter({ hasText: "Close Page" }).click();
    await expect(cards).toHaveCount(n - 1);
    await page.locator("#pages-list .page-card", { hasText: "Notes" }).first().locator(".page-open").click({ button: "right" });
    await expect(menu(page)).toBeVisible();
    await entries(page).filter({ hasText: /^Open$/ }).click();
    expect(await active(page)).not.toBe("screen-pages");
  });
});

test.describe("Context menu: placement and layout", () => {
  for (const [w, h] of [[360, 640], [390, 844], [412, 915], [768, 1024], [1024, 768], [1280, 800], [1440, 900]]) {
    test(`${w}x${h}: opened at the bottom-right corner of the last visible file, it stays fully inside the viewport`, async ({ page }) => {
      await page.setViewportSize({ width: w, height: h });
      await start(page); await openVault(page);
      const row = fileRow(page, 3); await row.scrollIntoViewIfNeeded();
      const b = await row.boundingBox();
      await page.mouse.click(Math.min(w - 2, b.x + b.width - 4), Math.min(h - 2, b.y + b.height - 4), { button: "right" });
      await expect(menu(page)).toBeVisible();
      expect(await inViewport(page)).toBe(true);
      for (const it of await entries(page).all()) { const ib = await it.boundingBox(); expect(ib.height).toBeGreaterThanOrEqual(43.5); }     // 44px+ targets
      expect(await ev(page, () => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
    });
  }
  test("200% text at 360px: the menu fits (it scrolls inside itself if needed) and every item is reachable", async ({ page }) => {
    await page.setViewportSize({ width: 360, height: 640 });
    await start(page); await openVault(page);
    await ev(page, () => document.documentElement.setAttribute("data-text-scale", "200"));
    await fileBtn(page).scrollIntoViewIfNeeded(); await fileBtn(page).click({ button: "right" });
    await expect(menu(page)).toBeVisible(); expect(await inViewport(page)).toBe(true);
    await page.keyboard.press("End"); await expect(entries(page).last()).toBeFocused(); await expect(entries(page).last()).toBeInViewport();
  });
  test("high contrast thickens the menu border; the menu is opaque (never glass)", async ({ page }) => {
    await start(page); await openVault(page);
    await ev(page, () => window.AuroraShell.setContrast("high"));
    await fileBtn(page).click({ button: "right" });
    const css = await menu(page).evaluate((n) => ({ w: getComputedStyle(n).borderTopWidth, bf: getComputedStyle(n).backdropFilter, bg: getComputedStyle(n).backgroundColor }));
    expect(css.w).toBe("2px"); expect(css.bf).toBe("none"); expect(css.bg).not.toMatch(/rgba\(.*,\s*0(\.\d+)?\)$/);
  });
  test("the menu closes if the page scrolls or the window resizes (it never floats away from its row)", async ({ page }) => {
    await start(page); await openVault(page);
    await fileBtn(page).click({ button: "right" }); await expect(menu(page)).toBeVisible();
    await page.setViewportSize({ width: 700, height: 600 });
    await expect(menu(page)).toHaveCount(0);
  });
});
