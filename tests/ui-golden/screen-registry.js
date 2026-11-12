/**
 * tests/ui-golden/screen-registry.js — the screens the layout matrix visits.
 * Each entry: { id, label, root, open(page) } where root is a CSS selector for the region to inspect
 * ("#screen-home", or ".dialog" for a dialog) and open(page) brings it on screen.
 * New apps add their screens with registerScreen(...) so they join the matrix (docs/agents/AURORA_ARCHITECTURE.md section 9).
 * Not a test file (name does not match *.test.js).
 */
const SCREENS = [];

function registerScreen(def) {
  if (!def || !def.id || typeof def.open !== "function") throw new Error("registerScreen needs { id, open }");
  if (SCREENS.some((s) => s.id === def.id)) throw new Error("screen already registered: " + def.id);
  SCREENS.push(Object.assign({ root: "#" + def.id, label: def.id }, def));
}

const show = (id) => (page) => page.evaluate((s) => window.AuroraShell.showScreen(s), id);
const ev = (page, fn, arg) => page.evaluate(fn, arg);

registerScreen({ id: "screen-home", label: "Home", open: show("screen-home") });
registerScreen({ id: "screen-spaces", label: "Spaces", open: async (page) => { await ev(page, () => { const S = window.AuroraShell; if (S.spaces().length < 6) { S.addSpace("A fairly long Space name"); S.addSpace("Reading list"); } S.showScreen("screen-spaces"); }); } });
registerScreen({ id: "screen-index", label: "Index (empty)", open: (page) => ev(page, () => window.AuroraShell.openIndex("")) });
registerScreen({ id: "screen-index-results", label: "Index (results)", root: "#screen-index", open: (page) => ev(page, () => window.AuroraShell.openIndex("e")) });
registerScreen({ id: "screen-index-aura", label: "Index (Aura row)", root: "#screen-index", open: (page) => ev(page, () => window.AuroraShell.openIndex("prepare my phone for studying")) });
registerScreen({ id: "screen-pages", label: "Pages", open: show("screen-pages") });
registerScreen({ id: "screen-quick", label: "Quick Center", open: show("screen-quick") });
registerScreen({ id: "screen-settings", label: "Settings", open: show("screen-settings") });
registerScreen({ id: "screen-aura-review", label: "Aura review", root: "#screen-aura", open: async (page) => { await ev(page, () => window.AuroraReview.ask("prepare my phone for studying")); } });
registerScreen({
  id: "screen-aura-receipt", label: "Aura receipt", root: "#screen-aura",
  open: async (page) => {
    await ev(page, () => window.AuroraReview.ask("prepare my phone for studying"));
    await page.locator("#btn-plan-confirm").click();
    await page.locator("#btn-plan-run").click();
    await page.locator("#plan-receipt").waitFor({ timeout: 8000 });
  },
});
registerScreen({
  id: "dialog-review", label: "Review dialog (S2)", root: ".dialog",
  open: async (page) => { await ev(page, () => { window.__dlg = window.AuroraSystem.request({ type: "file.move", arguments: { id: "timetable", destination: "work" } }, { source: "matrix" }); }); await page.locator("#review-dialog").waitFor(); },
  close: async (page) => { await page.keyboard.press("Escape"); await page.locator(".dialog").waitFor({ state: "detached" }); },
});
registerScreen({
  id: "dialog-delete", label: "Delete dialog (S3, hold)", root: ".dialog",
  open: async (page) => { await ev(page, () => { window.__dlg = window.AuroraSystem.request({ type: "file.delete", arguments: { id: "timetable" } }, { source: "matrix" }); }); await page.locator("#review-dialog").waitFor(); },
  close: async (page) => { await page.keyboard.press("Escape"); await page.locator(".dialog").waitFor({ state: "detached" }); },
});

registerScreen({ id: "screen-passport-overview", label: "Passport (overview)", root: "#screen-passport", open: (page) => ev(page, () => { window.AuroraShell.openApp("passport"); window.AuroraPassport.setTab("overview"); }) });
registerScreen({ id: "screen-passport-apps", label: "Passport (apps)", root: "#screen-passport", open: (page) => ev(page, () => { window.AuroraShell.openApp("passport"); window.AuroraPassport.setTab("apps"); }) });
registerScreen({ id: "screen-passport-activity", label: "Passport (activity)", root: "#screen-passport", open: (page) => ev(page, () => { window.AuroraShell.openApp("passport"); window.AuroraPassport.setTab("activity"); }) });

module.exports = { SCREENS, registerScreen };
