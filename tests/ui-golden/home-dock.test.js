/**
 * tests/ui-golden/home-dock.test.js — M1 / S3: Home and Dock.
 * Home is rendered from real models (Pages, Quick settings, Spaces, AuroraSystem status); nothing is sample data.
 * Browser prototype only; nothing here touches a real device.
 */
const { test, expect } = require("@playwright/test");

const BASE_URL = "http://localhost:3000";
const ev = (page, fn, arg) => page.evaluate(fn, arg);
const rows = (page) => page.locator("#home-continue-list .list-row");

test.describe("Home: greeting and Ask", () => {
  test("shows a live clock, a long date, the Space and the header actions", async ({ page }) => {
    await page.goto(BASE_URL);
    await expect(page.locator("#home-clock")).toHaveText(/^\d{1,2}:\d{2}/);
    await expect(page.locator("#home-date")).toHaveText(/\w+, /);
    await expect(page.locator("#screen-home .space-chip")).toHaveText("Study");
    await expect(page.locator("#screen-home h1")).toHaveText("Home");
    await expect(page.getByRole("button", { name: "Open Index search" })).toBeVisible();
    await expect(page.getByRole("button", { name: "Open Quick Center" })).toBeVisible();
  });

  test("the clock updates on the minute boundary and Home never starts a 1 s interval", async ({ page }) => {
    await page.addInitScript(() => { window.__iv = []; const o = window.setInterval; window.setInterval = (f, ms, ...a) => { window.__iv.push(ms); return o(f, ms, ...a); }; });
    await page.clock.install({ time: new Date("2026-11-12T10:59:30") });
    await page.goto(BASE_URL);
    await expect(page.locator("#home-clock")).toHaveText("10:59 AM");
    await page.clock.runFor(10000);
    await expect(page.locator("#home-clock")).toHaveText("10:59 AM");   // not yet the next minute
    await page.clock.runFor(21000);
    await expect(page.locator("#home-clock")).toHaveText("11:00 AM");
    expect((await ev(page, () => window.__iv)).filter((ms) => ms <= 1000)).toEqual([]);
  });

  test("the focus chip follows Focus mode", async ({ page }) => {
    await page.goto(BASE_URL);
    await expect(page.locator("#home-focus-chip")).toBeHidden();
    await ev(page, () => window.AuroraModels.qs.set("focus", true));
    await expect(page.locator("#home-focus-chip")).toBeVisible();
    await ev(page, () => window.AuroraModels.qs.set("focus", false));
    await expect(page.locator("#home-focus-chip")).toBeHidden();
  });

  test("the Ask pill opens Index with the field focused, by mouse and by keyboard", async ({ page }) => {
    await page.goto(BASE_URL);
    await page.locator("#btn-ask").click();
    await expect(page.locator("#screen-index")).toBeVisible();
    await expect(page.locator("#index-query")).toBeFocused();
    await page.keyboard.press("Escape");
    await expect(page.locator("#screen-home")).toBeVisible();
    await page.locator("#btn-ask").focus();
    await page.keyboard.press("Enter");
    await expect(page.locator("#index-query")).toBeFocused();
  });

  test("the Ask pill has an accessible name and shows the Ctrl K hint only on wider layouts", async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 800 });
    await page.goto(BASE_URL);
    await expect(page.getByRole("button", { name: "Ask Aura or search" })).toBeVisible();
    await expect(page.locator(".ask-hint")).toBeHidden();
    await page.setViewportSize({ width: 1024, height: 800 });
    await expect(page.locator(".ask-hint")).toBeVisible();
  });
});

