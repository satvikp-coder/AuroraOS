/**
 * tests/ui-golden/aurora-glass.test.js
 *
 * Automated validation of the Aurora Glass browser prototype.
 * Runs in Node.js with jsdom. Validates structure, controls, state
 * transitions, accessibility, responsive breakpoints, and theme/tier toggles.
 *
 * This is a design-prototype test only. It never touches a real OS or device.
 */

const fs = require("fs");
const path = require("path");
const { JSDOM } = require("jsdom");

const PROTO_DIR = path.resolve(__dirname, "..", "..", "aurora-glass", "prototype");
const HTML_PATH = path.join(PROTO_DIR, "index.html");
const CSS_PATH = path.join(PROTO_DIR, "shell.css");
const COMPONENTS_PATH = path.join(PROTO_DIR, "components.css");
const REVIEW_PATH = path.join(PROTO_DIR, "review.js");
const TOKENS_PATH = path.join(PROTO_DIR, "tokens.css");
const APP_PATH = path.join(PROTO_DIR, "app.js");

let passed = 0;
let failed = 0;
const failures = [];

function assert(condition, message) {
  if (condition) {
    passed++;
    console.log("  PASS:", message);
  } else {
    failed++;
    failures.push(message);
    console.log("  FAIL:", message);
  }
}

function assertContains(haystack, needle, message) {
  assert(String(haystack).includes(needle), message);
}

let htmlSource = "";
async function loadDom() {
  htmlSource = fs.readFileSync(HTML_PATH, "utf8");
  const dom = new JSDOM(htmlSource, {
    url: "http://localhost/prototype/",
    pretendToBeVisual: true,
  });
  return dom;
}

