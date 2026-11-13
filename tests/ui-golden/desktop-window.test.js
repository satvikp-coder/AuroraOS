/**
 * tests/ui-golden/desktop-window.test.js - M8-B: desktop window card.
 * At >= 1024px an app screen is presented as a centred window: one shared title bar (#window-bar: app tile, name, Close on the left) joined to the
 * screen. Close = Shell back() (history preserved). Not a window manager: no drag, no resize, the Pages model is untouched. Phones and tablets keep
 * the edge-to-edge screens, and the bar is then absent from the accessibility tree.
 */
const { test, expect } = require("@playwright/test");

const BASE_URL = "http://localhost:3000";
const ev = (page, fn, arg) => page.evaluate(fn, arg);
const APPS = [["notes", "Notes", "screen-notes"], ["calculator", "Calculator", "screen-calculator"], ["clock", "Clock", "screen-clock"], ["files", "Files", "screen-vault"],
  ["vault", "Vault", "screen-vault"], ["passport", "Passport", "screen-passport"], ["trail", "Trail", "screen-trail"], ["settings", "Settings", "screen-settings"]];
const shellScreens = ["screen-home", "screen-spaces", "screen-pages", "screen-quick", "screen-aura"];

async function start(page) {
  const errors = [];
  page.on("console", (m) => { if (m.type() === "error") errors.push(m.text()); });
  page.on("pageerror", (e) => errors.push(String(e)));
  page.__errors = errors;
  await page.goto(BASE_URL);
}
const bar = (page) => page.locator("#window-bar");
const active = (page) => ev(page, () => window.AuroraShell.state.activeScreen);