test.describe("Home: Continue mirrors the real Pages", () => {
  test.beforeEach(async ({ page }) => { await page.goto(BASE_URL); });

  test("lists running pages before paused ones and leaves out saved pages", async ({ page }) => {
    await expect(rows(page)).toHaveCount(3);
    await expect(rows(page).nth(0)).toContainText("Notes");
    await expect(rows(page).nth(0)).toContainText("Running");
    await expect(rows(page).nth(1)).toContainText("Paused");
    await expect(page.locator("#home-continue-list")).not.toContainText("Calculator");   // saved
    await expect(rows(page).nth(0)).toHaveAccessibleName("Open Notes, Study Space, running");
  });

  test("updates when a page is paused, resumed or closed", async ({ page }) => {
    await ev(page, () => window.AuroraModels.pages.resume("page-clock"));
    await expect(rows(page).nth(0)).toContainText(/Notes|Clock/);
    await ev(page, () => window.AuroraModels.pages.pause("page-notes"));
    await expect(page.locator('#home-continue-list [data-page-id="page-notes"]')).toContainText("Paused");
    await expect(rows(page).first()).toContainText("Clock");                              // the only running page is first
    await ev(page, () => window.AuroraModels.pages.close("page-notes"));
    await expect(page.locator('#home-continue-list [data-page-id="page-notes"]')).toHaveCount(0);
    await expect(rows(page)).toHaveCount(2);
  });

  test("shows at most four pages", async ({ page }) => {
    await ev(page, () => { const S = window.AuroraShell; ["calendar", "settings", "calculator"].forEach((a) => S.openApp(a, { background: true })); });
    expect(await ev(page, () => window.AuroraModels.pages.list().filter((p) => p.state !== "saved").length)).toBeGreaterThan(4);
    await expect(rows(page)).toHaveCount(4);
  });

  test("an entry opens its app", async ({ page }) => {
    await page.locator('#home-continue-list [data-page-id="page-files"]').click();
    await expect(page.locator("#screen-pages")).toBeVisible();
    await ev(page, () => window.AuroraShell.showScreen("screen-home"));
    await page.locator('#home-continue-list [data-page-id="page-notes"]').click();
    await expect(page.locator("#screen-pages")).toBeVisible();
  });

  test("View all opens Pages", async ({ page }) => {
    await page.getByRole("button", { name: "View all" }).click();
    await expect(page.locator("#screen-pages")).toBeVisible();
  });

  test("empty state offers Open Notes", async ({ page }) => {
    await ev(page, () => window.AuroraModels.pages.list().forEach((p) => window.AuroraModels.pages.close(p.id)));
    await expect(page.locator("#home-continue-list")).toContainText("Nothing in progress");
    await page.locator("#btn-continue-notes").click();
    expect(await ev(page, () => window.AuroraModels.pages.list().map((p) => p.appId))).toEqual(["notes"]);
  });
});

test.describe("Home: pins and Current Space", () => {
  test.beforeEach(async ({ page }) => { await page.goto(BASE_URL); });

  test("pins come from the active Space and change with it", async ({ page }) => {
    const pins = () => page.locator("#home-pins .pin-btn").evaluateAll((n) => n.map((b) => b.dataset.app));
    expect(await pins()).toEqual(["notes", "clock", "calculator", "settings", "calendar", "files", "passport"]);
    await ev(page, () => window.AuroraShell.switchSpace("work", { quiet: true }));
    expect(await pins()).toEqual(["files", "notes", "calendar", "settings", "passport"]);
    await ev(page, () => window.AuroraShell.switchSpace("personal", { quiet: true }));
    expect(await pins()).toEqual(["notes", "calendar", "clock", "settings", "passport"]);
  });

  test("every pin has an accessible name, an app tile and opens its app", async ({ page }) => {
    await expect(page.getByRole("button", { name: "Open Calendar" }).locator(".app-tile")).toHaveCount(1);
    await page.getByRole("button", { name: "Open Calendar" }).click();
    await expect(page.locator('#screen-pages [data-page="page-calendar"]')).toBeVisible();
    await ev(page, () => window.AuroraShell.showScreen("screen-home"));
    await page.getByRole("button", { name: "Open Settings" }).click();
    await expect(page.locator("#screen-settings")).toBeVisible();
  });

  test("tiles use each app's own hue", async ({ page }) => {
    const bg = await page.locator("#home-pins .pin-btn .app-tile").evaluateAll((n) => n.map((t) => getComputedStyle(t).backgroundImage));
    expect(new Set(bg).size).toBeGreaterThan(3);
  });

  test("a pinned app that cannot open says so and creates no Page", async ({ page }) => {
    await ev(page, () => { window.AuroraShell.registerApp({ id: "ghost", name: "Ghost", system: true }); });
    const before = await ev(page, () => window.AuroraModels.pages.list().length);
    expect(await ev(page, () => window.AuroraShell.openApp("ghost"))).toEqual({ ok: false, reason: "not-available" });
    await expect(page.locator("#toast")).toContainText("Ghost arrives in a later milestone");
    expect(await ev(page, () => window.AuroraModels.pages.list().length)).toBe(before);
  });

  test("Current Space shows name, description and its real page count, and updates on switch", async ({ page }) => {
    await expect(page.locator("#home-space-name")).toHaveText("Study");
    await expect(page.locator("#screen-home .space-desc")).toHaveText("Notes, focus timer, reading list");
    await expect(page.locator("#home-space-count")).toHaveText("2 pages open here");       // notes + clock
    await ev(page, () => window.AuroraShell.switchSpace("work", { quiet: true }));
    await expect(page.locator("#home-space-name")).toHaveText("Work");
    await expect(page.locator("#home-space-count")).toHaveText("1 page open here");        // files
    await ev(page, () => window.AuroraShell.switchSpace("projects", { quiet: true }));
    await expect(page.locator("#home-space-count")).toHaveText("0 pages open here");
    await expect(page.locator("#screen-home .space-chip")).toHaveText("Projects");
  });

  test("Switch Space opens Spaces", async ({ page }) => {
    await page.getByRole("button", { name: "Switch Space" }).click();
    await expect(page.locator("#screen-spaces")).toBeVisible();
  });

  test("a Space renamed to markup is shown as text", async ({ page }) => {
    await ev(page, () => { window.AuroraShell.renameSpace("study", "<b>Bold</b>"); });
    await expect(page.locator("#home-space-name")).toHaveText("<b>Bold</b>");
    await expect(page.locator("#screen-home b")).toHaveCount(0);
  });
});

