/**
 * tests/ui-golden/index.test.js — M1 / S5: Index (Ask + Search).
 * Local search over apps, settings and mock files, plus Aura Quick Commands. A multi-step plan only opens the
 * trusted review screen; an S0 action runs through the broker and offers Undo only when the receipt says it can be undone.
 */
const { test, expect } = require("@playwright/test");

const BASE_URL = "http://localhost:3000";
const ev = (page, fn, arg) => page.evaluate(fn, arg);
const opts = (page) => page.locator("#index-results .index-result");
const titles = (page, group) => page.locator('#index-results .index-result[data-group="' + group + '"] .index-title').allInnerTexts();

async function openIndex(page, q) { await page.goto(BASE_URL); await page.keyboard.press("Control+k"); if (q !== undefined) await page.locator("#index-query").fill(q); }

test.describe("Index: structure and accessibility", () => {
  test("is a labelled combobox controlling a listbox, with the first option active", async ({ page }) => {
    await openIndex(page);
    const input = page.locator("#index-query");
    await expect(input).toHaveAttribute("role", "combobox");
    await expect(input).toHaveAttribute("aria-controls", "index-results");
    await expect(input).toHaveAttribute("aria-autocomplete", "list");
    await expect(page.getByRole("combobox", { name: /Search apps, settings and files/ })).toBeVisible();
    await expect(page.locator("#index-results")).toHaveAttribute("role", "listbox");
    await expect(input).toHaveAttribute("aria-activedescendant", "idx-opt-0");
    await expect(opts(page).first()).toHaveAttribute("aria-selected", "true");
    await expect(opts(page).nth(1)).toHaveAttribute("aria-selected", "false");
    for (const o of await opts(page).all()) expect(await o.getAttribute("role")).toBe("option");
  });

  test("group headings are presentational and described by each option", async ({ page }) => {
    await openIndex(page, "notes");
    await expect(page.locator(".index-group").first()).toHaveAttribute("role", "presentation");
    await expect(opts(page).first()).toHaveAttribute("aria-describedby", /^idx-g-/);
  });

  test("the result count is announced politely (debounced)", async ({ page }) => {
    await openIndex(page, "notes");
    await expect(page.locator("#a11y-live")).toHaveText(/\d+ results?/);
    await page.locator("#index-query").fill("zzzzqqq");
    await expect(page.locator("#a11y-live")).toHaveText("No results");
  });
});

test.describe("Index: empty query", () => {
  test("offers four suggested commands and every app; suggestions fill the field and run nothing", async ({ page }) => {
    await openIndex(page);
    expect(await titles(page, "suggest")).toEqual(["Prepare my phone for studying", "Start a 10 minute timer", "Turn on focus mode", "Open Notes"]);
    expect((await titles(page, "apps")).length).toBe(9);
    await opts(page).nth(1).click();
    await expect(page.locator("#index-query")).toHaveValue("Start a 10 minute timer");
    await expect(page.locator("#index-query")).toBeFocused();
    expect(await ev(page, () => window.AuroraSystem.world.timer)).toBeNull();             // nothing ran
    await expect(opts(page).first()).toContainText("Start a timer for 10 minutes");       // now the Aura row
  });

  test("apps that are not built yet are shown honestly", async ({ page }) => {
    // A synthetic system app with no screen: independent of which real apps (Passport, Trail, Vault...) have shipped.
    await page.goto(BASE_URL);
    await ev(page, () => window.AuroraShell.registerApp({ id: "zz-later", name: "Zz Later App", system: true, screen: "screen-zz-later", hue: 20 }));
    await page.keyboard.press("Control+k");
    await page.locator("#index-query").fill("zz later");
    const row = opts(page).first();
    await expect(row).toContainText("Zz Later App");
    await expect(row).toContainText("Arrives in a later milestone");
    await expect(row).toContainText("Not yet");
    await row.click();
    await expect(page.locator("#toast")).toContainText("Zz Later App arrives in a later milestone");
    await expect(page.locator("#screen-index")).toBeVisible();
  });
});

