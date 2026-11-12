/**
 * tests/ui-golden/curated-shots.test.js — regenerates the curated reference screenshots (aurora-glass/screenshots/, <= 24 PNGs).
 * Skipped unless AURORA_SHOTS=1, so routine runs and CI never touch the tracked images:
 *   AURORA_SHOTS=1 npx playwright test --workers=1 tests/ui-golden/curated-shots.test.js
 * Names are <screen>-<width>-<theme>-<tier>[-<mode>].png. Look at the images after regenerating; do not just commit them.
 */
const { test } = require("@playwright/test");
const { shotPath } = require("./shots");

const BASE_URL = "http://localhost:3000";
test.skip(process.env.AURORA_SHOTS !== "1", "set AURORA_SHOTS=1 to regenerate the curated screenshots");

const ev = (page, fn, arg) => page.evaluate(fn, arg);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const ROUTES = {
  home: (p) => ev(p, () => window.AuroraShell.showScreen("screen-home")),
  spaces: (p) => ev(p, () => { window.AuroraShell.addSpace("Reading list"); window.AuroraShell.showScreen("screen-spaces"); }),
  index: (p) => ev(p, () => window.AuroraShell.openIndex("start a 10 minute timer")),
  pages: (p) => ev(p, () => window.AuroraShell.showScreen("screen-pages")),
  quick: (p) => ev(p, () => { window.AuroraModels.qs.set("focus", true); window.AuroraShell.showScreen("screen-quick"); }),
  settings: (p) => ev(p, () => window.AuroraShell.showScreen("screen-settings")),
  "aura-review": (p) => ev(p, () => { window.AuroraReview.ask("prepare my phone for studying"); }),
  "aura-confirm": async (p) => { await ev(p, () => { window.AuroraReview.ask("prepare my phone for studying"); }); await p.locator("#btn-plan-confirm").click(); },
  "aura-receipt": async (p) => { await ev(p, () => { window.AuroraReview.ask("prepare my phone for studying"); }); await p.locator("#btn-plan-confirm").click(); await p.locator("#btn-plan-run").click(); await p.locator("#plan-receipt").waitFor({ timeout: 8000 }); },
  "delete-confirm": (p) => ev(p, () => { window.AuroraSystem.request({ type: "file.delete", arguments: { id: "timetable" } }, { source: "shots" }); }),
  "glass-corner": async (p) => { await ev(p, () => { window.AuroraSystem.dev.stepDelayMs = 0; window.AuroraSystem.request({ type: "timer.start", arguments: { seconds: 600 } }, { source: "shots" }); window.AuroraSystem.permissions.set("files.edit", "allow"); }); await sleep(150); await p.locator("#glass-corner").click(); },
};

// [route, width, height, theme, tier, mode]
const SHOTS = [
  ["home", 390, 844, "light", "soft"], ["spaces", 390, 844, "light", "soft"], ["index", 390, 844, "light", "soft"], ["pages", 390, 844, "light", "soft"],
  ["quick", 390, 844, "light", "soft"], ["settings", 390, 844, "light", "soft"], ["aura-review", 390, 844, "light", "soft"], ["aura-confirm", 390, 844, "light", "soft"],
  ["aura-receipt", 390, 844, "light", "soft"], ["delete-confirm", 390, 844, "dark", "soft"], ["glass-corner", 390, 844, "light", "soft"],
  ["home", 390, 844, "dark", "soft"], ["quick", 390, 844, "dark", "soft"], ["home", 390, 844, "dark", "clear"],
  ["home", 768, 1024, "light", "soft"],
  ["home", 1280, 800, "light", "soft"], ["quick", 1280, 800, "light", "soft"], ["pages", 1280, 800, "light", "soft"], ["home", 1280, 800, "dark", "soft"], ["spaces", 1280, 800, "dark", "soft"],
  ["home", 360, 740, "light", "soft", "text200"], ["home", 390, 844, "light", "clear", "highcontrast"],
];

for (const [route, w, h, theme, tier, mode] of SHOTS) {
  const name = `${route}-${w}-${theme}-${tier}${mode ? "-" + mode : ""}.png`;
  test(name, async ({ page }, testInfo) => {
    await page.setViewportSize({ width: w, height: h });
    await page.clock.setFixedTime(new Date("2026-11-12T09:41:00"));   // reproducible clock; timers still run
    await page.goto(BASE_URL);
    await ev(page, ([t, ti, m]) => {
      window.AuroraSystem.dev.stepDelayMs = 20;
      window.AuroraShell.setAppearance(t); window.AuroraShell.setTier(ti);
      if (m === "text200") window.AuroraSettingsModel.set("accessibility.textSize", "200");
      if (m === "highcontrast") window.AuroraSettingsModel.set("accessibility.highContrast", true);
    }, [theme, tier, mode]);
    await ROUTES[route](page);
    await sleep(300);
    await page.screenshot({ path: shotPath(testInfo, name) });
  });
}