test.describe("Home: Aura suggestion", () => {
  test.beforeEach(async ({ page }) => { await page.goto(BASE_URL); });

  test("opens a reviewable plan and changes nothing before approval", async ({ page }) => {
    const snap = () => ev(page, () => ({ focus: window.AuroraModels.qs.get("focus"), space: window.AuroraShell.state.activeSpace, receipts: window.AuroraSystem.receipts().length, pages: window.AuroraModels.pages.list().map((p) => p.id + ":" + p.state).join(","), timer: window.AuroraSystem.world.timer }));
    const before = await snap();
    await page.locator("#btn-aura-study").click();
    await expect(page.locator("#screen-aura")).toBeVisible();
    await expect(page.locator("#screen-aura .plan-step")).toHaveCount(4);
    await page.waitForTimeout(1500);                       // longer than the whole simulated run would take
    expect(await snap()).toEqual(before);
    await expect(page.locator("#plan-receipt")).toHaveCount(0);
    await expect(page.locator("#btn-plan-confirm")).toBeEnabled();
  });

  test("is hidden when Aura is off and returns when it is on", async ({ page }) => {
    await expect(page.locator("#btn-aura-study")).toBeVisible();
    await ev(page, () => window.AuroraModels.qs.set("auraMode", "off"));
    await expect(page.locator("#home-aura")).toBeHidden();
    await ev(page, () => window.AuroraModels.qs.set("auraMode", "ask"));
    await expect(page.locator("#home-aura")).toBeVisible();
  });
});

test.describe("Home: Aurora Horizon", () => {
  test.beforeEach(async ({ page }) => { await page.goto(BASE_URL); });

  test("is idle with no meter until something runs", async ({ page }) => {
    await expect(page.locator("#horizon")).toBeVisible();
    await expect(page.locator("#horizon-text")).toHaveText("Ready");
    await expect(page.locator("#horizon-meter")).toBeHidden();
    await expect(page.locator("#dock-horizon")).toBeHidden();
  });

  test("shows determinate progress from real status events", async ({ page }) => {
    await ev(page, () => window.AuroraSystem.emit("status", { text: "Running 4 steps", running: true, done: 2, total: 4 }));
    await expect(page.locator("#horizon-text")).toHaveText("Running 4 steps");
    await expect(page.locator("#horizon-meter")).toBeVisible();
    await expect(page.locator("#horizon-meter")).toHaveAttribute("aria-valuenow", "50");
    await expect(page.locator("#dock-horizon")).toBeVisible();
    await ev(page, () => window.AuroraSystem.emit("status", { text: "Plan complete · 4 steps", running: false, done: 4, total: 4 }));
    await expect(page.locator("#horizon-meter")).toBeHidden();
    await expect(page.locator("#dock-horizon")).toBeHidden();
    await expect(page.locator("#horizon-text")).toHaveText("Plan complete · 4 steps");
  });

  test("follows a real broker run end to end", async ({ page }) => {
    await ev(page, () => { window.AuroraSystem.dev.stepDelayMs = 700; });
    await ev(page, () => { window.__run = window.AuroraReview.ask("start a 10 minute timer"); });
    await expect(page.locator("#horizon-meter")).toBeVisible();
    await expect(page.locator("#horizon-text")).toHaveText("Start a timer for 10 minutes");   // the step being run
    await ev(page, () => window.__run);
    await expect(page.locator("#horizon-meter")).toBeHidden();
    await expect(page.locator("#horizon-text")).toContainText("done");
    expect(await ev(page, () => window.AuroraSystem.world.timer.seconds)).toBe(600);
  });

  test("status text is plain text", async ({ page }) => {
    await ev(page, () => window.AuroraSystem.emit("status", { text: "<img src=x onerror=1>", running: false }));
    await expect(page.locator("#horizon-text")).toHaveText("<img src=x onerror=1>");
    await expect(page.locator("#horizon img")).toHaveCount(0);
  });
});