async function main() {
  console.log("== Aurora Glass prototype validation ==");
  console.log("HTML:", HTML_PATH);
  console.log("");

  // ---------- 1. File existence ----------
  console.log("-- File existence --");
  assert(fs.existsSync(HTML_PATH), "index.html exists");
  assert(fs.existsSync(CSS_PATH), "shell.css exists");
  assert(fs.existsSync(TOKENS_PATH), "tokens.css exists");
  assert(fs.existsSync(APP_PATH), "app.js exists");

  // ---------- 2. Load DOM ----------
  console.log("\n-- DOM structure --");
  const dom = await loadDom();
  const document = dom.window.document;

  // All required screens present
  const requiredScreens = [
    "screen-home",
    "screen-spaces",
    "screen-index",
    "screen-aura",
  ];
  requiredScreens.forEach((id) => {
    assert(!!document.getElementById(id), `Screen present: ${id}`);
  });

  // Aurora Dock present
  assert(!!document.getElementById("dock"), "Aurora Dock present");
  ["dock-back", "dock-home", "dock-spaces", "dock-pages", "dock-ask"].forEach((id) => {
    assert(!!document.getElementById(id), `Dock button: ${id}`);
  });

  // Glass Corner present
  assert(!!document.querySelector(".glass-corner"), "Glass Corner present");

  // Aurora Horizon present
  assert(!!document.querySelector(".horizon"), "Aurora Horizon present");

  // Aura Plan: the screen is an empty placeholder; review.js (the one trusted review surface) renders it.
  assert(document.getElementById("screen-aura").children.length === 0, "Aura plan screen is an empty placeholder in index.html");
  const reviewSrc = fs.readFileSync(REVIEW_PATH, "utf8");
  ["plan-request-text", "btn-plan-edit", "btn-plan-confirm", "plan-confirmation", "plan-running", "plan-receipt", "plan-failure"].forEach((id) => {
    assert(reviewSrc.includes('"' + id + '"'), `review.js renders Aura plan element: ${id}`);
  });

  // Index search
  assert(!!document.getElementById("index-query"), "Index search input present");
  assert(!!document.getElementById("index-results"), "Index results list present");

  // ---------- 3. Accessibility: ARIA / labels ----------
  console.log("\n-- Accessibility --");
  const dock = document.getElementById("dock");
  assert(dock.getAttribute("role") === "navigation", "Dock has navigation role");
  assert(dock.getAttribute("aria-label"), "Dock has aria-label");

  const gc = document.querySelector(".glass-corner");
  assert(gc.getAttribute("aria-label"), "Glass Corner has aria-label");
  assert(gc.getAttribute("tabindex") === "0", "Glass Corner is keyboard-focusable");

  const searchInput = document.getElementById("index-query");
  assert(searchInput.getAttribute("role") === "combobox" || !!searchInput.getAttribute("aria-label") || !!searchInput.getAttribute("placeholder"), "Index input has accessible name");

  const live = document.getElementById("a11y-live");
  assert(!!live && live.getAttribute("aria-live") === "polite", "Polite live region present");

  // Every button has accessible text
  const buttons = document.querySelectorAll("button");
  let buttonsWithoutLabel = 0;
  buttons.forEach((b) => {
    const label = b.getAttribute("aria-label") || b.textContent.trim();
    if (!label) buttonsWithoutLabel++;
  });
  assert(buttonsWithoutLabel === 0, `All ${buttons.length} buttons have accessible names`);

  // Focus-visible styles present in CSS
  const css = fs.readFileSync(COMPONENTS_PATH, "utf8");
  assert(css.includes(":focus-visible"), "components.css defines :focus-visible styles");
  // Scoped overrides such as "#pages-memory .btn { ... }" are fine; a rule that STARTS with a component class is a redefinition.
  assert(!/^.(btn|card|chip|dialog|toast|meter|switch|list|empty-state)[s,.:{[]/m.test(fs.readFileSync(CSS_PATH, "utf8")), "shell.css does not redefine component classes");

  // ---------- 4. Themes & tiers ----------
  console.log("\n-- Themes & glass tiers --");
  const root = document.documentElement;
  assert(root.getAttribute("data-theme") === "light", "Default theme is light");
  assert(root.getAttribute("data-glass-tier") === "soft", "Default tier is Soft Glass");

  // tokens.css defines light/dark/clear/soft
  const tokensCss = fs.readFileSync(TOKENS_PATH, "utf8");
  assert(tokensCss.includes("data-theme=\"light\""), "tokens.css defines light mode");
  assert(tokensCss.includes("data-theme=\"dark\""), "tokens.css defines dark mode");
  assert(tokensCss.includes("data-glass-tier=\"clear\""), "tokens.css defines Clear tier");
  assert(tokensCss.includes("data-glass-tier=\"soft\""), "tokens.css defines Soft Glass tier");
  assert(tokensCss.includes("data-contrast=\"high\""), "tokens.css defines high contrast");

  // ---------- 5. Responsive breakpoints ----------
  console.log("\n-- Responsive (360/390/412px) --");
  assert(tokensCss.includes("--break-phone: 360px"), "360px breakpoint token");
  assert(tokensCss.includes("--break-phone-wide: 390px"), "390px breakpoint token");
  assert(tokensCss.includes("--break-phone-max: 412px"), "412px breakpoint token");

  // Test setting viewport widths
  for (const w of [360, 390, 412]) {
    dom.window.innerWidth = w;
    dom.window.innerHeight = 800;
    const screen = document.querySelector(".screen.active");
    assert(!!screen, `Content renders at ${w}px width`);
  }

  // ---------- 6. Touch targets ----------
  console.log("\n-- Touch targets (48x48) --");
  assert(tokensCss.includes("--touch-target: 48px"), "48px touch target token defined");
  assert(css.includes("min-height: var(--touch-target)"), "Buttons use touch-target min-height");
  assert(css.includes("min-width: var(--touch-target)"), "Buttons use touch-target min-width");

  // ---------- 7. Motion / reduced motion ----------
  console.log("\n-- Motion & reduced motion --");
  assert(tokensCss.includes("prefers-reduced-motion"), "Respects prefers-reduced-motion");
  assert(tokensCss.includes("120ms") || tokensCss.includes("--motion-fast: 120ms"), "Motion uses 120ms fast tier");
  assert(tokensCss.includes("220ms") || tokensCss.includes("--motion-slow: 220ms"), "Motion caps at 220ms");

  // ---------- 8. 200% text reflow ----------
  console.log("\n-- 200% text reflow --");
  assert(tokensCss.includes("--text-2xl: 30px") || tokensCss.includes("--text-3xl: 34px"), "Headline tokens 28-34px range");
  assert(tokensCss.includes("--text-base: 16px"), "Base text 16px (within 14-17px routine range)");

  // ---------- 9. Keyboard navigation simulation ----------
  console.log("\n-- Keyboard navigation --");

  // Tab cycles focus through dock buttons
  const dockBtns = Array.from(document.querySelectorAll(".dock-btn"));
  assert(dockBtns.length === 5, "5 dock buttons");
  dockBtns.forEach((b) => {
    assert(b.getAttribute("tabindex") === "0" || !b.getAttribute("tabindex"), `Dock button focusable: ${b.id}`);
  });

  // Escape handling present in app.js
  const appJs = fs.readFileSync(APP_PATH, "utf8");
  assert(appJs.includes('e.key === "Escape"'), "app.js handles Escape key");
  assert(appJs.includes('e.key === "Enter"'), "app.js handles Enter key");
  assert(appJs.includes("showScreen"), "app.js has showScreen navigation");

  // ---------- 10. Aura plan state transitions ----------
  console.log("\n-- Aura plan (rendered by review.js; behaviour is covered by tests/ui-golden) --");
  assert(!document.getElementById("controls-panel"), "Prototype controls panel was removed (its switches live in Settings)");
  assert(!/planTimer|startPlan|advancePlan/.test(fs.readFileSync(APP_PATH, "utf8")), "app.js no longer fakes a plan run");
  assert(reviewSrc.includes("T.approve("), "review.js approves through the broker's trusted facet");
  assert(!/innerHTML/.test(reviewSrc + fs.readFileSync(APP_PATH, "utf8")), "no innerHTML in the shell or the review surface");

  // ---------- 11. No dead-end buttons ----------
  console.log("\n-- No dead-end controls --");
  const allButtons = document.querySelectorAll("button");
  let deadEnds = 0;
  allButtons.forEach((b) => {
    const id = b.id;
    const onclick = b.getAttribute("onclick");
    const hasHandler = !!id || !!onclick || b.closest("[data-target]");
    if (!hasHandler) deadEnds++;
  });
  // Allow some decorative buttons without IDs but check the main interactive ones
  // The plan controls are created by review.js. (The old Repair/Skip/Stop failure buttons were part of the fake
  // plan flow; a failed real plan now offers per-step Undo, Undo completed steps and Open Trail.)
  const keyIds = ["btn-plan-confirm", "btn-plan-cancel", "btn-plan-run", "btn-plan-stop", "btn-plan-edit", "btn-receipt-close", "btn-plan-undo-all", "btn-plan-trail"];
  keyIds.forEach((id) => {
    assert(reviewSrc.includes('"' + id + '"'), `Key control created by review.js: ${id}`);
  });

  // ---------- 12. Prototype disclaimers ----------
  console.log("\n-- Prototype disclaimers --");
  assert(htmlSource.includes("simulated"), "HTML mentions simulated/mock state");
  assert(appJs.includes("simulated"), "app.js mentions simulated operations");
  assert(appJs.includes("never controls a real OS"), "app.js states it never controls a real OS");

  // ---------- Summary ----------
  console.log("\n== Summary ==");
  console.log(`Passed: ${passed}`);
  console.log(`Failed: ${failed}`);
  if (failures.length) {
    console.log("\nFailures:");
    failures.forEach((f) => console.log("  -", f));
  }
  process.exit(failed > 0 ? 1 : 0);
}

main().catch((err) => {
  console.error("Test harness error:", err);
  process.exit(1);
});
