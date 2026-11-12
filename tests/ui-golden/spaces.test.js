/**
 * tests/ui-golden/spaces.test.js — M1 / S4: Spaces.
 * Create, rename and switch go through the AuroraShell Space API; names are validated and rendered as plain text.
 */
const { test, expect } = require("@playwright/test");

const BASE_URL = "http://localhost:3000";
const ev = (page, fn, arg) => page.evaluate(fn, arg);
const card = (page, id) => page.locator('.space-card-item[data-space="' + id + '"]');
const dialog = (page) => page.locator(".dialog");

async function openSpaces(page) { await page.goto(BASE_URL); await page.locator("#dock-spaces").click(); await expect(page.locator("#screen-spaces")).toBeVisible(); }
async function newSpace(page, name) {
  await page.locator("#btn-space-new").click();
  await expect(dialog(page)).toBeVisible();
  await page.locator("#dlg-input").fill(name);
}

test.describe("Spaces: cards", () => {
  test.beforeEach(async ({ page }) => { await openSpaces(page); });

  test("each card shows name, description, pinned apps, real page count and state", async ({ page }) => {
    const study = card(page, "study");
    await expect(study.locator("h3")).toHaveText("Study");
    await expect(study.locator(".space-card-desc")).toHaveText("Notes, focus timer, reading list");
    await expect(study.locator(".space-pins .app-tile")).toHaveCount(7);
    await expect(study.locator(".space-pins")).toHaveAccessibleName("Pinned in Study: Notes, Clock, Calculator, Settings, Calendar, Files, Passport");
    await expect(study.locator(".space-pages")).toHaveText("2 pages open");
    await expect(study).toHaveClass(/selected/);
    await expect(study.locator(".space-switch")).toHaveAttribute("aria-current", "true");
    await expect(study.locator(".space-state")).toHaveText("Active");
    await expect(card(page, "work").locator(".space-pages")).toHaveText("1 page open");
    await expect(card(page, "projects").locator(".space-pages")).toHaveText("0 pages open");
    await expect(card(page, "work").locator(".space-state")).toHaveText("Switch to this Space");
    await expect(card(page, "work").locator(".space-switch")).toHaveAccessibleName("Switch to Work Space");
  });

  test("page counts follow the Pages model", async ({ page }) => {
    await ev(page, () => window.AuroraModels.pages.open("calendar", "Calendar", "projects", 10));
    await expect(card(page, "projects").locator(".space-pages")).toHaveText("1 page open");
  });

  test("clicking anywhere on a card switches to that Space and keeps focus on its switch button", async ({ page }) => {
    await card(page, "work").click({ position: { x: 40, y: 60 } });   // empty part of the card: the stretched switch button receives it
    await expect(card(page, "work")).toHaveClass(/selected/);
    await expect(card(page, "study")).not.toHaveClass(/selected/);
    await expect(page.locator("#toast")).toContainText("Now in Work");
    expect(await ev(page, () => window.AuroraShell.state.activeSpace)).toBe("work");
    await expect(card(page, "work").locator(".space-switch")).toBeFocused();
    await page.locator('[data-fk="space:personal"]').click();
    await expect(page.locator('[data-fk="space:personal"]')).toBeFocused();
  });

  test("keyboard: Tab reaches the switch button, Enter and Space activate it", async ({ page }) => {
    await page.locator('[data-fk="space:projects"]').focus();
    await page.keyboard.press("Enter");
    await expect(card(page, "projects")).toHaveClass(/selected/);
    await expect(page.locator('[data-fk="space:projects"]')).toBeFocused();
    await page.locator('[data-fk="space:personal"]').focus();
    await page.keyboard.press("Space");
    await expect(card(page, "personal")).toHaveClass(/selected/);
  });

  test("the focus ring shows on the whole card, not a tiny button", async ({ page }) => {
    await page.keyboard.press("Tab");
    await page.locator('[data-fk="space:work"]').focus();
    const w = await ev(page, () => { const b = document.querySelector('[data-fk="space:work"]'); const a = getComputedStyle(b, "::after"); return { outline: a.outlineStyle, w: parseFloat(a.width) }; });
    expect(w.outline).toBe("solid");
    expect(w.w).toBeGreaterThan(200);
  });

  test("switching updates Home immediately", async ({ page }) => {
    await card(page, "personal").locator(".space-switch").click();
    await page.locator("#dock-home").click();
    await expect(page.locator("#home-space-name")).toHaveText("Personal");
    await expect(page.locator("#screen-home .space-chip")).toHaveText("Personal");
    expect(await page.locator("#home-pins .pin-btn").evaluateAll((n) => n.map((b) => b.dataset.app))).toEqual(["notes", "calendar", "clock", "settings", "passport"]);
  });

  test("Back returns to where you came from", async ({ page }) => {
    await page.locator("#btn-spaces-back").click();
    await expect(page.locator("#screen-home")).toBeVisible();
  });
});