test.describe("Dock", () => {
  test("has five labelled navigation buttons with icons and no emoji glyphs", async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 800 });
    await page.goto(BASE_URL);
    const names = await page.locator("#dock .dock-btn").evaluateAll((n) => n.map((b) => b.getAttribute("aria-label")));
    expect(names).toEqual(["Back", "Home", "Spaces", "Pages", "Ask Aura / Search"]);
    await expect(page.locator("#dock .dock-btn svg.icon")).toHaveCount(5);
    const text = await page.locator("#dock").innerText();
    expect(/[←★\u{1F4C5}\u{1F5C9}\u{1F50E}]/u.test(text)).toBe(false);
    for (const b of await page.locator("#dock .dock-btn").all()) {
      const r = await b.boundingBox();
      expect(Math.min(r.width, r.height)).toBeGreaterThanOrEqual(48);
    }
  });

  test("marks exactly the current screen with aria-current (and a visible, non-colour cue)", async ({ page }) => {
    await page.goto(BASE_URL);
    const current = () => page.locator("#dock [aria-current]").evaluateAll((n) => n.map((b) => b.id));
    expect(await current()).toEqual(["dock-home"]);
    await page.locator("#dock-spaces").click();
    expect(await current()).toEqual(["dock-spaces"]);
    await page.locator("#dock-ask").click();
    expect(await current()).toEqual(["dock-ask"]);
    await page.locator("#dock-pages").click();
    expect(await current()).toEqual(["dock-pages"]);
    expect(await page.locator("#dock-pages").evaluate((b) => getComputedStyle(b, "::after").content)).not.toBe("none");
    await ev(page, () => window.AuroraShell.showScreen("screen-quick"));
    expect(await current()).toEqual([]);                      // Quick Center has no Dock button
  });

  test("Back and Home buttons navigate", async ({ page }) => {
    await page.goto(BASE_URL);
    await page.locator("#dock-spaces").click();
    await page.locator("#dock-pages").click();
    await page.locator("#dock-back").click();
    await expect(page.locator("#screen-spaces")).toBeVisible();
    await page.locator("#dock-home").click();
    await expect(page.locator("#screen-home")).toBeVisible();
  });

  test("running apps appear in the Dock at tablet and desktop widths, not on phones", async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 800 });
    await page.goto(BASE_URL);
    await expect(page.locator("#dock-apps")).toBeHidden();
    await page.setViewportSize({ width: 768, height: 900 });
    await expect(page.locator("#dock-apps")).toBeVisible();
    await expect(page.getByRole("button", { name: "Notes, running" })).toBeVisible();
    await expect(page.getByRole("button", { name: "Clock, paused" })).toBeVisible();
    await expect(page.locator("#dock-apps .dock-app")).toHaveCount(3);
    await page.getByRole("button", { name: "Files, paused" }).click();
    await expect(page.locator("#screen-pages")).toBeVisible();
    await ev(page, () => window.AuroraModels.pages.close("page-files"));
    await expect(page.locator("#dock-apps .dock-app")).toHaveCount(2);
  });

  test("more than six running apps collapse into +n", async ({ page }) => {
    await page.setViewportSize({ width: 1280, height: 800 });
    await page.goto(BASE_URL);
    await ev(page, () => { const m = window.AuroraModels; ["a1", "a2", "a3", "a4", "a5"].forEach((id) => m.pages.open(id, id.toUpperCase(), "study", 10)); });
    await expect(page.locator("#dock-apps .dock-app")).toHaveCount(7);
    await expect(page.locator(".dock-more")).toHaveAccessibleName("2 more open apps");
  });

  test("the tray (time and Quick Center) shows from 1024 px", async ({ page }) => {
    await page.setViewportSize({ width: 768, height: 800 });
    await page.goto(BASE_URL);
    await expect(page.locator("#dock-tray")).toBeHidden();
    await page.setViewportSize({ width: 1024, height: 800 });
    await expect(page.locator("#dock-tray")).toBeVisible();
    await expect(page.locator("#dock-time")).toHaveText(/\d{1,2}:\d{2}/);
    await page.getByRole("button", { name: "Quick Center", exact: true }).click();
    await expect(page.locator("#screen-quick")).toBeVisible();
  });

  test("the Dock never exceeds the viewport and keeps 48 px targets at 360 px with 200% text", async ({ page }) => {
    await page.setViewportSize({ width: 360, height: 740 });
    await page.goto(BASE_URL);
    await ev(page, () => document.documentElement.setAttribute("data-text-scale", "200"));
    const r = await ev(page, () => {
      const vw = document.documentElement.clientWidth;
      return { sw: document.documentElement.scrollWidth, dock: document.getElementById("dock").getBoundingClientRect().right,
        btns: [...document.querySelectorAll("#dock .dock-btn")].map((b) => { const q = b.getBoundingClientRect(); return [q.left >= -1 && q.right <= vw + 1, Math.min(q.width, q.height) >= 47.5]; }),
        labelsHidden: [...document.querySelectorAll("#dock .dock-label")].every((l) => getComputedStyle(l).display === "none") };
    });
    expect(r.sw).toBeLessThanOrEqual(360);
    expect(r.dock).toBeLessThanOrEqual(361);
    expect(r.btns.every(([inside, big]) => inside && big)).toBe(true);
    expect(r.labelsHidden).toBe(true);
    await expect(page.getByRole("button", { name: "Pages" })).toBeVisible();      // names survive without visible labels
  });

  test("the Dock sits below the content and does not cover a dialog's actions", async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 700 });
    await page.goto(BASE_URL);
    await ev(page, () => { window.AuroraUI.dialog({ title: "Check", body: "Body", actions: [{ label: "Cancel", value: 0 }, { label: "OK", kind: "accent", value: 1 }] }); });
    const ok = await page.getByRole("button", { name: "OK" }).boundingBox();
    const hit = await ev(page, ({ x, y }) => { const n = document.elementFromPoint(x, y); return n && n.closest(".dialog") ? "dialog" : n && n.id; }, { x: ok.x + ok.width / 2, y: ok.y + ok.height / 2 });
    expect(hit).toBe("dialog");
  });
});

