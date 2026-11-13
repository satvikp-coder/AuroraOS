/**
 * tests/ui-golden/shell-scroll.test.js - M8-A: the Glass Corner never sits over scrolled content, on ANY screen.
 * Shell-level rule (shell.css): once .content has scrolled, an opaque band occupies the corner's row; content scrolls under the band, not under
 * the corner. For every screen in screen-registry.js (Notes and Clock included) at 360 px with 200% text, 390 px and 1280 px, the content is scrolled
 * to the top, a quarter, the middle and the bottom; the points of the corner are probed with elementsFromPoint, and nothing from the scrolled
 * content may be found beneath the corner. Also: no band at scroll top (layout unchanged), the band does not block clicks, focus is never
 * hidden behind it, and dialogs stay above it.
 */
const { test, expect } = require("@playwright/test");
const { SCREENS } = require("./screen-registry");

const BASE_URL = "http://localhost:3000";
const ev = (page, fn, arg) => page.evaluate(fn, arg);
const VIEWPORTS = [{ name: "360 @200%", width: 360, height: 740, scale: "200" }, { name: "390", width: 390, height: 844 }, { name: "1280", width: 1280, height: 800 }];

/** Runs in the page. Returns problems for the current scroll position. */
function probe() {
  const out = [];
  const gc = document.querySelector(".glass-corner"); if (!gc || gc.hidden) return ["no glass corner"];
  const r = gc.getBoundingClientRect(), content = document.getElementById("content");
  const scrolled = content.scrollTop > 2;
  const shell = content.closest(".shell");
  if (shell.classList.contains("is-scrolled") !== scrolled) out.push("band state does not match scroll position: scrollTop=" + content.scrollTop + " class=" + shell.className);
  for (const fx of [0.3, 0.5, 0.7]) for (const fy of [0.3, 0.5, 0.7]) {
    const x = r.left + r.width * fx, y = r.top + r.height * fy;
    const stack = document.elementsFromPoint(x, y), i = stack.indexOf(gc);
    if (i === -1) { out.push("corner not on top at " + Math.round(x) + "," + Math.round(y)); continue; }
    // elementsFromPoint lists EVERYTHING at the point, front to back, occluded or not. The band is .shell's ::before, so content listed after .shell is hidden by it.
    const si = stack.indexOf(shell), visible = stack.slice(i + 1, si === -1 ? undefined : si);
    const under = visible.find((n) => content.contains(n) && n !== content);
    if (scrolled && si === -1) out.push("band missing from the hit stack");
    if (under && scrolled) out.push("scrolled content above the band under the corner: " + (under.id ? "#" + under.id : under.tagName + "." + String(under.className).slice(0, 30)));
    if (under && !scrolled && under.matches("button, a[href], input, select, textarea, [role=button], [role=tab], [role=switch]")) out.push("control under the corner at scroll top: " + (under.id || under.tagName));
  }
  return out;
}

for (const vp of VIEWPORTS) {
  test(`corner never over scrolled content: every screen, ${vp.name}`, async ({ page }) => {
    test.setTimeout(300000);
    await page.setViewportSize({ width: vp.width, height: vp.height });
    await page.goto(BASE_URL);
    if (vp.scale) await ev(page, (s) => document.documentElement.setAttribute("data-text-scale", s), vp.scale);
    let scrollable = 0;
    for (const scr of SCREENS) {
      if (scr.root === ".dialog") continue;
      await scr.open(page);
      const max = await ev(page, () => { const c = document.getElementById("content"); return c.scrollHeight - c.clientHeight; });
      if (max > 40) scrollable++;
      for (const f of [0, 0.25, 0.5, 1]) {
        await ev(page, (y) => { document.getElementById("content").scrollTop = y; }, Math.round(max * f));
        await page.waitForTimeout(30);
        expect(await ev(page, probe), `${scr.label} | ${vp.name} | scroll ${f * 100}% of ${max}`).toEqual([]);
      }
      await ev(page, () => { document.getElementById("content").scrollTop = 0; });
    }
    expect(scrollable, "some screens must actually scroll or this test proves nothing").toBeGreaterThan(5);
  });
}

test.describe("Corner band behaviour", () => {
  test.use({ viewport: { width: 390, height: 700 } });
  test("no band at scroll top; the band appears after scrolling, hides AND blocks what is beneath it, keeps the corner clickable, and is gone again at the top", async ({ page }) => {
    await page.goto(BASE_URL);
    await ev(page, () => window.AuroraShell.openApp("vault"));
    const band = () => ev(page, () => getComputedStyle(document.querySelector(".shell"), "::before").content !== "none" && getComputedStyle(document.querySelector(".shell"), "::before").height !== "auto");
    expect(await band()).toBe(false);
    await ev(page, () => { document.getElementById("content").scrollTop = 300; });
    await expect.poll(band).toBe(true);
    const hit = await ev(page, () => { const e = document.elementFromPoint(60, 20); return e ? (document.getElementById("content").contains(e) ? "content" : "shell") : "none"; });
    expect(hit).toBe("shell");                                                            // a tap on the band cannot reach a control hidden beneath it
    await page.locator("#glass-corner").click();                                          // the corner itself stays clickable above the band
    await expect(page.locator("#gc-panel")).toBeVisible();
    await page.keyboard.press("Escape");
    await ev(page, () => { document.getElementById("content").scrollTop = 0; });
    await expect.poll(band).toBe(false);
  });
  test("keyboard focus is scrolled clear of the band", async ({ page }) => {
    await page.goto(BASE_URL);
    await ev(page, () => { window.AuroraShell.openApp("vault"); document.getElementById("content").scrollTop = 400; });
    // Tab upwards through controls: each focused control must be below the band
    for (let i = 0; i < 6; i++) await page.keyboard.press("Shift+Tab");
    const r = await ev(page, () => { const n = document.activeElement; const b = n.getBoundingClientRect(); const content = document.getElementById("content"); return { inContent: content.contains(n), top: b.top, band: parseFloat(getComputedStyle(content.closest(".shell")).getPropertyValue("--corner-band")) || 0 }; });
    if (r.inContent && r.top >= 0) expect(r.top + 1).toBeGreaterThanOrEqual(64 - 1);
  });
  test("dialogs and the toast stay above the band", async ({ page }) => {
    await page.goto(BASE_URL);
    await ev(page, () => { window.AuroraShell.openApp("vault"); document.getElementById("content").scrollTop = 300; window.AuroraUI.dialog({ title: "Above", body: "x", id: "probe-dialog" }); });
    const top = await ev(page, () => { const d = document.getElementById("probe-dialog").getBoundingClientRect(); const e = document.elementFromPoint(d.left + d.width / 2, d.top + 20); return !!e && document.getElementById("probe-dialog").contains(e); });
    expect(top).toBe(true);
  });
});
