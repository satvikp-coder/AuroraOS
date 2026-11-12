/** Real-browser tests for the Settings screen (Chromium). Everything is simulated: no real device setting exists. */
const { test, expect } = require("@playwright/test");
const BASE_URL = "http://localhost:3000";

async function openSettings(page) {
  await page.goto(BASE_URL);
  await page.getByRole("button", { name: "Open Settings" }).click();
  await expect(page.locator("#screen-settings")).toBeVisible();
}
const row = (page, id) => page.locator(`#settings-sections [data-setting="${id}"]`);
const ctl = (page, id) => page.locator(`#settings-sections [data-fk="${id}"]`);

test.describe("Settings: structure and navigation", () => {
  test("opens from the pinned Settings tool and from Quick Center, and goes back", async ({ page }) => {
    await openSettings(page);
    await page.locator("#btn-settings-back").click();
    await expect(page.locator("#screen-home")).toBeVisible();
    await page.locator("#btn-quick-center").click();
    await page.getByRole("button", { name: "Open all settings" }).click();
    await expect(page.locator("#screen-settings")).toBeVisible();
  });
  test("all eight sections render with headings; advanced entries are hidden by default", async ({ page }) => {
    await openSettings(page);
    for (const n of ["Appearance", "Accessibility", "Notifications", "Privacy", "Battery", "Storage", "Aura", "Developer options"])
      await expect(page.getByRole("heading", { name: n, exact: true })).toBeVisible();
    await expect(row(page, "storage.trailDays")).toHaveCount(0);
    await expect(row(page, "privacy.auraHistory")).toHaveCount(0);
  });
  test("the Developer switch reveals the advanced entries from the SAME registry", async ({ page }) => {
    await openSettings(page);
    await ctl(page, "developer.showAdvanced").click();
    await expect(row(page, "storage.trailDays")).toBeVisible();
    await expect(row(page, "privacy.auraHistory")).toBeVisible();
    await ctl(page, "developer.showAdvanced").click();
    await expect(row(page, "storage.trailDays")).toHaveCount(0);
  });
  test("stored-only and locked entries are labelled honestly; locked entries have no control", async ({ page }) => {
    await openSettings(page);
    await expect(row(page, "notifications.lockPreviews")).toContainText("Stored only");
    const locked = row(page, "aura.approval");
    await expect(locked).toContainText("Locked");
    await expect(locked).toContainText("cannot be turned off");
    expect(await locked.locator("button, select, input").count()).toBe(0);
    expect(await row(page, "storage.usage").locator("button, select, input").count()).toBe(0);
  });
});