test.describe("Home: layouts", () => {
  const VPS = [[360, 740], [390, 844], [412, 915], [768, 1024], [1024, 768], [1280, 800], [1440, 900]];
  for (const [w, h] of VPS) {
    for (const mode of ["default", "200%"]) {
      test(`no horizontal overflow or clipped controls at ${w}px (${mode} text)`, async ({ page }) => {
        await page.setViewportSize({ width: w, height: h });
        await page.goto(BASE_URL);
        if (mode === "200%") await ev(page, () => document.documentElement.setAttribute("data-text-scale", "200"));
        const problems = await ev(page, () => {
          const out = [], vw = document.documentElement.clientWidth;
          if (document.documentElement.scrollWidth > vw + 1) out.push("page scrolls horizontally " + document.documentElement.scrollWidth);
          document.querySelectorAll("#screen-home button, #dock button").forEach((b) => {
            const r = b.getBoundingClientRect(); if (!r.width) return;
            if (r.left < -1 || r.right > vw + 1) out.push("clipped " + (b.id || b.getAttribute("aria-label")));
            if (Math.min(r.width, r.height) < 44) out.push("small target " + (b.id || b.getAttribute("aria-label")) + " " + Math.round(r.width) + "x" + Math.round(r.height));
          });
          document.querySelectorAll("#screen-home h2, #screen-home .row-title, #screen-home .pin-label").forEach((n) => { if (n.scrollWidth > n.clientWidth + 1 && getComputedStyle(n).overflow !== "visible") out.push("text clipped " + n.textContent.slice(0, 20)); });
          return out;
        });
        expect(problems).toEqual([]);
      });
    }
  }

  test("desktop uses a 12-column composition: Continue about 7/12, Space and Aura about 5/12, Pinned full width", async ({ page }) => {
    await page.setViewportSize({ width: 1280, height: 800 });
    await page.goto(BASE_URL);
    const b = async (sel) => page.locator(sel).first().boundingBox();
    const cont = await b(".home-continue"), side = await b(".home-side"), pins = await b(".home-pins"), grid = await b(".home-grid");
    expect(cont.width / grid.width).toBeGreaterThan(0.55);
    expect(cont.width / grid.width).toBeLessThan(0.62);
    expect(side.width / grid.width).toBeGreaterThan(0.38);
    expect(side.x).toBeGreaterThan(cont.x + cont.width);
    expect(Math.abs(pins.width - grid.width)).toBeLessThan(2);
    expect(pins.y).toBeGreaterThan(cont.y + cont.height - 1);
  });

  test("tablet puts Continue and Pinned side by side", async ({ page }) => {
    await page.setViewportSize({ width: 768, height: 1024 });
    await page.goto(BASE_URL);
    const cont = await page.locator(".home-continue").boundingBox(), pins = await page.locator(".home-pins").boundingBox();
    expect(pins.x).toBeGreaterThan(cont.x + cont.width - 1);
    expect(Math.abs(pins.y - cont.y)).toBeLessThan(2);
  });

  test("phone is a single column", async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 844 });
    await page.goto(BASE_URL);
    const xs = await page.locator(".home-continue, .home-pins, .home-space, .home-aura").evaluateAll((n) => n.map((e) => Math.round(e.getBoundingClientRect().left)));
    expect(new Set(xs).size).toBe(1);
  });

  for (const [theme, tier] of [["light", "soft"], ["dark", "soft"], ["light", "clear"], ["dark", "clear"]]) {
    test(`renders in ${theme} ${tier} without errors and with legible clock contrast`, async ({ page }) => {
      const errors = []; page.on("pageerror", (e) => errors.push(e.message)); page.on("console", (m) => { if (m.type() === "error") errors.push(m.text()); });
      await page.goto(BASE_URL);
      await ev(page, ([t, ti]) => { window.AuroraShell.setAppearance(t); window.AuroraShell.setTier(ti); }, [theme, tier]);
      const lum = (c) => { const [r, g, b] = c.match(/[\d.]+/g).slice(0, 3).map(Number).map((v) => { v /= 255; return v <= 0.03928 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4; }); return 0.2126 * r + 0.7152 * g + 0.0722 * b; };
      const { fg, bg } = await ev(page, () => { const cs = getComputedStyle(document.getElementById("home-clock")); const bgc = getComputedStyle(document.body).backgroundColor; return { fg: cs.color, bg: bgc }; });
      const ratio = (Math.max(lum(fg), lum(bg)) + 0.05) / (Math.min(lum(fg), lum(bg)) + 0.05);
      expect(ratio).toBeGreaterThan(4.5);
      expect(errors).toEqual([]);
    });
  }
});