test.describe("Desktop window card (1280)", () => {
  test.use({ viewport: { width: 1280, height: 800 } });
  for (const [app, name, screen] of APPS) {
    test(`${name}: title bar with app tile and name, Close button, joined to the screen`, async ({ page }) => {
      await start(page);
      await ev(page, (a) => window.AuroraShell.openApp(a), app);
      await expect(page.locator("#" + screen)).toHaveClass(/active/);
      await expect(bar(page)).toBeVisible();
      await expect(bar(page)).toHaveAttribute("aria-label", name + " window");
      await expect(page.locator("#window-bar-title")).toHaveText(name);
      await expect(page.locator("#window-bar .app-tile")).toBeVisible();
      await expect(page.getByRole("button", { name: "Close " + name })).toBeVisible();
      const [b, s] = await Promise.all([bar(page).boundingBox(), page.locator("#" + screen).boundingBox()]);
      expect(Math.abs(b.x - s.x)).toBeLessThan(1.5); expect(Math.abs(b.width - s.width)).toBeLessThan(1.5);          // same column
      expect(Math.abs(b.y + b.height - s.y)).toBeLessThan(2);                                                         // joined, no gap
      expect(b.x).toBeGreaterThan(40);                                                                                // a window, not edge to edge
      await expect(page.locator("#" + screen)).toHaveClass(/is-window/);
      expect(page.__errors).toEqual([]);
    });
  }
  test("shell screens (Home, Spaces, Pages, Quick Center, Aura) are not windows", async ({ page }) => {
    await start(page);
    for (const id of shellScreens) {
      await ev(page, (s) => window.AuroraShell.showScreen(s), id);
      await expect(bar(page)).toBeHidden();
      await expect(page.locator("#" + id)).not.toHaveClass(/is-window/);
    }
  });
  test("Close goes Back and keeps history: Home, Notes, Settings; Close returns to Notes, Close again to Home", async ({ page }) => {
    await start(page);
    await ev(page, () => { window.AuroraShell.openApp("notes"); window.AuroraShell.openApp("settings"); });
    expect(await active(page)).toBe("screen-settings");
    await page.getByRole("button", { name: "Close Settings" }).click();
    expect(await active(page)).toBe("screen-notes");
    await expect(page.locator("#window-bar-title")).toHaveText("Notes");                                              // the bar follows the visible screen
    await page.getByRole("button", { name: "Close Notes" }).click();
    expect(await active(page)).toBe("screen-home");
    await expect(bar(page)).toBeHidden();
  });
  test("Close does not close the Page of the app: the Pages model is untouched, and the app reopens", async ({ page }) => {
    await start(page);
    await ev(page, () => window.AuroraShell.openApp("notes"));
    const before = await ev(page, () => window.AuroraModels.pages.list().map((p) => p.id + ":" + p.state));
    await page.getByRole("button", { name: "Close Notes" }).click();
    expect(await ev(page, () => window.AuroraModels.pages.list().map((p) => p.id + ":" + p.state))).toEqual(before);
    await ev(page, () => window.AuroraShell.openApp("notes"));
    await expect(page.locator("#screen-notes")).toHaveClass(/active/); await expect(bar(page)).toBeVisible();
  });
  test("Escape and the in-screen Back still work as before", async ({ page }) => {
    await start(page);
    await ev(page, () => window.AuroraShell.openApp("calculator"));
    await page.locator("#btn-calc-back").click();
    expect(await active(page)).toBe("screen-home");
    await ev(page, () => window.AuroraShell.openApp("clock"));
    await page.keyboard.press("Escape");
    expect(await active(page)).toBe("screen-home");
  });
  test("Close is keyboard operable with a visible focus ring, and Enter closes", async ({ page }) => {
    await start(page);
    await ev(page, () => window.AuroraShell.openApp("trail"));
    await page.locator("#window-bar-close").focus();
    const o = await page.locator("#window-bar-close").evaluate((n) => getComputedStyle(n).outlineStyle + "|" + getComputedStyle(n).outlineWidth);
    expect(o).not.toMatch(/^none/); expect(o).not.toMatch(/\|0px$/);
    await page.keyboard.press("Enter");
    expect(await active(page)).toBe("screen-home");
  });
  test("Close is on the left, so it can never sit under the Glass Corner; the bar clears the corner and the Dock", async ({ page }) => {
    await start(page);
    for (const [app] of APPS) {
      await ev(page, (a) => window.AuroraShell.openApp(a), app);
      const r = await ev(page, () => {
        const c = document.querySelector(".glass-corner").getBoundingClientRect(), b = document.getElementById("window-bar").getBoundingClientRect(), x = document.getElementById("window-bar-close").getBoundingClientRect();
        const d = document.querySelector(".dock, #dock"), dk = d ? d.getBoundingClientRect() : null;
        return { closeLeftOfCorner: x.right < c.left, closeOverlapsCorner: !(x.right <= c.left || x.left >= c.right || x.bottom <= c.top || x.top >= c.bottom), dockOverlap: dk ? b.bottom > dk.top && b.top < dk.bottom : false };
      });
      expect(r.closeLeftOfCorner, app).toBe(true); expect(r.closeOverlapsCorner, app).toBe(false); expect(r.dockOverlap, app).toBe(false);
    }
  });
  test("the window grows with its content: nothing spills out of the card", async ({ page }) => {
    await start(page);
    await ev(page, () => window.AuroraShell.openApp("vault"));
    const r = await ev(page, () => {
      const s = document.getElementById("screen-vault"), c = document.getElementById("content");
      const last = [...s.querySelectorAll("button, li")].filter((n) => n.offsetParent).pop().getBoundingClientRect(), sb = s.getBoundingClientRect();
      return { cardBottom: sb.bottom + c.scrollTop, lastBottom: last.bottom + c.scrollTop };
    });
    expect(r.lastBottom).toBeLessThanOrEqual(r.cardBottom + 1);
  });
  test("no horizontal overflow on any app window", async ({ page }) => {
    await start(page);
    for (const [app] of APPS) { await ev(page, (a) => window.AuroraShell.openApp(a), app); expect(await ev(page, () => document.documentElement.scrollWidth <= window.innerWidth), app).toBe(true); }
  });
  test("high contrast: window borders are 2px", async ({ page }) => {
    await start(page);
    await ev(page, () => { window.AuroraShell.setContrast("high"); window.AuroraShell.openApp("notes"); });
    expect(await bar(page).evaluate((n) => getComputedStyle(n).borderTopWidth)).toBe("2px");
    expect(await page.locator("#screen-notes").evaluate((n) => getComputedStyle(n).borderLeftWidth)).toBe("2px");
  });
  test("reduced motion: the window has no transition or animation", async ({ page }) => {
    await page.emulateMedia({ reducedMotion: "reduce" });
    await start(page);
    await ev(page, () => { window.AuroraShell.setMotion("reduce"); window.AuroraShell.openApp("notes"); });
    for (const sel of ["#window-bar", "#screen-notes"]) {
      const t = await page.locator(sel).evaluate((n) => getComputedStyle(n).transitionDuration + "|" + getComputedStyle(n).animationName);
      expect(t).toMatch(/^0s\|none$/);
    }
  });
  test("the bar is a labelled group named after the app (Files and Vault open the same screen under their own names)", async ({ page }) => {
    await start(page);
    await ev(page, () => window.AuroraShell.openApp("files"));
    await expect(page.getByRole("group", { name: "Files window" })).toBeVisible();
    await ev(page, () => window.AuroraShell.openApp("vault"));
    await expect(page.getByRole("group", { name: "Vault window" })).toBeVisible();
  });
});

