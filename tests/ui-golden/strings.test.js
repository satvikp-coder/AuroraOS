/**
 * tests/ui-golden/strings.test.js - M8-G: language-ready strings.
 * English is the only verified language. Hindi and Gujarati are placeholders that show English (and must not claim another <html lang>). The test-only
 * pseudo-locale "en-XA" (accented, ~40% longer) stresses layouts with long strings at every viewport. The catalog and source scan are in tests/unit/strings.test.js.
 */
const { test, expect } = require("@playwright/test");

const BASE_URL = "http://localhost:3000";
const ev = (page, fn, arg) => page.evaluate(fn, arg);
const VIEWPORTS = [[360, 740], [390, 844], [412, 915], [768, 1024], [1024, 768], [1280, 800], [1440, 900]];

async function start(page, query) {
  const errors = [];
  page.on("console", (m) => { if (m.type() === "error") errors.push(m.text()); });
  page.on("pageerror", (e) => errors.push(String(e)));
  page.__errors = errors;
  await page.goto(BASE_URL + (query || ""));
}
const dockLabels = (page) => page.locator(".dock-label").allTextContents();
const dockAria = (page) => page.locator(".dock-btn").evaluateAll((n) => n.map((b) => b.getAttribute("aria-label")));
const htmlAttr = (page, a) => ev(page, (x) => document.documentElement.getAttribute(x), a);

test.describe("Strings: English is the verified default", () => {
  test("the dock, and every migrated string, read exactly as before; <html lang> is en and the locale is verified", async ({ page }) => {
    await start(page);
    expect(await dockLabels(page)).toEqual(["Back", "Home", "Spaces", "Pages", "Ask"]);
    expect(await dockAria(page)).toEqual(["Back", "Home", "Spaces", "Pages", "Ask Aura / Search"]);
    expect(await htmlAttr(page, "lang")).toBe("en"); expect(await htmlAttr(page, "data-locale")).toBe("en"); expect(await htmlAttr(page, "data-locale-status")).toBe("verified");
    expect(await ev(page, () => window.AuroraStrings.isVerified(window.AuroraStrings.locale()))).toBe(true);
    expect(page.__errors).toEqual([]);
  });
  test("using the interface in English records no missing key or unfilled parameter (welcome, shortcuts, Index)", async ({ page }) => {
    await start(page, "?welcome=1");
    await page.locator("#welcome-dialog").waitFor();
    await page.locator("#btn-welcome-next").click(); await page.locator("#btn-welcome-next").click(); await page.locator("#btn-welcome-back").click();
    await page.keyboard.press("Escape"); await page.locator("#welcome-dialog").waitFor({ state: "detached" });
    await page.keyboard.press("?"); await page.locator("#shortcuts-dialog").waitFor();
    await page.locator("#shortcuts-filter").fill("zzzzqq"); await expect(page.locator("#shortcuts-none")).toContainText("No shortcut matches");
    await page.keyboard.press("Escape");
    await page.keyboard.press("Control+k"); await page.locator("#index-query").fill("notes"); await page.locator("#index-query").fill("zzzzqq");
    expect(await ev(page, () => window.AuroraStrings.missing())).toEqual([]);
  });
});