test.describe("Settings: real effects in the prototype", () => {
  test("theme: Settings changes the real theme and Quick Center stays in sync both ways", async ({ page }) => {
    await openSettings(page);
    await ctl(page, "appearance.theme").selectOption("dark");
    await expect(page.locator("html")).toHaveAttribute("data-theme", "dark");
    await page.locator("#btn-settings-back").click();
    await page.locator("#btn-quick-center").click();
    await expect(page.locator("#qc-appearance")).toHaveValue("dark");
    await page.locator("#qc-appearance").selectOption("light");
    await page.getByRole("button", { name: "Open all settings" }).click();
    await expect(ctl(page, "appearance.theme")).toHaveValue("light");
    await expect(page.locator("html")).toHaveAttribute("data-theme", "light");
  });
  test("text size scales the real text tokens", async ({ page }) => {
    await openSettings(page);
    const size = () => page.locator("#screen-settings .home-title").evaluate((e) => parseFloat(getComputedStyle(e).fontSize));
    const base = await size();
    await ctl(page, "accessibility.textSize").selectOption("200");
    await expect(page.locator("html")).toHaveAttribute("data-text-scale", "200");
    expect(await size()).toBeGreaterThanOrEqual(base * 1.9);
    await ctl(page, "accessibility.textSize").selectOption("100");
    expect(await page.locator("html").getAttribute("data-text-scale")).toBeNull();
    expect(await size()).toBe(base);
  });
  test("high contrast and reduce motion change the real document state", async ({ page }) => {
    await openSettings(page);
    await ctl(page, "accessibility.highContrast").click();
    await expect(page.locator("html")).toHaveAttribute("data-contrast", "high");
    await expect(ctl(page, "accessibility.highContrast")).toHaveAttribute("aria-checked", "true");
    await ctl(page, "accessibility.reduceMotion").click();
    expect(await page.evaluate(() => getComputedStyle(document.documentElement).getPropertyValue("--motion-base").trim())).toBe("0ms");
    await ctl(page, "accessibility.reduceMotion").click();
    expect(await page.evaluate(() => document.documentElement.style.getPropertyValue("--motion-base"))).toBe("");
  });
  test("battery saver forces Clear, locks the glass style control, and restores on turning off", async ({ page }) => {
    await openSettings(page);
    await expect(page.locator("html")).toHaveAttribute("data-glass-tier", "soft");
    await ctl(page, "battery.saver").click();
    await expect(page.locator("html")).toHaveAttribute("data-glass-tier", "clear");
    await expect(ctl(page, "appearance.glass")).toBeDisabled();
    await expect(ctl(page, "appearance.glass")).toHaveValue("clear");
    await expect(row(page, "appearance.glass")).toContainText("Battery saver keeps Clear mode");
    await ctl(page, "battery.saver").click();
    await expect(page.locator("html")).toHaveAttribute("data-glass-tier", "soft");
    await expect(ctl(page, "appearance.glass")).toBeEnabled();
    await ctl(page, "appearance.glass").selectOption("clear");
    await expect(page.locator("html")).toHaveAttribute("data-glass-tier", "clear");
  });
  test("Glass Corner and privacy indicators can be hidden and shown", async ({ page }) => {
    await openSettings(page);
    await ctl(page, "notifications.glassCorner").click();
    await expect(page.locator(".glass-corner")).toBeHidden();
    await ctl(page, "notifications.glassCorner").click();
    await expect(page.locator(".glass-corner")).toBeVisible();
    await ctl(page, "privacy.indicators").click();
    await page.locator("#btn-settings-back").click();
    await page.locator("#btn-quick-center").click();
    await expect(page.locator("#qc-privacy-card")).toBeHidden();
  });
  test("focus mode and Aura mode share state with Quick Center", async ({ page }) => {
    await openSettings(page);
    await ctl(page, "notifications.focus").click();
    await ctl(page, "aura.mode").selectOption("quick");
    await page.locator("#btn-settings-back").click();
    await page.locator("#btn-quick-center").click();
    await expect(page.locator('[data-setting="focus"]')).toHaveAttribute("aria-pressed", "true");
    await expect(page.locator("#qc-aura-mode")).toHaveValue("quick");
  });
  test("a finished Aura study plan turns focus on in Settings too", async ({ page }) => {
    await page.goto(BASE_URL);
    await page.locator("#btn-aura-study").click();
    await page.locator("#btn-plan-confirm").click();
    await page.locator("#btn-plan-run").click();
    await expect(page.locator("#plan-receipt")).toBeVisible({ timeout: 8000 });
    await page.locator("#btn-receipt-close").click();
    await page.getByRole("button", { name: "Open Settings" }).click();
    await expect(ctl(page, "notifications.focus")).toHaveAttribute("aria-checked", "true");
  });
});