test.describe("Spaces: New Space", () => {
  test.beforeEach(async ({ page }) => { await openSpaces(page); });

  test("creates a Space through the prompt, adds a card and focuses it", async ({ page }) => {
    await newSpace(page, "  Reading  ");
    await expect(page.getByRole("button", { name: "Create" })).toBeEnabled();
    await page.getByRole("button", { name: "Create" }).click();
    await expect(dialog(page)).toHaveCount(0);
    await expect(page.locator(".space-card-item")).toHaveCount(5);
    await expect(page.locator(".space-card-item h3").last()).toHaveText("Reading");
    await expect(page.locator("#toast")).toContainText("Created Reading");
    await expect(page.locator(".space-card-item").last().locator(".space-switch")).toBeFocused();
    expect(await ev(page, () => window.AuroraShell.spaces().map((s) => s.name))).toContain("Reading");
  });

  test("Enter in the field confirms; Escape cancels and returns focus to the button", async ({ page }) => {
    await newSpace(page, "Garden");
    await page.keyboard.press("Enter");
    await expect(page.locator(".space-card-item h3").last()).toHaveText("Garden");
    await page.locator("#btn-space-new").click();
    await page.locator("#dlg-input").fill("Nope");
    await page.keyboard.press("Escape");
    await expect(dialog(page)).toHaveCount(0);
    await expect(page.locator(".space-card-item")).toHaveCount(5);
    await expect(page.locator("#btn-space-new")).toBeFocused();
  });

  test("the dialog starts quiet with Create disabled, then explains each problem inline", async ({ page }) => {
    await page.locator("#btn-space-new").click();
    await expect(page.locator("#dlg-input")).toBeFocused();
    await expect(page.getByRole("button", { name: "Create" })).toBeDisabled();
    await expect(page.locator(".field-error")).toHaveText("");
    const cases = [["   ", "Enter a name."], ["study", "already exists"], ["STUDY", "already exists"], ["bad\u0007name", "not allowed"], ["evil‮txt", "not allowed"]];
    for (const [text, msg] of cases) {
      await page.locator("#dlg-input").fill(text);
      await expect(page.locator(".field-error")).toContainText(msg);
      await expect(page.locator("#dlg-input")).toHaveAttribute("aria-invalid", "true");
      await expect(page.getByRole("button", { name: "Create" })).toBeDisabled();
    }
    await page.locator("#dlg-input").fill("x".repeat(30));                       // the field itself stops typing at 24
    expect((await page.locator("#dlg-input").inputValue()).length).toBe(24);
    await page.locator("#dlg-input").fill("Fine");
    await expect(page.locator(".field-error")).toHaveText("");
    await expect(page.getByRole("button", { name: "Create" })).toBeEnabled();
  });

  test("a hostile name is created and shown as text everywhere", async ({ page }) => {
    const NAME = "<img src=x onerror=1>";
    await newSpace(page, NAME);
    await page.getByRole("button", { name: "Create" }).click();
    await expect(page.locator("#screen-spaces h3").last()).toHaveText(NAME);
    await expect(page.locator("#screen-spaces img")).toHaveCount(0);
    await page.locator(".space-card-item").last().locator(".space-switch").click();
    await page.locator("#dock-home").click();
    await expect(page.locator("#home-space-name")).toHaveText(NAME);
    await expect(page.locator("#screen-home img")).toHaveCount(0);
    await expect(page.locator("#screen-home .space-chip")).toHaveText(NAME);
  });

  test("the eighth Space is allowed; the ninth is refused with a message and no dialog", async ({ page }) => {
    await ev(page, () => { for (let i = 1; i <= 3; i++) window.AuroraShell.addSpace("S" + i); });
    await newSpace(page, "Eighth");
    await page.getByRole("button", { name: "Create" }).click();
    await expect(page.locator(".space-card-item")).toHaveCount(8);
    await page.locator("#btn-space-new").click();
    await expect(dialog(page)).toHaveCount(0);
    await expect(page.locator("#toast")).toContainText("Up to 8 Spaces");
    await expect(page.locator(".space-card-item")).toHaveCount(8);
    expect(await ev(page, () => window.AuroraShell.addSpace("Ninth").ok)).toBe(false);   // the API refuses too
  });
});