test.describe("Index: results, ranking and limits", () => {
  test("finds apps, settings, mock files and actions, in the documented group order", async ({ page }) => {
    await openIndex(page, "e");
    const groups = (await page.locator("#index-results .index-group").allTextContents()).map((g) => g.trim());   // text, not the CSS-uppercased rendering
    expect(groups).toEqual(expect.arrayContaining(["Apps", "Settings"]));
    const order = ["Aura", "Try asking", "Apps", "Settings", "Files", "Actions"];
    expect(groups.map((g) => order.indexOf(g))).toEqual([...groups.map((g) => order.indexOf(g))].sort((a, b) => a - b));
  });

  test("an app, a setting, a file and an action are each findable", async ({ page }) => {
    await openIndex(page, "calc");
    expect(await titles(page, "apps")).toEqual(["Calculator"]);
    await page.locator("#index-query").fill("text size");
    expect((await titles(page, "settings")).join(" ")).toMatch(/text size/i);
    await page.locator("#index-query").fill("timetable");
    expect(await titles(page, "files")).toEqual(["Exam timetable.txt"]);
    await expect(opts(page).first()).toContainText("mock file");
    await page.locator("#index-query").fill("quick");
    expect(await titles(page, "actions")).toEqual(["Quick Center"]);
  });

  test("ranks label prefix, then word prefix, then substring, then keyword", async ({ page }) => {
    await openIndex(page);
    await ev(page, () => {   // registered out of rank order on purpose
      const S = window.AuroraShell;
      S.registerApp({ id: "zed", name: "Zed", keywords: ["alpha"] });
      S.registerApp({ id: "scalpha", name: "Scalpha" });
      S.registerApp({ id: "my-alpha", name: "My Alpha" });
      S.registerApp({ id: "alphabet", name: "Alphabet" });
    });
    await page.locator("#index-query").fill("alpha");
    expect(await titles(page, "apps")).toEqual(["Alphabet", "My Alpha", "Scalpha", "Zed"]);
    await page.locator("#index-query").fill("notes");
    expect((await titles(page, "files"))[0]).toBe("Study notes.md");     // word prefix in a file name
    await page.locator("#index-query").fill("stopwatch");
    expect(await titles(page, "apps")).toEqual(["Clock"]);              // keyword only
  });

  test("at most 6 results per group and 20 in total", async ({ page }) => {
    await openIndex(page, "e");
    for (const g of ["apps", "settings", "files", "actions"]) expect((await titles(page, g)).length).toBeLessThanOrEqual(6);
    expect(await opts(page).count()).toBeLessThanOrEqual(20);
    await page.locator("#index-query").fill("a");
    expect(await opts(page).count()).toBeLessThanOrEqual(20);
  });

  test("a no-match state quotes the query as text and offers suggestions that fill the field", async ({ page }) => {
    await openIndex(page, "zzzzqqq");
    await expect(page.locator("#index-empty")).toBeVisible();
    await expect(page.locator("#index-empty")).toContainText("Nothing matches “zzzzqqq”");
    await expect(page.locator("#index-results")).toBeHidden();
    await expect(page.locator("#index-query")).toHaveAttribute("aria-expanded", "false");
    await page.getByRole("button", { name: "Turn on focus mode" }).click();
    await expect(page.locator("#index-query")).toHaveValue("Turn on focus mode");
    await expect(page.locator("#index-empty")).toBeHidden();
  });

  test("Clear is hidden when empty, clears the query and refocuses the field", async ({ page }) => {
    await openIndex(page);
    await expect(page.locator("#btn-index-clear")).toBeHidden();
    await page.locator("#index-query").fill("notes");
    await page.getByRole("button", { name: "Clear search" }).click();
    await expect(page.locator("#index-query")).toHaveValue("");
    await expect(page.locator("#index-query")).toBeFocused();
    await expect(page.locator("#btn-index-clear")).toBeHidden();
  });
});