test.describe("Settings: filter, Index search, keyboard", () => {
  test("the filter narrows rows, finds advanced entries, and reports no match", async ({ page }) => {
    await openSettings(page);
    await page.locator("#settings-filter").fill("contrast");
    await expect(page.locator("#settings-sections .setting-row")).toHaveCount(1);
    await expect(row(page, "accessibility.highContrast")).toBeVisible();
    await page.locator("#settings-filter").fill("history");
    await expect(row(page, "privacy.auraHistory")).toBeVisible();          // advanced entry found while advanced view is off
    await expect(row(page, "storage.trailDays")).toBeVisible();
    await page.locator("#settings-filter").fill("zzz-nothing");
    await expect(page.locator("#settings-empty")).toBeVisible();
    await page.locator("#settings-filter").fill("");
    await expect(page.locator("#settings-empty")).toBeHidden();
  });
  test("Index finds settings; Enter opens the Settings screen with that control focused", async ({ page }) => {
    await page.goto(BASE_URL);
    await page.keyboard.press("Control+k");
    await page.locator("#index-query").fill("text size");
    await expect(page.locator("#index-results .index-result", { hasText: "Setting · Appearance" }).or(page.locator("#index-results .index-result", { hasText: "Text size" })).first()).toBeVisible();
    await page.keyboard.press("Enter");
    await expect(page.locator("#screen-settings")).toBeVisible();
    await expect(ctl(page, "accessibility.textSize")).toBeFocused();
    await expect(row(page, "accessibility.textSize")).toHaveClass(/settings-highlight/);
  });
  test("Index finds an ADVANCED setting and enables the advanced view to show it", async ({ page }) => {
    await page.goto(BASE_URL);
    await page.keyboard.press("Control+k");
    await page.locator("#index-query").fill("retention");
    await page.locator("#index-results .index-result").first().click();
    await expect(page.locator("#screen-settings")).toBeVisible();
    await expect(ctl(page, "storage.trailDays")).toBeFocused();
    await expect(ctl(page, "developer.showAdvanced")).toHaveAttribute("aria-checked", "true");
  });
  test("every settings entry is findable through Index by its label", async ({ page }) => {
    await page.goto(BASE_URL);
    const labels = await page.evaluate(() => window.AuroraSettingsModel.all().map((e) => e.label));
    expect(labels.length).toBeGreaterThanOrEqual(14);
    await page.keyboard.press("Control+k");
    for (const l of labels) {
      await page.locator("#index-query").fill(l);
      await expect(page.locator("#index-results .index-result", { hasText: l }).first(), l).toBeVisible();
    }
  });
  test("Index renders text, not HTML: an injection attempt creates no elements and runs no script", async ({ page }) => {
    await page.goto(BASE_URL);
    await page.keyboard.press("Control+k");
    await page.locator("#index-query").fill("<img src=x onerror=window.__pwn=1>");
    await page.waitForTimeout(200);
    expect(await page.evaluate(() => typeof window.__pwn)).toBe("undefined");
    expect(await page.locator("#index-results img").count()).toBe(0);
  });
  test("toggling with the keyboard works and keeps focus on the same control", async ({ page }) => {
    await openSettings(page);
    await ctl(page, "accessibility.highContrast").focus();
    await page.keyboard.press("Space");
    await expect(ctl(page, "accessibility.highContrast")).toHaveAttribute("aria-checked", "true");
    await expect(ctl(page, "accessibility.highContrast")).toBeFocused();
    await page.keyboard.press("Enter");
    await expect(ctl(page, "accessibility.highContrast")).toHaveAttribute("aria-checked", "false");
    await expect(ctl(page, "accessibility.highContrast")).toBeFocused();
  });
  test("the range control changes its stored value and its visible output", async ({ page }) => {
    await openSettings(page);
    await ctl(page, "developer.showAdvanced").click();
    await ctl(page, "storage.trailDays").fill("45");
    await expect(page.locator("#st-out-storage-trailDays")).toHaveText("45 days");
    expect(await page.evaluate(() => window.AuroraSettingsModel.get("storage.trailDays"))).toBe(45);
  });
});

