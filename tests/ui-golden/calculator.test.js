/**
 * tests/ui-golden/calculator.test.js - M6: Calculator.
 * A browser-prototype app over the DOM-free engine (tests/unit/calc.test.js covers the arithmetic). These tests check the screen: pointer and keyboard
 * paths, the error display, history reuse, keyboard scoping (keys only act while the screen is active and focus is on the page or the keypad),
 * no layout shift on long results, narrow and wide layouts, and that nothing is persisted or sent anywhere.
 */
const { test, expect } = require("@playwright/test");

const BASE_URL = "http://localhost:3000";
const ev = (page, fn, arg) => page.evaluate(fn, arg);

async function start(page) { await page.goto(BASE_URL); }
async function open(page) {
  await ev(page, () => window.AuroraShell.openApp("calculator"));
  await expect(page.locator("#screen-calculator h1")).toHaveText("Calculator");
  await ev(page, () => window.AuroraCalculator.reset());
}
const result = (page) => page.locator("#calc-result");
const expr = (page) => page.locator("#calc-expr");
const KEY = { "+": "add", "-": "subtract", "*": "multiply", "/": "divide", "=": "equals", ".": "dot", "C": "clear", "B": "backspace", "%": "percent", "n": "sign" };
async function tap(page, seq) {
  for (const ch of seq) await page.locator("#btn-calc-" + (KEY[ch] || ch)).click();
}
async function type(page, seq) { for (const ch of seq) await page.keyboard.press(ch); }

test.describe("Calculator: getting there", () => {
  test("is a real screen opened from the app list, Index and openApp; starts at 0 with the prototype notice", async ({ page }) => {
    await start(page);
    await open(page);
    await expect(result(page)).toHaveText("0");
    await expect(page.locator("#calc-banner")).toContainText("Nothing is saved");
    await expect(page.locator("#screen-calculator")).toHaveClass(/active/);
    await ev(page, () => window.AuroraShell.showScreen("screen-home"));
    await page.keyboard.press("Control+k");
    await page.locator("#index-query").fill("calculator");
    await page.locator("#index-results .index-result", { hasText: "Calculator" }).first().click();
    await expect(page.locator("#screen-calculator")).toHaveClass(/active/);
  });
  test("has twenty keys, each at least 48px tall, with accessible names for the symbols", async ({ page }) => {
    await start(page); await open(page);
    const keys = page.locator(".calc-key");
    await expect(keys).toHaveCount(20);
    const boxes = await keys.evaluateAll((els) => els.map((e) => { const r = e.getBoundingClientRect(); return [Math.round(r.width), Math.round(r.height), e.getAttribute("aria-label") || e.textContent]; }));
    for (const [w, h, name] of boxes) { expect(h, name).toBeGreaterThanOrEqual(48); expect(w, name).toBeGreaterThanOrEqual(40); }
    for (const label of ["Clear", "Backspace", "Percent", "Divide", "Multiply", "Subtract", "Add", "Equals", "Change sign", "Decimal point"]) await expect(page.getByRole("button", { name: label, exact: true })).toBeVisible();
  });
  test("the Back button returns to the previous screen", async ({ page }) => {
    await start(page); await open(page);
    await page.locator("#btn-calc-back").click();
    await expect(page.locator("#screen-calculator")).not.toHaveClass(/active/);
  });
});