test.describe("Spaces: rename", () => {
  test.beforeEach(async ({ page }) => { await openSpaces(page); });

  test("opens prefilled, renames, and updates the card, Home and the active chip", async ({ page }) => {
    await page.getByRole("button", { name: "Rename Study" }).click();
    await expect(page.locator("#dlg-input")).toHaveValue("Study");
    await expect(page.getByRole("button", { name: "Rename", exact: true })).toBeEnabled();   // unchanged name is valid
    await page.locator("#dlg-input").fill("Exams");
    await page.getByRole("button", { name: "Rename", exact: true }).click();
    await expect(card(page, "study").locator("h3")).toHaveText("Exams");
    await expect(page.locator("#toast")).toContainText("Renamed to Exams");
    await expect(page.locator("#screen-home .space-chip")).toHaveText("Exams");
    await expect(card(page, "study").locator(".space-switch")).toHaveAccessibleName("Exams Space, active");
  });

  test("Rename does not switch Spaces", async ({ page }) => {
    await page.getByRole("button", { name: "Rename Work" }).click();
    await page.keyboard.press("Escape");
    expect(await ev(page, () => window.AuroraShell.state.activeSpace)).toBe("study");
    await expect(page.getByRole("button", { name: "Rename Work" })).toBeFocused();
  });

  test("duplicates are refused ignoring case, but changing your own name's case is fine", async ({ page }) => {
    await page.getByRole("button", { name: "Rename Work" }).click();
    await page.locator("#dlg-input").fill("PROJECTS");
    await expect(page.locator(".field-error")).toContainText("already exists");
    await expect(page.getByRole("button", { name: "Rename", exact: true })).toBeDisabled();
    await page.locator("#dlg-input").fill("WORK");
    await expect(page.getByRole("button", { name: "Rename", exact: true })).toBeEnabled();
    await page.getByRole("button", { name: "Rename", exact: true }).click();
    await expect(card(page, "work").locator("h3")).toHaveText("WORK");
  });
});