test.describe("Strings: placeholders and unknown languages", () => {
  for (const l of ["hi", "gu"]) {
    test(`?lang=${l} (a placeholder) shows English, marks itself a placeholder, and does NOT claim another <html lang>`, async ({ page }) => {
      await start(page, "?welcome=1&lang=" + l);
      expect(await htmlAttr(page, "data-locale")).toBe(l); expect(await htmlAttr(page, "data-locale-status")).toBe("placeholder"); expect(await htmlAttr(page, "lang")).toBe("en");
      expect(await dockLabels(page)).toEqual(["Back", "Home", "Spaces", "Pages", "Ask"]);
      await expect(page.locator("#welcome-heading")).toHaveText("Spaces keep your day organised");
      await expect(page.locator("#welcome-step-label")).toHaveText("Step 1 of 3");
      expect(await ev(page, () => window.AuroraStrings.isVerified(window.AuroraStrings.locale()))).toBe(false);
    });
  }
  test("an unknown or hostile ?lang is ignored", async ({ page }) => {
    for (const q of ["?lang=xx", "?lang=__proto__", "?lang=constructor", "?lang=", "?lang=<img>"]) {
      await start(page, q);
      expect(await ev(page, () => window.AuroraStrings.locale()), q).toBe("en"); expect(await htmlAttr(page, "lang"), q).toBe("en");
    }
  });
  test("the pseudo-locale is not offered as a language to people", async ({ page }) => {
    await start(page);
    expect(await ev(page, () => window.AuroraStrings.locales().filter((l) => l.status === "test-only").map((l) => l.id))).toEqual(["en-XA"]);
    expect(await page.locator("text=Pseudo-locale").count()).toBe(0);
  });
});

test.describe("Strings: pseudo-locale (test only)", () => {
  test("static HTML and dynamic dialogs both follow the language: dock, welcome, shortcut sheet", async ({ page }) => {
    await start(page, "?welcome=1&lang=en-XA");
    const dock = await dockLabels(page);
    expect(dock.every((x) => /^\[.+\]$/.test(x))).toBe(true); expect(dock[1]).not.toBe("Home");
    expect((await dockAria(page))[4]).toMatch(/^\[.+\]$/);
    await page.locator("#welcome-dialog").waitFor();
    await expect(page.locator("#welcome-step-label")).toHaveText(/^\[.+ 1 .+ 3 .+\]$/);                     // {n} and {total} were still filled
    await expect(page.locator("#btn-welcome-next")).toHaveText(/^\[.+\]$/);
    await page.keyboard.press("Escape");
    await page.keyboard.press("?"); await page.locator("#shortcuts-dialog").waitFor();
    await expect(page.locator("#shortcuts-dialog .dialog-title")).toHaveText(/^\[.+\]$/);
    await expect(page.locator("#shortcuts-filter")).toHaveAttribute("placeholder", /^\[.+\]$/);
    expect(await ev(page, () => window.AuroraStrings.missing())).toEqual([]);
    expect(await htmlAttr(page, "lang")).toBe("en-XA"); expect(await htmlAttr(page, "data-locale-status")).toBe("test-only");
    expect(page.__errors).toEqual([]);
  });
  test("setLocale at runtime re-translates the static dock, fires aurora:locale, and refuses an unknown language", async ({ page }) => {
    await start(page);
    await ev(page, () => { window.__loc = []; document.addEventListener("aurora:locale", (e) => window.__loc.push(e.detail.locale)); });
    expect(await ev(page, () => window.AuroraStrings.setLocale("en-XA"))).toBe(true);
    expect((await dockLabels(page))[1]).toMatch(/^\[.+\]$/);
    expect(await ev(page, () => window.AuroraStrings.setLocale("nope"))).toBe(false);
    expect(await ev(page, () => window.AuroraStrings.setLocale("en"))).toBe(true);
    expect(await dockLabels(page)).toEqual(["Back", "Home", "Spaces", "Pages", "Ask"]);
    expect(await ev(page, () => window.__loc)).toEqual(["en-XA", "en"]);
  });
  test("the Index result count follows the language and keeps English plural rules", async ({ page }) => {
    await start(page, "?lang=hi");
    await page.keyboard.press("Control+k"); await page.locator("#index-query").fill("zzzzqq");
    await expect(page.locator("#a11y-live")).toHaveText("No results");
    await page.locator("#index-query").fill("notes");
    await expect(page.locator("#a11y-live")).toHaveText(/^\d+ results?$/);
  });
});

