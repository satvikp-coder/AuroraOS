/**
 * tests/ui-golden/layout-matrix.test.js — M1 / S8: layout across the supported matrix.
 * 7 viewports x 2 themes x 2 glass tiers x 3 modes (default, 150% text, accessibility: 200% text + high contrast +
 * reduced motion) over every screen in screen-registry.js. For each state it asserts: no horizontal scroll, no control
 * clipped by the viewport, 44 px minimum targets, no clipped headings, nothing under the Glass Corner, the Dock inside
 * the viewport, dialogs that fit with their actions reachable, and no console errors.
 * Screenshots (390 and 1280, default mode) go to the per-test output folder, or to aurora-glass/screenshots with AURORA_SHOTS=1.
 */
const { test, expect } = require("@playwright/test");
const { SCREENS } = require("./screen-registry");
const { shotPath } = require("./shots");

const BASE_URL = "http://localhost:3000";

const VIEWPORTS = [
  { name: "360", width: 360, height: 740 }, { name: "390", width: 390, height: 844 }, { name: "412", width: 412, height: 915 },
  { name: "tablet-768", width: 768, height: 1024 }, { name: "laptop-1024", width: 1024, height: 768 },
  { name: "desktop-1280", width: 1280, height: 800 }, { name: "wide-1440", width: 1440, height: 900 },
];
const MODES = ["default", "150%", "a11y"];

/** Runs in the page. Returns a list of problems for the region `root`. */
function inspect({ root, isDialog, isOverlay }) {
  const out = [];
  const vw = document.documentElement.clientWidth, vh = window.innerHeight;
  const region = document.querySelector(root);
  if (!region) return ["region not found: " + root];
  const name = (n) => (n.id ? "#" + n.id : n.getAttribute("aria-label") || n.getAttribute("data-fk") || (n.textContent || "").trim().slice(0, 24) || n.tagName);
  if (document.documentElement.scrollWidth > vw + 1) out.push("page scrolls horizontally " + document.documentElement.scrollWidth + " > " + vw);

  const gc = document.querySelector(".glass-corner"), gcr = gc && !gc.hidden ? gc.getBoundingClientRect() : null;
  if (gcr && (gcr.left < -1 || gcr.right > vw + 1)) out.push("glass corner clipped");

  region.querySelectorAll("button, input, select, textarea, [role=switch], [role=option]").forEach((n) => {
    const r = n.getBoundingClientRect();
    if (!r.width || !r.height || getComputedStyle(n).visibility === "hidden") return;
    if (n.closest("[hidden]")) return;
    if (r.left < -1 || r.right > vw + 1) out.push("clipped horizontally: " + name(n) + " [" + Math.round(r.left) + "," + Math.round(r.right) + "] of " + vw);
    if (n.tagName !== "INPUT" || n.type !== "range") { if (Math.min(r.width, r.height) < 44) out.push("target < 44px: " + name(n) + " " + Math.round(r.width) + "x" + Math.round(r.height)); }
    if (!isDialog && !isOverlay && gcr && r.left < gcr.right && r.right > gcr.left && r.top < gcr.bottom && r.bottom > gcr.top && n !== gc) out.push("overlaps the Glass Corner: " + name(n));
  });

  region.querySelectorAll("h1, h2, h3, .row-title, .index-title, .page-open, .space-card-item h3, .step-title, .dialog-title").forEach((n) => {
    if (!n.offsetWidth || n.closest(".visually-hidden")) return;
    if (n.scrollWidth > n.clientWidth + 1 && getComputedStyle(n).overflowX !== "visible") out.push("text clipped: " + (n.textContent || "").trim().slice(0, 24));
  });

  const dock = document.getElementById("dock");
  if (dock) {
    const d = dock.getBoundingClientRect();
    if (d.left < -1 || d.right > vw + 1) out.push("dock exceeds the viewport [" + Math.round(d.left) + "," + Math.round(d.right) + "]");
    dock.querySelectorAll(".dock-btn").forEach((b) => { const r = b.getBoundingClientRect(); if (r.left < -1 || r.right > vw + 1) out.push("dock button clipped: " + b.id); if (Math.min(r.width, r.height) < 44) out.push("dock target < 44px: " + b.id); });
  }

  if (isOverlay) {                                                  // a popup menu: wholly inside the viewport, above the Glass Corner like a dialog
    const m = region.getBoundingClientRect();
    if (m.left < -1 || m.right > vw + 1 || m.top < -1 || m.bottom > vh + 1) out.push("menu outside the viewport");
  }
  if (isDialog) {
    const d = region.getBoundingClientRect();
    if (d.left < -1 || d.right > vw + 1) out.push("dialog clipped horizontally");
    const scrolls = region.scrollHeight > region.clientHeight + 1 || region.closest(".scrim").scrollHeight > region.closest(".scrim").clientHeight + 1;
    if (d.height > vh + 1 && !scrolls) out.push("dialog taller than the viewport and not scrollable");
    region.querySelectorAll(".dialog-actions button").forEach((b) => {
      b.scrollIntoView({ block: "nearest" });
      const r = b.getBoundingClientRect();
      if (r.bottom > vh + 1 || r.top < -1) out.push("dialog action not reachable: " + name(b));
    });
  }
  return out;
}

for (const vp of VIEWPORTS) {
  for (const theme of ["light", "dark"]) {
    for (const tier of ["soft", "clear"]) {
      test(`layout ${vp.name} ${theme} ${tier}: every screen in every mode`, async ({ page }, testInfo) => {
        test.setTimeout(120000);
        const errors = [];
        page.on("pageerror", (e) => errors.push("pageerror: " + e.message));
        page.on("console", (m) => { if (m.type() === "error") errors.push(m.text()); });
        await page.setViewportSize({ width: vp.width, height: vp.height });
        await page.goto(BASE_URL);
        await page.evaluate(([t, ti]) => { window.AuroraShell.setAppearance(t); window.AuroraShell.setTier(ti); window.AuroraSystem.dev.stepDelayMs = 0; }, [theme, tier]);

        for (const mode of MODES) {
          await page.evaluate((m) => {
            const root = document.documentElement, st = window.AuroraSettingsModel;
            st.set("accessibility.textSize", m === "default" ? "100" : m === "150%" ? "150" : "200");
            st.set("accessibility.highContrast", m === "a11y");
            st.set("accessibility.reduceMotion", m === "a11y");
            st.set("developer.showAdvanced", m === "a11y");
          }, mode);
          for (const scr of SCREENS) {
            await scr.open(page);
            await page.waitForTimeout(40);
            const problems = await page.evaluate(inspect, { root: scr.root, isDialog: scr.root === ".dialog", isOverlay: scr.root === ".ctx-menu" });
            expect(problems, `${scr.label} | ${vp.name} ${theme} ${tier} | ${mode}`).toEqual([]);
            if (mode === "default" && theme === "light" && tier === "soft" && (vp.name === "390" || vp.name === "desktop-1280")) {
              await page.screenshot({ path: shotPath(testInfo, `matrix-${scr.id}-${vp.name}.png`) });
            }
            if (scr.close) await scr.close(page);
          }
        }
        expect(errors).toEqual([]);
      });
    }
  }
}