test.describe("Calculator: pointer", () => {
  test("2 + 3 x 4 = 20 (immediate execution) and the expression line shows the last operation", async ({ page }) => {
    await start(page); await open(page);
    await tap(page, "2+3*4=");
    await expect(result(page)).toHaveText("20");
    await expect(expr(page)).toHaveText("5 × 4 =");
  });
  test("0.1 + 0.2 = 0.3 exactly; 1 / 3 shows twelve digits; repeated equals repeats", async ({ page }) => {
    await start(page); await open(page);
    await tap(page, "0.1+0.2="); await expect(result(page)).toHaveText("0.3");
    await tap(page, "C1/3="); await expect(result(page)).toHaveText("0.333333333333");
    await tap(page, "C2+3=="); await expect(result(page)).toHaveText("8");
  });
  test("percent, sign and backspace keys", async ({ page }) => {
    await start(page); await open(page);
    await tap(page, "200+10%="); await expect(result(page)).toHaveText("220");
    await tap(page, "C9n"); await expect(result(page)).toHaveText("-9");
    await tap(page, "C123BB"); await expect(result(page)).toHaveText("1");
  });
  test("division by zero shows a plain-language error, ignores everything but Clear, and Clear recovers", async ({ page }) => {
    await start(page); await open(page);
    await tap(page, "7/0=");
    await expect(result(page)).toHaveText("Can't divide by zero");
    await expect(result(page)).toHaveClass(/is-error/);
    await tap(page, "5+"); await expect(result(page)).toHaveText("Can't divide by zero");
    await tap(page, "C5"); await expect(result(page)).toHaveText("5");
    await expect(result(page)).not.toHaveClass(/is-error/);
  });
  test("a result that needs exponent form is shown as such and the entry is capped at twelve digits", async ({ page }) => {
    await start(page); await open(page);
    await tap(page, "9999999999999"); await expect(result(page)).toHaveText("999999999999");
    await tap(page, "+1="); await expect(result(page)).toHaveText("1e+12");
  });
});

test.describe("Calculator: history", () => {
  test("completed calculations are listed newest first (at most 10); errors and typing are not", async ({ page }) => {
    await start(page); await open(page);
    await expect(page.locator("#calc-history .calc-hist-empty")).toBeVisible();
    await tap(page, "2+3="); await tap(page, "C6*7="); await tap(page, "C7/0=");
    await expect(page.locator(".calc-hist-item")).toHaveCount(2);
    await expect(page.locator(".calc-hist-item").first()).toContainText("6 × 7 =");
    await expect(page.locator(".calc-hist-item").first()).toContainText("42");
    for (let i = 1; i <= 12; i++) await tap(page, "C" + i + "+1=");
    await expect(page.locator(".calc-hist-item")).toHaveCount(10);
    await expect(page.locator("#calc-history-count")).toHaveText("10 calculations");
  });
  test("tapping a history entry reuses its result, an operator continues from it, a digit starts fresh, and reuse is not recorded twice", async ({ page }) => {
    await start(page); await open(page);
    await tap(page, "6*7=C5+5=");
    await page.locator('.calc-hist-item[data-hist="1"]').click();                 // 6 x 7 = 42
    await expect(result(page)).toHaveText("42");
    await tap(page, "+8="); await expect(result(page)).toHaveText("50");
    await expect(page.locator(".calc-hist-item")).toHaveCount(3);
    await page.locator('.calc-hist-item[data-hist="2"]').click();
    await tap(page, "9"); await expect(result(page)).toHaveText("9");
  });
  test("Clear history empties the list without touching the display", async ({ page }) => {
    await start(page); await open(page);
    await tap(page, "2+3=");
    await page.locator("#btn-calc-hist-clear").click();
    await expect(page.locator(".calc-hist-item")).toHaveCount(0);
    await expect(result(page)).toHaveText("5");
  });
});