test.describe("Settings: reset dialog and JSON", () => {
  test("reset needs confirmation: focus moves in, Tab stays inside, Escape closes and returns focus, nothing changes on cancel", async ({ page }) => {
    await openSettings(page);
    await ctl(page, "appearance.theme").selectOption("dark");
    await page.locator("#btn-settings-reset").click();
    const dlg = page.locator("#settings-reset-confirm");
    await expect(dlg).toBeVisible();
    await expect(page.locator("#btn-settings-reset-cancel")).toBeFocused();
    await page.keyboard.press("Tab"); await expect(page.locator("#btn-settings-reset-ok")).toBeFocused();
    await page.keyboard.press("Tab"); await expect(page.locator("#btn-settings-reset-cancel")).toBeFocused();
    await page.keyboard.press("Shift+Tab"); await expect(page.locator("#btn-settings-reset-ok")).toBeFocused();
    await page.keyboard.press("Escape");
    await expect(dlg).toBeHidden();
    await expect(page.locator("#screen-settings")).toBeVisible();          // Escape closed only the dialog
    await expect(page.locator("#btn-settings-reset")).toBeFocused();
    await expect(page.locator("html")).toHaveAttribute("data-theme", "dark");
  });
  test("confirming reset restores every default and every effect", async ({ page }) => {
    await openSettings(page);
    await ctl(page, "appearance.theme").selectOption("dark");
    await ctl(page, "accessibility.textSize").selectOption("150");
    await ctl(page, "accessibility.highContrast").click();
    await ctl(page, "battery.saver").click();
    await ctl(page, "notifications.glassCorner").click();
    await page.locator("#btn-settings-reset").click();
    await page.locator("#btn-settings-reset-ok").click();
    await expect(page.locator("#settings-reset-confirm")).toBeHidden();
    const html = page.locator("html");
    await expect(html).toHaveAttribute("data-theme", "light");
    await expect(html).toHaveAttribute("data-contrast", "normal");
    await expect(html).toHaveAttribute("data-glass-tier", "soft");
    expect(await html.getAttribute("data-text-scale")).toBeNull();
    await expect(page.locator(".glass-corner")).toBeVisible();
    expect(await page.evaluate(() => JSON.stringify(window.AuroraSettingsModel.snapshot()))).toBe(await page.evaluate(() => JSON.stringify(window.AuroraSettings.createSettings().snapshot())));
  });
  test("the JSON view shows the live values and updates when a setting changes", async ({ page }) => {
    await openSettings(page);
    await page.locator("#btn-settings-json").click();
    await expect(page.locator("#settings-json")).toContainText('"appearance.theme": "light"');
    await ctl(page, "appearance.theme").selectOption("dark");
    await expect(page.locator("#settings-json")).toContainText('"appearance.theme": "dark"');
    await expect(page.locator("#btn-settings-json")).toHaveAttribute("aria-expanded", "true");
    const text = await page.locator("#settings-json").textContent();
    expect(Object.keys(JSON.parse(text))).not.toContain("aura.approval");
  });
});

// ---------------- viewport / theme / tier / accessibility matrix ----------------
const VIEWPORTS = [
  { name: "360", width: 360, height: 800 }, { name: "390", width: 390, height: 800 }, { name: "412", width: 412, height: 800 },
  { name: "tablet-768", width: 768, height: 1024 }, { name: "desktop-1280", width: 1280, height: 800 },
];
const SCREENS = [{ id: "screen-settings", open: (p) => p.evaluate(() => window.AuroraShell.showScreen("screen-settings")) },
  { id: "screen-pages", open: (p) => p.evaluate(() => window.AuroraShell.showScreen("screen-pages")) },
  { id: "screen-quick", open: (p) => p.evaluate(() => window.AuroraShell.showScreen("screen-quick")) }];