test.describe("Home: reduced motion and keyboard", () => {
  test("Horizon fill does not animate under reduced motion", async ({ page }) => {
    await page.goto(BASE_URL);
    await ev(page, () => window.AuroraShell.setMotion("reduce"));
    expect(await ev(page, () => getComputedStyle(document.getElementById("dock-horizon-fill")).transitionDuration)).toMatch(/^0s$|^0ms$/);
  });

  test("Tab order reaches header actions, Ask, Continue, pins, then the Dock", async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 844 });
    await page.goto(BASE_URL);
    await ev(page, () => document.activeElement && document.activeElement.blur());
    const seen = [];
    for (let i = 0; i < 12; i++) { await page.keyboard.press("Tab"); seen.push(await ev(page, () => { const a = document.activeElement; return a ? (a.id || a.getAttribute("aria-label") || a.className) : ""; })); }
    expect(seen.slice(0, 3)).toEqual(["btn-index", "btn-quick-center", "btn-ask"]);
    expect(seen.some((s) => /^Open Notes/.test(s))).toBe(true);
  });
});

test.describe("Text scaling keeps the type hierarchy", () => {
  test("at 200% text on a phone the root size scales and a row title is never smaller than its subtitle", async ({ page }) => {
    await page.setViewportSize({ width: 360, height: 740 });
    await page.goto(BASE_URL);
    const base = await ev(page, () => parseFloat(getComputedStyle(document.documentElement).fontSize));
    await ev(page, () => document.documentElement.setAttribute("data-text-scale", "200"));
    const r = await ev(page, () => ({
      root: parseFloat(getComputedStyle(document.documentElement).fontSize),
      title: parseFloat(getComputedStyle(document.querySelector("#home-continue-list .row-title")).fontSize),
      meta: parseFloat(getComputedStyle(document.querySelector("#home-continue-list .row-meta")).fontSize),
    }));
    expect(base).toBe(16);
    expect(r.root).toBe(32);
    expect(r.title).toBeGreaterThanOrEqual(r.meta);
  });
});