for (const w of [1024, 1440]) {
  test.describe(`Desktop window card (${w})`, () => {
    test.use({ viewport: { width: w, height: 800 } });
    test("every app window is centred, within its width limit and inside the viewport", async ({ page }) => {
      await start(page);
      for (const [app, , screen] of APPS) {
        await ev(page, (a) => window.AuroraShell.openApp(a), app);
        const s = await page.locator("#" + screen).boundingBox();
        expect(s.x, app).toBeGreaterThanOrEqual(0); expect(s.x + s.width, app).toBeLessThanOrEqual(w);
        expect(Math.abs(s.x - (w - (s.x + s.width))), app).toBeLessThan(40);                                          // centred (scrollbar tolerance)
        expect(s.width, app).toBeLessThanOrEqual(1120);
        expect(await ev(page, () => document.documentElement.scrollWidth <= window.innerWidth), app).toBe(true);
      }
    });
  });
}

test.describe("Not on phones or tablets", () => {
  for (const [w, h] of [[390, 844], [412, 915], [768, 1024]]) {
    test(`${w}px: no window bar, no window styling, and nothing in the accessibility tree`, async ({ page }) => {
      await page.setViewportSize({ width: w, height: h });
      await start(page);
      for (const [app, name, screen] of APPS) {
        await ev(page, (a) => window.AuroraShell.openApp(a), app);
        await expect(bar(page)).toBeHidden();
        await expect(page.getByRole("group", { name: name + " window" })).toHaveCount(0);
        const st = await page.locator("#" + screen).evaluate((n) => ({ border: getComputedStyle(n).borderTopWidth, shadow: getComputedStyle(n).boxShadow }));
        expect(st.shadow, app).toBe("none"); expect(st.border, app).toBe("0px");
      }
    });
  }
});

test.describe("200% text", () => {
  test.use({ viewport: { width: 1024, height: 768 } });
  test("the title bar stays one row, Close stays reachable, nothing overflows", async ({ page }) => {
    await start(page);
    await ev(page, () => { document.documentElement.setAttribute("data-text-scale", "200"); window.AuroraShell.openApp("calculator"); });
    const b = await bar(page).boundingBox(); expect(b.height).toBeLessThan(110);
    await expect(page.getByRole("button", { name: "Close Calculator" })).toBeInViewport();
    expect(await ev(page, () => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
    await page.getByRole("button", { name: "Close Calculator" }).click();
    expect(await active(page)).toBe("screen-home");
  });
});