test.describe("Calculator: keyboard", () => {
  test("digits, operators, Enter, Backspace, Escape, % and the sign key all work while the screen is active", async ({ page }) => {
    await start(page); await open(page);
    await type(page, "12+30");
    await page.keyboard.press("Enter");
    await expect(result(page)).toHaveText("42");
    await page.keyboard.press("Escape"); await expect(result(page)).toHaveText("0");
    await type(page, "5*3="); await expect(result(page)).toHaveText("15");
    await page.keyboard.press("Escape");
    await type(page, "123"); await page.keyboard.press("Backspace"); await expect(result(page)).toHaveText("12");
    await page.keyboard.press("F9"); await expect(result(page)).toHaveText("-12");
    await page.keyboard.press("Escape");
    await type(page, "200+10%="); await expect(result(page)).toHaveText("220");
    await page.keyboard.press("Escape"); await type(page, "1,5+1.5="); await expect(result(page)).toHaveText("3");
    await page.keyboard.press("Escape"); await type(page, "2+3"); await page.keyboard.press("Delete"); await type(page, "4="); await expect(result(page)).toHaveText("6");
  });
  test("Enter does not also click the focused key (no double action)", async ({ page }) => {
    await start(page); await open(page);
    await page.locator("#btn-calc-7").click();
    await page.locator("#btn-calc-7").focus();
    await type(page, "+1");
    await page.keyboard.press("Enter");
    await expect(result(page)).toHaveText("8");
    await expect(page.locator(".calc-hist-item")).toHaveCount(1);
  });
  test("keys do nothing when the Calculator is not the active screen", async ({ page }) => {
    await start(page); await open(page);
    await tap(page, "5");
    await ev(page, () => window.AuroraShell.showScreen("screen-home"));
    await type(page, "99+1=");
    await ev(page, () => window.AuroraShell.openApp("calculator"));
    await expect(result(page)).toHaveText("5");
  });
  test("keys do not leak into other controls: typing in Index search does not change the calculator", async ({ page }) => {
    await start(page); await open(page);
    await tap(page, "5");
    await page.keyboard.press("Control+k");
    const search = page.locator("#index-query");
    await search.fill(""); await search.pressSequentially("12+3");
    await ev(page, () => window.AuroraShell.openApp("calculator"));
    await expect(result(page)).toHaveText("5");
  });
  test("a focused Back button keeps its own behaviour: Enter goes back instead of evaluating", async ({ page }) => {
    await start(page); await open(page);
    await tap(page, "2+3");
    await page.locator("#btn-calc-back").focus();
    await page.keyboard.press("Enter");
    await expect(page.locator("#screen-calculator")).not.toHaveClass(/active/);
    await ev(page, () => window.AuroraShell.openApp("calculator"));
    await expect(result(page)).toHaveText("3");
  });
  test("modifier shortcuts are left alone (Ctrl+5 is not a digit)", async ({ page }) => {
    await start(page); await open(page);
    await page.keyboard.press("Control+5");
    await expect(result(page)).toHaveText("0");
  });
});