for (const vp of VIEWPORTS) {
  for (const theme of ["light", "dark"]) {
    for (const tier of ["soft", "clear"]) {
      test(`layout ${vp.name} ${theme} ${tier}: Settings, Pages and Quick Center have no overflow, clipping or small targets (also at 200% text + high contrast)`, async ({ page }, testInfo) => {
        await page.setViewportSize({ width: vp.width, height: vp.height });
        await page.goto(BASE_URL);
        await page.evaluate(([th, ti]) => { window.AuroraShell.setAppearance(th); window.AuroraShell.setTier(ti); }, [theme, tier]);
        for (const mode of ["default", "a11y"]) {
          if (mode === "a11y") {
            await page.evaluate(() => { const s = window.AuroraSettingsModel; s.set("accessibility.textSize", "200"); s.set("accessibility.highContrast", true); s.set("accessibility.reduceMotion", true); s.set("developer.showAdvanced", true); });
          }
          for (const scr of SCREENS) {
            await scr.open(page);
            await page.waitForTimeout(60);
            const problems = await page.evaluate((id) => {
              const out = [], vw = document.documentElement.clientWidth, sec = document.getElementById(id);
              if (document.documentElement.scrollWidth > window.innerWidth + 1) out.push("page scrolls horizontally: " + document.documentElement.scrollWidth + " > " + window.innerWidth);
              sec.querySelectorAll("button, select, input, [role=switch]").forEach((n) => {
                const r = n.getBoundingClientRect();
                if (r.width === 0 || r.height === 0) return;
                if (r.right > vw + 1 || r.left < -1) out.push("clipped horizontally: " + (n.id || n.getAttribute("data-fk") || n.textContent.trim().slice(0, 20)) + " [" + Math.round(r.left) + "," + Math.round(r.right) + "] of " + vw);
                if (Math.min(r.width, r.height) < 44 && n.type !== "hidden") out.push("touch target < 44px: " + (n.id || n.getAttribute("data-fk") || n.textContent.trim().slice(0, 20)) + " " + Math.round(r.width) + "x" + Math.round(r.height));
              });
              document.querySelectorAll("#dock .dock-btn").forEach((n) => {                   /* the dock is outside the screen section */
                const r = n.getBoundingClientRect();
                if (r.left < -1 || r.right > vw + 1) out.push("dock clipped: " + n.id + " [" + Math.round(r.left) + "," + Math.round(r.right) + "] of " + vw);
                if (Math.min(r.width, r.height) < 44) out.push("dock target < 44px: " + n.id + " " + Math.round(r.width) + "x" + Math.round(r.height));
              });
              const gc = document.querySelector(".glass-corner");
              if (gc && !gc.hidden) { const r = gc.getBoundingClientRect(); if (r.left < -1 || r.right > vw + 1) out.push("glass corner clipped"); }
              sec.querySelectorAll("h1, h2, h3, .setting-label, .page-meta").forEach((n) => { if (n.scrollWidth > n.clientWidth + 1 && getComputedStyle(n).overflow !== "visible") out.push("text clipped: " + n.textContent.trim().slice(0, 25)); });
              return out;
            }, scr.id);
            expect(problems, `${scr.id} ${mode}`).toEqual([]);
            if (vp.name === "390" || vp.name === "360") await page.screenshot({ path: testInfo.outputPath(`${scr.id}-${vp.name}-${theme}-${tier}-${mode}.png`) });
          }
        }
      });
    }
  }
}

// Font metrics differ between machines (the Linux CI runner overflowed where a Windows machine did not).
// Stress the same checks with a deliberately wide font so a fit that depends on narrow glyphs fails everywhere.
for (const vp of [{ name: "360", width: 360, height: 800 }, { name: "390", width: 390, height: 800 }]) {
  test(`wide-font stress ${vp.name}: no overflow or clipping at 200% text with a wide monospace font`, async ({ page }) => {
    await page.setViewportSize({ width: vp.width, height: vp.height });
    await page.goto(BASE_URL);
    await page.addStyleTag({ content: "*, *::before, *::after { font-family: 'Courier New', Courier, monospace !important; }" });
    await page.evaluate(() => { const s = window.AuroraSettingsModel; s.set("accessibility.textSize", "200"); s.set("developer.showAdvanced", true); });
    for (const id of ["screen-settings", "screen-pages", "screen-quick", "screen-home"]) {
      await page.evaluate((i) => window.AuroraShell.showScreen(i), id);
      await page.waitForTimeout(60);
      const problems = await page.evaluate((i) => {
        const out = [], vw = document.documentElement.clientWidth;
        if (document.documentElement.scrollWidth > window.innerWidth + 1) out.push("page scrolls horizontally: " + document.documentElement.scrollWidth);
        document.getElementById(i).querySelectorAll("button, select, input, [role=switch], .tile, .setting-row").forEach((n) => {
          const r = n.getBoundingClientRect();
          if (r.width && (r.right > vw + 1 || r.left < -1)) out.push("clipped: " + (n.id || n.getAttribute("data-setting") || n.getAttribute("data-fk") || n.className) + " [" + Math.round(r.left) + "," + Math.round(r.right) + "] of " + vw);
        });
        return out;
      }, id);
      expect(problems, id).toEqual([]);
    }
  });
}