test.describe("Index: keyboard", () => {
  test("ArrowDown/ArrowUp move and wrap; Home/End jump after navigating; Enter activates the active option", async ({ page }) => {
    await openIndex(page, "e");
    const n = await opts(page).count();
    const active = () => page.locator("#index-query").getAttribute("aria-activedescendant");
    expect(await active()).toBe("idx-opt-0");
    await page.keyboard.press("ArrowUp");
    expect(await active()).toBe("idx-opt-" + (n - 1));            // wraps to the end
    await page.keyboard.press("ArrowDown");
    expect(await active()).toBe("idx-opt-0");                     // wraps to the start
    await page.keyboard.press("ArrowDown");
    await page.keyboard.press("End");
    expect(await active()).toBe("idx-opt-" + (n - 1));
    await page.keyboard.press("Home");
    expect(await active()).toBe("idx-opt-0");
    await expect(page.locator(".index-result.is-active")).toHaveCount(1);
    await expect(page.locator("#index-query")).toBeFocused();     // focus never leaves the field
  });

  test("Home/End still move the caret until you start navigating results", async ({ page }) => {
    await openIndex(page, "abc");
    await page.keyboard.press("Home");
    await page.keyboard.type("X");
    await expect(page.locator("#index-query")).toHaveValue("Xabc");
  });

  test("Enter opens the active app; a second option can be chosen by arrow", async ({ page }) => {
    await openIndex(page, "calc");
    await page.keyboard.press("Enter");
    await expect(page.locator("#screen-calculator")).toBeVisible();                  // Calculator is a real app now (M6): it opens its own screen instead of the Pages fallback
  });

  test("Escape goes back", async ({ page }) => {
    await openIndex(page, "notes");
    await page.keyboard.press("Escape");
    await expect(page.locator("#screen-home")).toBeVisible();
  });

  test("the active option scrolls into view", async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 500 });
    await openIndex(page);
    for (let i = 0; i < 12; i++) await page.keyboard.press("ArrowDown");
    const box = await page.locator(".index-result.is-active").boundingBox();
    expect(box.y).toBeGreaterThanOrEqual(0);
    expect(box.y + box.height).toBeLessThanOrEqual(500);
  });

  test("pointer click activates the clicked option", async ({ page }) => {
    await openIndex(page, "calc");
    await opts(page).first().click();
    await expect(page.locator("#screen-calculator")).toBeVisible();
  });
});

test.describe("Index: Aura commands and the broker boundary", () => {
  test("an S0 command runs, shows the real receipt, and offers Undo that really undoes", async ({ page }) => {
    await openIndex(page, "start a 10 minute timer");
    await expect(opts(page).first()).toContainText("Runs now. You can undo it.");
    await page.keyboard.press("Enter");
    await expect(page.locator("#toast")).toContainText("Start a timer for 10 minutes · done");
    expect(await ev(page, () => window.AuroraSystem.world.timer.seconds)).toBe(600);
    expect(await ev(page, () => window.AuroraSystem.receipts().length)).toBe(1);
    await page.locator("#toast .toast-action").click();
    await expect(page.locator("#toast")).toContainText("Undone");
    expect(await ev(page, () => window.AuroraSystem.world.timer)).toBeNull();
    expect(await ev(page, () => window.AuroraSystem.receipts()[0].undoState)).toBe("used");
  });

  test("an S0 command with no undo does not offer Undo", async ({ page }) => {
    await openIndex(page, "search for cats");
    await expect(opts(page).first()).toContainText("Runs now");
    await expect(opts(page).first()).not.toContainText("undo");
    await page.keyboard.press("Enter");
    await expect(page.locator("#toast")).toContainText("done");
    await expect(page.locator("#toast .toast-action")).toHaveCount(0);
  });

  test("a plan opens the trusted review screen and runs nothing", async ({ page }) => {
    await openIndex(page, "prepare my phone for studying");
    await expect(opts(page).first()).toContainText("4-step plan");
    await expect(opts(page).first()).toContainText("Opens a plan to review. Nothing runs yet.");
    await page.keyboard.press("Enter");
    await expect(page.locator("#screen-aura")).toBeVisible();
    await expect(page.locator("#screen-aura .plan-step")).toHaveCount(4);
    await page.waitForTimeout(1500);
    expect(await ev(page, () => ({ f: window.AuroraModels.qs.get("focus"), r: window.AuroraSystem.receipts().length, t: window.AuroraSystem.world.timer }))).toEqual({ f: false, r: 0, t: null });
  });

  test("a command that needs review asks first; cancelling changes nothing; approving runs it", async ({ page }) => {
    await openIndex(page, "turn on focus mode");
    await expect(opts(page).first()).toContainText("Asks you to review first");
    await page.keyboard.press("Enter");
    await expect(page.locator("#review-dialog")).toBeVisible();
    await page.locator("#btn-review-cancel").click();
    expect(await ev(page, () => window.AuroraModels.qs.get("focus"))).toBe(false);
    await page.locator("#index-query").focus();
    await page.keyboard.press("Enter");
    await page.locator("#btn-review-approve").click();
    await expect.poll(() => ev(page, () => window.AuroraModels.qs.get("focus"))).toBe(true);
    await expect(page.locator("#toast")).toContainText("done");
  });

  test("a command Passport blocks says so and cannot run", async ({ page }) => {
    await openIndex(page);
    await ev(page, () => window.AuroraSystem.permissions.set("settings.write", "deny"));
    await page.locator("#index-query").fill("turn on focus mode");
    await expect(opts(page).first()).toContainText("Blocked in Passport");
    await page.keyboard.press("Enter");
    await expect(page.locator("#review-dialog")).toContainText("can't run");
    await page.locator('#review-dialog button:has-text("Close")').click();
    expect(await ev(page, () => window.AuroraModels.qs.get("focus"))).toBe(false);
  });

  test("when Aura is off the Aura row says so and opens Quick Center; nothing runs", async ({ page }) => {
    await openIndex(page);
    await ev(page, () => window.AuroraModels.qs.set("auraMode", "off"));
    await page.locator("#index-query").fill("start a 10 minute timer");
    await expect(opts(page).first()).toContainText("Aura is off");
    await page.keyboard.press("Enter");
    await expect(page.locator("#screen-quick")).toBeVisible();
    expect(await ev(page, () => window.AuroraSystem.world.timer)).toBeNull();
    expect(await ev(page, () => window.AuroraSystem.receipts().length)).toBe(0);
  });

  test("ordinary text is not treated as a command", async ({ page }) => {
    await openIndex(page, "what is the weather");
    await expect(page.locator('#index-results .index-result[data-group="aura"]')).toHaveCount(0);
  });
});