test.describe("Calculator: layout and accessibility", () => {
  test("the result is a polite live region and the history is labelled", async ({ page }) => {
    await start(page); await open(page);
    await expect(result(page)).toHaveAttribute("aria-live", "polite");
    await expect(page.locator("#calc-history-card")).toHaveAttribute("aria-label", "History");
    await expect(page.locator("#calc-pad")).toHaveAttribute("role", "group");
  });
  test("a long result does not move the keypad (no layout shift) and never overflows the screen horizontally", async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 844 });
    await start(page); await open(page);
    const top = () => page.locator("#calc-pad").evaluate((e) => Math.round(e.getBoundingClientRect().top));
    const t0 = await top();
    await tap(page, "123456789012");
    expect(await top()).toBe(t0);
    await tap(page, "*999999999999=");
    expect(await top()).toBe(t0);
    await expect(result(page)).toHaveText("1.23456789012e+23");
    const over = await ev(page, () => document.documentElement.scrollWidth - document.documentElement.clientWidth);
    expect(over).toBeLessThanOrEqual(0);
    await tap(page, "C7/0=");
    expect(await top()).toBe(t0);
  });
  test("narrow: history sits below the keypad; wide: beside it", async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 844 });
    await start(page); await open(page);
    const pos = () => ev(page, () => { const p = document.querySelector("#calc-pad").getBoundingClientRect(), h = document.querySelector("#calc-history-card").getBoundingClientRect(); return { padBottom: p.bottom, histTop: h.top, padRight: p.right, histLeft: h.left }; });
    let p = await pos(); expect(p.histTop).toBeGreaterThanOrEqual(p.padBottom - 1);
    await page.setViewportSize({ width: 1280, height: 800 });
    p = await pos(); expect(p.histLeft).toBeGreaterThanOrEqual(p.padRight - 1);
  });
  test("works at 200% text, dark theme and high contrast without horizontal overflow, with every key still visible", async ({ page }) => {
    await page.setViewportSize({ width: 360, height: 780 });
    await start(page); await open(page);
    await ev(page, () => { document.documentElement.setAttribute("data-text-scale", "200"); document.documentElement.setAttribute("data-theme", "dark"); document.documentElement.setAttribute("data-contrast", "high"); });
    await tap(page, "123456789012*999999999999=");
    const over = await ev(page, () => document.documentElement.scrollWidth - document.documentElement.clientWidth);
    expect(over).toBeLessThanOrEqual(0);
    await expect(result(page)).toHaveText("1.23456789012e+23");
    const clipped = await result(page).evaluate((e) => e.scrollWidth - e.clientWidth);
    expect(clipped, "the whole result is visible (no clipped exponent)").toBeLessThanOrEqual(0);
    const titleWraps = await page.locator("#screen-calculator h1").evaluate((e) => e.getBoundingClientRect().height > parseFloat(getComputedStyle(e).fontSize) * 1.6);
    expect(titleWraps, "the title stays on one line").toBe(false);
    const hidden = await page.locator(".calc-key").evaluateAll((els) => els.filter((e) => { const r = e.getBoundingClientRect(); return r.width < 20 || r.height < 40 || r.right > innerWidth + 1; }).length);
    expect(hidden).toBe(0);
  });
  test("under reduced motion nothing animates", async ({ page }) => {
    await page.emulateMedia({ reducedMotion: "reduce" });
    await start(page); await open(page);
    await tap(page, "2+3=");
    const t = await page.locator("#btn-calc-5").evaluate((e) => getComputedStyle(e).transitionDuration);
    expect(["0s", "0s, 0s"]).toContain(t);
  });
});

test.describe("Calculator: safety", () => {
  test("a hostile string cannot be injected: only fixed key presses change the display, and history text is plain text", async ({ page }) => {
    await start(page); await open(page);
    const r = await ev(page, () => { const C = window.AuroraCalculator; C.press("digit", "<img src=x onerror=window.__pwn=1>"); C.press("op", "constructor"); C.press("bogus"); return window.__pwn === 1; });
    expect(r).toBe(false);
    await expect(result(page)).toHaveText("0");
    await expect(page.locator("#screen-calculator img")).toHaveCount(0);
  });
  test("nothing is stored: no localStorage or sessionStorage entry and no network request", async ({ page }) => {
    const reqs = [];
    page.on("request", (q) => { if (!q.url().startsWith(BASE_URL) && !q.url().startsWith("data:")) reqs.push(q.url()); });
    await start(page); await open(page);
    await tap(page, "2+3=");
    const stored = await ev(page, () => ({ l: Object.keys(localStorage).filter((k) => /calc/i.test(k)), s: Object.keys(sessionStorage).filter((k) => /calc/i.test(k)) }));
    expect(stored.l).toEqual([]); expect(stored.s).toEqual([]);
    expect(reqs).toEqual([]);
  });
  test("the browser console stays clean while using every key", async ({ page }) => {
    const bad = [];
    page.on("console", (m) => { if (m.type() === "error") bad.push(m.text()); });
    page.on("pageerror", (e) => bad.push(String(e)));
    await start(page); await open(page);
    await tap(page, "7/0=C1.5*2=%n+B");
    await type(page, "12+3=");
    expect(bad).toEqual([]);
  });
});