test.describe("Spaces: layout and appearance", () => {
  for (const [w, h] of [[360, 740], [390, 844], [768, 1024], [1280, 800], [1440, 900]]) {
    for (const mode of ["default", "200%"]) {
      test(`no overflow, clipping or small targets at ${w}px (${mode} text, 8 Spaces, long names)`, async ({ page }) => {
        await page.setViewportSize({ width: w, height: h });
        await openSpaces(page);
        await ev(page, () => { const S = window.AuroraShell; ["A very long space name here", "Another extremely long one", "WWWWWWWWWWWWWWWWWWWWWWWW"].forEach((n) => S.addSpace(n)); S.addSpace("Four"); });
        if (mode === "200%") await ev(page, () => document.documentElement.setAttribute("data-text-scale", "200"));
        const problems = await ev(page, () => {
          const out = [], vw = document.documentElement.clientWidth;
          if (document.documentElement.scrollWidth > vw + 1) out.push("page scrolls horizontally " + document.documentElement.scrollWidth);
          document.querySelectorAll("#screen-spaces button").forEach((b) => {
            const r = b.getBoundingClientRect(); if (!r.width) return;
            if (r.left < -1 || r.right > vw + 1) out.push("clipped " + (b.id || b.getAttribute("aria-label")));
            if (Math.min(r.width, r.height) < 44) out.push("small target " + (b.id || b.getAttribute("aria-label")) + " " + Math.round(r.width) + "x" + Math.round(r.height));
          });
          document.querySelectorAll("#screen-spaces .space-card-item").forEach((c) => { const r = c.getBoundingClientRect(); if (r.right > vw + 1) out.push("card clipped " + c.dataset.space); });
          return out;
        });
        expect(problems).toEqual([]);
      });
    }
  }

  for (const [theme, tier] of [["light", "soft"], ["dark", "clear"]]) {
    test(`renders in ${theme} ${tier} with no console errors, and the active card is distinguishable without colour`, async ({ page }) => {
      const errors = []; page.on("pageerror", (e) => errors.push(e.message)); page.on("console", (m) => { if (m.type() === "error") errors.push(m.text()); });
      await openSpaces(page);
      await ev(page, ([t, ti]) => { window.AuroraShell.setAppearance(t); window.AuroraShell.setTier(ti); window.AuroraShell.setContrast("high"); }, [theme, tier]);
      await expect(card(page, "study").locator(".space-state")).toHaveText("Active");     // text, not just an outline
      await expect(card(page, "study").locator(".space-switch svg.icon")).toHaveCount(1); // and an icon
      expect(await card(page, "study").evaluate((c) => parseFloat(getComputedStyle(c).borderTopWidth))).toBeGreaterThanOrEqual(3);
      expect(errors).toEqual([]);
    });
  }

  test("desktop shows several columns; phone shows one", async ({ page }) => {
    await page.setViewportSize({ width: 1280, height: 800 });
    await openSpaces(page);
    const xs = await page.locator(".space-card-item").evaluateAll((n) => new Set(n.map((c) => Math.round(c.getBoundingClientRect().left))).size);
    expect(xs).toBeGreaterThanOrEqual(3);
    await page.setViewportSize({ width: 390, height: 844 });
    const one = await page.locator(".space-card-item").evaluateAll((n) => new Set(n.map((c) => Math.round(c.getBoundingClientRect().left))).size);
    expect(one).toBe(1);
  });
});

test.describe("Spaces: a renamed Space is renamed everywhere", () => {
  test("Pages, the Aura plan and its editor use the live name", async ({ page }) => {
    await page.goto(BASE_URL);
    await ev(page, () => window.AuroraShell.renameSpace("study", "Exams"));
    await page.locator("#dock-pages").click();
    await expect(page.locator('[data-page="page-notes"] .page-meta')).toContainText("Exams Space");
    await ev(page, () => window.AuroraShell.showScreen("screen-home"));
    await page.locator("#btn-aura-study").click();
    await expect(page.locator("#screen-aura")).toContainText("Switch to Exams Space");
    await page.locator("#btn-plan-edit").click();
    await expect(page.locator("#screen-aura select option", { hasText: "Exams" })).toHaveCount(1);
    expect(await ev(page, () => window.AuroraSystem.describe({ type: "space.switch", arguments: { space: "study" } }).title)).toBe("Switch to Exams Space");
    expect(await ev(page, () => window.AuroraSystem.describe({ type: "file.move", arguments: { id: "timetable", destination: "study" } }).title)).toBe("Move “Exam timetable.txt” to Exams");
  });
});