/** In-page checks for the long-string stress. Returns a list of problems. */
function inspectLayout({ checkDock }) {
  const out = [], vw = window.innerWidth, vh = window.innerHeight;
  if (document.documentElement.scrollWidth > vw + 1) out.push("horizontal overflow " + document.documentElement.scrollWidth + " > " + vw);
  const dlg = document.querySelector(".dialog");
  if (dlg) {
    const r = dlg.getBoundingClientRect();
    if (r.left < -1 || r.right > vw + 1) out.push("dialog clipped horizontally");
    dlg.querySelectorAll("button, input, select").forEach((b) => {
      if (b.hidden || b.offsetParent === null) return;
      b.scrollIntoView({ block: "nearest" });
      const br = b.getBoundingClientRect();
      if (br.left < -1 || br.right > vw + 1 || br.top < -1 || br.bottom > vh + 1) out.push("not reachable: " + (b.id || b.className));
      if (Math.min(br.width, br.height) < 44 && b.type !== "search") out.push("target < 44px: " + (b.id || b.textContent.trim().slice(0, 20)) + " " + Math.round(br.width) + "x" + Math.round(br.height));
    });
    dlg.querySelectorAll("h3, p, li, .dialog-title").forEach((n) => { if (n.scrollWidth > n.clientWidth + 1 && getComputedStyle(n).overflowX !== "visible") out.push("text clipped: " + n.className); });
  }
  if (checkDock) {
    document.querySelectorAll(".dock-btn").forEach((b) => {
      const r = b.getBoundingClientRect();
      if (r.left < -1 || r.right > vw + 1) out.push("dock button outside viewport: " + b.id);
      if (Math.min(r.width, r.height) < 44) out.push("dock target < 44px: " + b.id + " " + Math.round(r.width) + "x" + Math.round(r.height));
      const l = b.querySelector(".dock-label");
      if (l && l.offsetParent !== null) { const lr = l.getBoundingClientRect(); if (lr.left < r.left - 1 || lr.right > r.right + 1) out.push("dock label spills out of its button: " + b.id + " " + Math.round(lr.width) + " in " + Math.round(r.width)); }
    });
  }
  return out;
}

test.describe("Strings: long-string layout stress (pseudo-locale, every viewport)", () => {
  for (const [w, h] of VIEWPORTS) {
    test(`${w}x${h}: Home and Dock, the welcome tour (every step) and the shortcut sheet hold their layout`, async ({ page }) => {
      await page.setViewportSize({ width: w, height: h });
      await start(page, "?welcome=1&lang=en-XA");
      await page.locator("#welcome-dialog").waitFor();
      for (let i = 0; i < 3; i++) {
        expect(await ev(page, inspectLayout, { checkDock: false }), `welcome step ${i + 1}`).toEqual([]);
        if (i < 2) await page.locator("#btn-welcome-next").click();
      }
      await page.locator("#btn-welcome-next").click();                                                  // Finish
      await page.locator("#welcome-dialog").waitFor({ state: "detached" });
      expect(await ev(page, inspectLayout, { checkDock: true }), "home and dock").toEqual([]);
      await page.keyboard.press("?"); await page.locator("#shortcuts-dialog").waitFor();
      expect(await ev(page, inspectLayout, { checkDock: false }), "shortcut sheet").toEqual([]);
      expect(page.__errors).toEqual([]);
    });
  }
  test("360px at 200% text: dialogs still fit and scroll, and the actions stay reachable", async ({ page }) => {
    await page.setViewportSize({ width: 360, height: 740 });
    await start(page, "?welcome=1&lang=en-XA");
    await ev(page, () => document.documentElement.setAttribute("data-text-scale", "200"));
    await page.locator("#welcome-dialog").waitFor();
    for (let i = 0; i < 3; i++) { expect(await ev(page, inspectLayout, { checkDock: false }), `welcome step ${i + 1}`).toEqual([]); if (i < 2) { await page.locator("#btn-welcome-next").scrollIntoViewIfNeeded(); await page.locator("#btn-welcome-next").click(); } }
  });
});
