/* aurora-glass/prototype/app.js — the Aurora Glass shell (window.AuroraShell).
 *
 * Owns: screens and navigation history, the app registry, Spaces, Index search, theme/tier/contrast/motion.
 * SIMULATION ONLY: this is a browser prototype. It never controls a real OS or device.
 * Aura plans, approvals and receipts live in review.js / system.js; this file only navigates.
 * Contract: docs/agents/AURORA_ARCHITECTURE.md sections 3 and 4. All text goes through textContent.
 */
(function () {
  "use strict";
  const UI = window.AuroraUI;
  if (!UI) return;
  const { el } = UI;

  const root = document.documentElement;
  const content = document.getElementById("content");

  const state = {
    theme: "light",
    tier: "soft",
    contrast: "normal",
    motion: "normal",
    activeScreen: "screen-home",
    activeSpace: "study",
    indexQuery: "",
  };

  const HOME = "screen-home";
  const MAX_HISTORY = 40;
  const history = [HOME];
  const screenListeners = [];

  // ---------- Helpers ----------
  const announce = (text) => UI.announce(text);
  const showToast = (text, opts) => UI.toast(text, typeof opts === "number" ? { ms: opts } : opts);
  const models = () => window.AuroraModels || null;

  // ---------- Screens and navigation ----------
  function screenEl(id) { return typeof id === "string" ? document.getElementById(id) : null; }

  function addScreen(id, label) {
    const existing = screenEl(id);
    if (existing) return existing;
    const s = el("section", { id, class: "screen", "aria-label": label || id });
    content.appendChild(s);
    return s;
  }

  function syncDock() {
    document.querySelectorAll(".dock-btn").forEach((b) => {
      const isCurrent = b.id !== "dock-back" && b.getAttribute("data-target") === state.activeScreen;
      b.classList.toggle("active", isCurrent);
      if (isCurrent) b.setAttribute("aria-current", "page"); else b.removeAttribute("aria-current");
    });
  }

  function focusScreen(s) {
    const h = s.querySelector("h1");
    if (h) { if (!h.hasAttribute("tabindex")) h.setAttribute("tabindex", "-1"); h.focus({ preventScroll: true }); return; }
    const first = s.querySelector("button, [href], input, select, textarea, [tabindex='0']");
    if (first) first.focus({ preventScroll: true });
  }

  /** Activate a screen. push=false is used by back(). Returns the id shown, or null for an unknown screen. */
  function showScreen(id, opts) {
    const o = Object.assign({ push: true, focus: true }, opts || {});
    const s = screenEl(id);
    if (!s || !s.classList.contains("screen")) return null;
    document.querySelectorAll("#app .screen").forEach((n) => n.classList.remove("active"));
    s.classList.add("active");
    state.activeScreen = id;
    if (o.push) {
      if (id === HOME) history.length = 0;
      if (history[history.length - 1] !== id) history.push(id);
      if (history.length > MAX_HISTORY) history.splice(0, history.length - MAX_HISTORY);
    }
    announce("Screen changed: " + (s.getAttribute("aria-label") || id));
    if (o.focus) focusScreen(s);
    content.scrollTop = 0;
    window.scrollTo(0, 0);
    syncDock();
    screenListeners.slice().forEach((fn) => fn(id));
    return id;
  }

  /** Go to the previous screen (Home when there is none). Returns the screen shown. */
  function back() {
    if (history.length > 1) history.pop();
    const target = history[history.length - 1] || HOME;
    if (!history.length) history.push(HOME);
    return showScreen(target, { push: false }) || showScreen(HOME);
  }

  // ---------- App registry and opening apps ----------
  const appList = [];
  function registerApp(def) {
    if (!def || typeof def.id !== "string" || !/^[a-z][a-z0-9-]{0,31}$/.test(def.id) || typeof def.name !== "string") return false;
    if (appList.some((a) => a.id === def.id)) return false;
    appList.push({
      id: def.id, name: def.name, icon: def.icon || def.id, hue: typeof def.hue === "number" ? def.hue : 215,
      screen: def.screen || null, memMB: def.memMB || 100, keywords: (def.keywords || []).slice(), system: !!def.system,
    });
    return true;
  }
  const apps = () => appList.map((a) => Object.assign({}, a, { keywords: a.keywords.slice() }));
  const app = (id) => { const a = appList.find((x) => x.id === id); return a ? Object.assign({}, a, { keywords: a.keywords.slice() }) : null; };

  /** Open or resume an app's simulated Page and show its screen. Returns {ok, id, created} | {ok:false, reason}. */
  function openApp(id, opts) {
    const o = opts || {};
    const a = appList.find((x) => x.id === id);
    if (!a) return { ok: false, reason: "unknown-app" };
    const m = models();
    if (a.system) {
      // System surfaces (Passport, Trail, Vault) are screens, not Pages. Until their screen exists they say so.
      if (!a.screen || !screenEl(a.screen)) { showToast(a.name + " arrives in a later milestone", { tone: "warn" }); return { ok: false, reason: "not-available" }; }
      if (!o.background) showScreen(a.screen);
      return { ok: true, id: a.id, created: false };
    }
    if (!m) return { ok: false, reason: "not-ready" };
    const had = m.pages.list().some((p) => p.appId === a.id);
    const r = m.pages.open(a.id, a.name, state.activeSpace, a.memMB);
    if (!r.ok) {
      if (r.reason === "low-memory") {
        showScreen("screen-pages");
        showToast("Not enough simulated memory to open " + a.name, { tone: "warn" });
        announce("Low memory. Cannot open " + a.name + ". Save paused pages first.");
      }
      return { ok: false, reason: r.reason || "cannot-open" };
    }
    if (!o.background) {
      if (a.screen && screenEl(a.screen)) showScreen(a.screen); else showScreen("screen-pages");
      announce(a.name + " is open (simulated page)");
    }
    return { ok: true, id: r.id, created: !had };
  }

  // ---------- Spaces ----------
  const BAD_NAME = /[\u0000-\u001f\u007f-\u009f‪-‮⁦-⁩​-‏﻿]/;
  const MAX_SPACES = 8;
  const spaceList = [
    { id: "study", name: "Study", description: "Notes, focus timer, reading list", hue: 215, pinned: ["notes", "clock", "calculator", "settings", "calendar", "files"], builtin: true },
    { id: "work", name: "Work", description: "Projects, files, meetings", hue: 235, pinned: ["files", "notes", "calendar", "settings"], builtin: true },
    { id: "projects", name: "Projects", description: "Active builds, drafts, reviews", hue: 190, pinned: ["files", "notes", "calculator", "settings"], builtin: true },
    { id: "personal", name: "Personal", description: "Journal, photos, reminders", hue: 300, pinned: ["notes", "calendar", "clock", "settings"], builtin: true },
  ];
  const spaceListeners = [];
  const spaces = () => spaceList.map((s) => Object.assign({}, s, { pinned: s.pinned.slice() }));
  const spaceById = (id) => spaceList.find((s) => s.id === id) || null;

  /** Returns an error sentence, or null when the name is acceptable. */
  function validateSpaceName(name, selfId) {
    if (typeof name !== "string") return "Enter a name.";
    const n = name.trim();
    if (!n) return "Enter a name.";
    if (n.length > 24) return "Use 24 characters or fewer.";
    if (BAD_NAME.test(n)) return "That name has characters that are not allowed.";
    if (spaceList.some((s) => s.id !== (selfId || null) && s.name.toLowerCase() === n.toLowerCase())) return "A Space with that name already exists.";
    return null;
  }

  function notifySpaces() { renderSpaces(); spaceListeners.slice().forEach((fn) => fn(state.activeSpace)); }

  function addSpace(name) {
    if (spaceList.length >= MAX_SPACES) return { ok: false, reason: "Up to " + MAX_SPACES + " Spaces are supported." };
    const problem = validateSpaceName(name, null);
    if (problem) return { ok: false, reason: problem };
    const n = name.trim();
    const base = "space-" + (n.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "").slice(0, 20) || "x");
    let id = base, k = 2;
    while (spaceById(id)) id = base + "-" + (k++);
    spaceList.push({ id, name: n, description: "Your own Space", hue: 150 + (spaceList.length * 37) % 200, pinned: ["notes", "files", "settings"], builtin: false });
    notifySpaces();
    return { ok: true, id };
  }

  function renameSpace(id, name) {
    const s = spaceById(id);
    if (!s) return { ok: false, reason: "That Space does not exist." };
    const problem = validateSpaceName(name, id);
    if (problem) return { ok: false, reason: problem };
    s.name = name.trim();
    notifySpaces();
    return { ok: true, id };
  }

  function switchSpace(id, opts) {
    const s = spaceById(id);
    if (!s) return { ok: false, reason: "unknown-space" };
    state.activeSpace = id;
    notifySpaces();
    if (!(opts && opts.quiet)) { announce("Switched to " + s.name + " Space"); showToast("Now in " + s.name); }
    return { ok: true, id };
  }

  function renderSpaces() {
    const grid = document.querySelector(".space-grid");
    if (grid) {
      grid.replaceChildren();
      spaceList.forEach((sp) => {
        const active = sp.id === state.activeSpace;
        grid.appendChild(el("article", {
          class: "space-card-item" + (active ? " selected" : ""), role: "listitem", tabindex: 0, "data-space": sp.id,
          "aria-label": sp.name + " Space — " + (active ? "active" : "inactive"),
          onclick: () => switchSpace(sp.id),
          onkeydown: (e) => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); switchSpace(sp.id); } },
        }, [
          el("div", { class: "space-badge", text: sp.name.charAt(0).toUpperCase() }),
          el("h3", { text: sp.name }),
          el("p", { text: sp.description }),
          el("span", { class: "space-state", text: active ? "Active" : "Inactive" }),
        ]));
      });
    }
    const chip = document.querySelector(".space-chip");
    if (chip) {
      chip.dataset.space = state.activeSpace;
      chip.classList.add("selected");
      const label = chip.querySelector("span:last-child");
      if (label) label.textContent = spaceById(state.activeSpace).name;
    }
  }

  // ---------- Index ----------
  // Screens may contribute searchable items: fn(query) -> [{kind, label, meta, action?}]. Text only, never HTML.
  const indexSources = [];
  const mockIndex = [
    { kind: "App", label: "Notes", meta: "Open notes" },
    { kind: "App", label: "Files", meta: "Browse files" },
    { kind: "App", label: "Calculator", meta: "Calculate" },
    { kind: "App", label: "Clock", meta: "Timer & alarm" },
    { kind: "Setting", label: "Display", meta: "Brightness, dark mode" },
    { kind: "Setting", label: "Passport", meta: "Permissions & access" },
    { kind: "Action", label: "Quick Center", meta: "Tiles & sliders" },
    { kind: "Action", label: "Vault export", meta: "Export user files" },
  ];

  function renderIndex() {
    const box = document.getElementById("index-results");
    const input = document.getElementById("index-query");
    if (!box || !input) return;
    const q = state.indexQuery.toLowerCase();
    box.replaceChildren();
    let results = mockIndex.filter((i) => i.label.toLowerCase().includes(q) || i.meta.toLowerCase().includes(q));
    indexSources.forEach((fn) => { results = results.concat(fn(state.indexQuery) || []); });
    input.setAttribute("aria-expanded", results.length > 0 ? "true" : "false");
    results.forEach((item) => {
      box.appendChild(el("div", {
        class: "index-result", role: "option", tabindex: 0, "aria-label": item.kind + ": " + item.label + ", " + item.meta,
        onclick: () => selectIndex(item),
        onkeydown: (e) => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); selectIndex(item); } },
      }, [el("strong", { text: item.label }), el("span", { class: "meta", text: item.kind + " · " + item.meta })]));
    });
    input.setAttribute("aria-controls", results.length ? box.id : "");
    announce(results.length + " results");
  }

  function selectIndex(item) {
    if (typeof item.action === "function") { announce("Opening " + item.label); item.action(); return; }
    announce("Selected " + item.label);
    showToast("Opened " + item.label + " (simulated)");
    showScreen(HOME);
  }

  /** Show Index, set and focus the query, and render the results. */
  function openIndex(query) {
    const q = typeof query === "string" ? query : "";
    showScreen("screen-index", { focus: false });
    const input = document.getElementById("index-query");
    state.indexQuery = q;
    if (input) { input.value = q; input.focus(); }
    renderIndex();
  }

  // ---------- Appearance ----------
  function applyTheme() {
    root.setAttribute("data-theme", state.theme);
    root.setAttribute("data-glass-tier", state.tier);
    root.setAttribute("data-contrast", state.contrast);
    if (state.motion === "reduce") root.setAttribute("data-motion", "reduce"); else root.removeAttribute("data-motion");
    syncDock();
  }

  // ---------- Wiring ----------
  function wireNav() {
    const go = (btnId, target) => { const b = document.getElementById(btnId); if (b) b.addEventListener("click", () => showScreen(target)); };
    const goBack = (btnId) => { const b = document.getElementById(btnId); if (b) b.addEventListener("click", () => back()); };
    go("dock-home", HOME);
    go("dock-spaces", "screen-spaces");
    go("dock-pages", "screen-pages");
    go("btn-quick-center", "screen-quick");
    go("btn-switch-space", "screen-spaces");
    goBack("dock-back");
    ["btn-pages-back", "btn-quick-back", "btn-index-back", "btn-spaces-back", "btn-settings-back"].forEach(goBack);
    const ask = (id) => { const b = document.getElementById(id); if (b) b.addEventListener("click", () => openIndex("")); };
    ask("btn-index"); ask("dock-ask");

    // Pinned tools: one handler, driven by data-app.
    document.querySelectorAll(".pin-btn[data-app]").forEach((b) => b.addEventListener("click", () => openApp(b.dataset.app)));

    const input = document.getElementById("index-query");
    if (input) {
      input.addEventListener("input", (e) => { state.indexQuery = e.target.value; renderIndex(); });
      input.addEventListener("keydown", (e) => {
        if (e.key === "Enter") { e.preventDefault(); const first = document.querySelector("#index-results .index-result"); if (first) first.click(); }
      });
    }
    const clear = document.getElementById("btn-index-clear");
    if (clear) clear.addEventListener("click", () => { if (input) { input.value = ""; input.focus(); } state.indexQuery = ""; renderIndex(); });

    // Aura suggestion on Home: opens a plan to REVIEW. Nothing runs from this button.
    const btnAura = document.getElementById("btn-aura-study");
    if (btnAura) btnAura.addEventListener("click", () => { if (window.AuroraReview) window.AuroraReview.ask("Prepare my phone for studying"); });

    // Glass Corner (the held-inbox popover arrives in a later step).
    const gc = document.querySelector(".glass-corner");
    if (gc) {
      gc.addEventListener("click", () => {
        const expanded = gc.getAttribute("aria-expanded") === "true";
        gc.setAttribute("aria-expanded", String(!expanded));
        announce(expanded ? "Glass Corner closed" : "Glass Corner opened — notifications");
      });
      gc.addEventListener("keydown", (e) => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); gc.click(); } });
    }
  }

  function wireKeyboard() {
    document.addEventListener("keydown", (e) => {
      if (e.key === "Escape" && !e.defaultPrevented) {
        // Dialogs handle their own Escape (capture phase, preventDefault). Next layer: the Glass Corner, then Back.
        const gc = document.querySelector(".glass-corner");
        if (gc && gc.getAttribute("aria-expanded") === "true") { gc.setAttribute("aria-expanded", "false"); gc.focus(); announce("Glass Corner closed"); return; }
        if (state.activeScreen !== HOME) back();
      }
      if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "k") { e.preventDefault(); openIndex(state.indexQuery); }
    });
  }

  window.AuroraShell = {
    state,
    showScreen, back, addScreen,
    registerApp, apps, app, openApp,
    spaces, addSpace, renameSpace, switchSpace, validateSpaceName,
    openIndex, registerIndexSource(fn) { indexSources.push(fn); }, refreshIndex() { renderIndex(); },
    onScreenShow(fn) { screenListeners.push(fn); },
    onSpaceChange(fn) { spaceListeners.push(fn); },
    setAppearance(theme) { state.theme = theme === "dark" ? "dark" : "light"; applyTheme(); },
    setTier(tier) { state.tier = tier === "clear" ? "clear" : "soft"; applyTheme(); },
    setContrast(c) { state.contrast = c === "high" ? "high" : "normal"; applyTheme(); },
    setMotion(m) { state.motion = m === "reduce" ? "reduce" : "normal"; applyTheme(); },
    announce, showToast,
  };

  // Built-in apps. Hues follow docs/agents/AURORA_UI_TASKS.md S2.
  [
    { id: "notes", name: "Notes", hue: 42, memMB: 180, keywords: ["write", "text", "memo"] },
    { id: "files", name: "Files", hue: 212, memMB: 240, keywords: ["documents", "folder", "vault"] },
    { id: "calculator", name: "Calculator", hue: 228, memMB: 60, keywords: ["math", "numbers"] },
    { id: "clock", name: "Clock", hue: 262, memMB: 90, keywords: ["timer", "stopwatch", "alarm"] },
    { id: "settings", name: "Settings", hue: 220, memMB: 120, screen: "screen-settings", keywords: ["preferences", "options"] },
    { id: "calendar", name: "Calendar", hue: 6, memMB: 150, keywords: ["events", "schedule"] },
    { id: "passport", name: "Passport", hue: 152, system: true, keywords: ["permissions", "privacy", "access"] },
    { id: "trail", name: "Trail", hue: 280, system: true, keywords: ["history", "undo", "receipts"] },
    { id: "vault", name: "Vault", hue: 188, system: true, keywords: ["files", "storage"] },
  ].forEach(registerApp);

  function init() {
    wireNav();
    wireKeyboard();
    renderSpaces();
    applyTheme();
    showScreen(HOME);
    announce("Aurora Glass prototype ready. Home screen.");
  }

  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", init);
  else init();
})();