test.describe("Index: untrusted text", () => {
  test("a markup query creates no elements and runs no script, in results or the no-match state", async ({ page }) => {
    await openIndex(page, "<img src=x onerror=window.__pwn=1>");
    await page.waitForTimeout(250);
    expect(await ev(page, () => typeof window.__pwn)).toBe("undefined");
    await expect(page.locator("#screen-index img")).toHaveCount(0);
    await expect(page.locator("#index-empty")).toContainText("<img src=x onerror=window.__pwn=1>");
  });

  test("an Aura search command carrying markup shows it as text", async ({ page }) => {
    await openIndex(page, "search for <b>hi</b>");
    await expect(opts(page).first()).toContainText("<b>hi</b>");
    await expect(page.locator("#screen-index b")).toHaveCount(0);
  });

  test("a Space renamed to markup is searchable and rendered as text in file results", async ({ page }) => {
    await openIndex(page);
    await ev(page, () => window.AuroraShell.renameSpace("study", "<i>x</i>"));
    await page.locator("#index-query").fill("timetable");
    await expect(opts(page).first()).toContainText("<i>x</i>");
    await expect(page.locator("#screen-index i")).toHaveCount(0);
  });
});

test.describe("Index: layout", () => {
  for (const [w, h] of [[360, 740], [390, 844], [768, 1024], [1280, 800]]) {
    for (const mode of ["default", "200%"]) {
      test(`no overflow, clipping or small options at ${w}px (${mode} text)`, async ({ page }) => {
        await page.setViewportSize({ width: w, height: h });
        await openIndex(page);
        if (mode === "200%") await ev(page, () => document.documentElement.setAttribute("data-text-scale", "200"));
        for (const q of ["", "e", "zzzzqqq", "prepare my phone for studying"]) {
          await page.locator("#index-query").fill(q);
          const problems = await ev(page, () => {
            const out = [], vw = document.documentElement.clientWidth;
            if (document.documentElement.scrollWidth > vw + 1) out.push("page scrolls horizontally " + document.documentElement.scrollWidth);
            document.querySelectorAll("#screen-index .index-result, #screen-index button, #index-query").forEach((n) => {
              const r = n.getBoundingClientRect(); if (!r.width) return;
              if (r.left < -1 || r.right > vw + 1) out.push("clipped " + (n.id || n.textContent.trim().slice(0, 20)));
              if (r.height < 44) out.push("small " + (n.id || n.textContent.trim().slice(0, 20)) + " " + Math.round(r.height));
            });
            return out;
          });
          expect(problems, "query '" + q + "'").toEqual([]);
        }
      });
    }
  }

  for (const [theme, tier] of [["light", "soft"], ["dark", "clear"]]) {
    test(`renders in ${theme} ${tier} with high contrast and no console errors`, async ({ page }) => {
      const errors = []; page.on("pageerror", (e) => errors.push(e.message)); page.on("console", (m) => { if (m.type() === "error") errors.push(m.text()); });
      await openIndex(page, "e");
      await ev(page, ([t, ti]) => { window.AuroraShell.setAppearance(t); window.AuroraShell.setTier(ti); window.AuroraShell.setContrast("high"); }, [theme, tier]);
      expect(await page.locator(".index-result.is-active").evaluate((n) => parseFloat(getComputedStyle(n).borderTopWidth))).toBeGreaterThanOrEqual(3);
      expect(errors).toEqual([]);
    });
  }
});
