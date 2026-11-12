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

  function pageCount(spaceId) { const m = models(); return m ? m.pages.list().filter((p) => p.space === spaceId).length : 0; }

  function renderSpaces() {
    const screen = $("screen-spaces");
    if (screen) UI.keepFocus(screen, renderSpaceGrid, "#btn-space-new");
    const chip = document.querySelector(".space-chip");
    if (chip) {
      chip.dataset.space = state.activeSpace;
      chip.classList.add("selected");
      const label = chip.querySelector("span:last-child");
      if (label) label.textContent = spaceById(state.activeSpace).name;
    }
  }

  function renderSpaceGrid() {
    const grid = $("space-grid");
    if (!grid) return;
    grid.replaceChildren();
    spaceList.forEach((sp) => {
      const active = sp.id === state.activeSpace;
      const n = pageCount(sp.id);
      const pins = sp.pinned.map((id) => appList.find((a) => a.id === id)).filter(Boolean);
      grid.appendChild(el("article", { class: "space-card-item" + (active ? " selected" : ""), role: "listitem", "data-space": sp.id }, [
        el("div", { class: "space-card-head" }, [
          el("span", { class: "app-tile space-badge", style: { "--h": sp.hue }, "aria-hidden": "true", text: sp.name.charAt(0).toUpperCase() }),
          el("div", { class: "space-card-title" }, [el("h3", { text: sp.name }), el("p", { class: "space-card-desc", text: sp.description })]),
        ]),
        el("div", { class: "space-pins", role: "group", "aria-label": "Pinned in " + sp.name + ": " + pins.map((a) => a.name).join(", ") }, pins.map((a) => UI.appTile(a.icon, a.hue, "sm"))),
        el("p", { class: "space-pages", text: n === 1 ? "1 page open" : n + " pages open" }),
        el("div", { class: "space-card-foot" }, [
          el("button", {
            type: "button", class: "space-switch", "data-fk": "space:" + sp.id, "aria-current": active ? "true" : undefined,
            "aria-label": active ? sp.name + " Space, active" : "Switch to " + sp.name + " Space", onclick: () => switchSpace(sp.id),
          }, active ? [UI.icon("check"), el("span", { class: "space-state", text: "Active" })] : [el("span", { class: "space-state", text: "Switch to this Space" })]),
          el("button", { type: "button", class: "btn outline btn-icon space-rename", "data-fk": "rename:" + sp.id, "aria-label": "Rename " + sp.name, title: "Rename", onclick: () => renameFlow(sp.id) }, [UI.icon("edit")]),
        ]),
      ]));
    });
  }

  async function newSpaceFlow() {
    if (spaceList.length >= MAX_SPACES) { showToast("Up to " + MAX_SPACES + " Spaces are supported. Rename one instead.", { tone: "warn" }); return; }
    const name = await UI.prompt({ title: "New Space", label: "Space name", confirmLabel: "Create", maxLength: 24, validate: (t) => validateSpaceName(t, null) });
    if (name === null) return;
    const r = addSpace(name);
    if (r.ok) { announce("Space " + name + " created"); showToast("Created " + name); const b = document.querySelector('[data-fk="space:' + CSS.escape(r.id) + '"]'); if (b) b.focus(); }
    else showToast(r.reason, { tone: "error" });
  }

  async function renameFlow(id) {
    const sp = spaceById(id);
    if (!sp) return;
    const name = await UI.prompt({ title: "Rename " + sp.name, label: "Space name", value: sp.name, confirmLabel: "Rename", maxLength: 24, validate: (t) => validateSpaceName(t, id) });
    if (name === null) return;
    const r = renameSpace(id, name);
    if (r.ok) { announce("Space renamed to " + name); showToast("Renamed to " + name); }
    else showToast(r.reason, { tone: "error" });
  }

  // ---------- Index (Ask + Search) ----------
  // Sources contribute items; each item is {group, kind, label, meta, tile|icon, chip?, risk?, muted?, rank, action}.
  // Screens may add sources: fn(query) -> [{kind, label, meta, action}] (text only, never HTML).
  const indexSources = [];
  const INDEX_GROUPS = [["aura", "Aura"], ["suggest", "Try asking"], ["apps", "Apps"], ["settings", "Settings"], ["files", "Files"], ["actions", "Actions"]];
  const KIND_GROUP = { App: "apps", Setting: "settings", File: "files", Action: "actions", Aura: "aura" };
  const SUGGESTIONS = ["Prepare my phone for studying", "Start a 10 minute timer", "Turn on focus mode", "Open Notes"];
  const SYSTEM_ACTIONS = [
    { label: "Quick Center", meta: "Brightness, volume and quick toggles", icon: "quick", keywords: ["tiles", "sliders", "wifi", "brightness", "volume", "battery"], go: "screen-quick" },
    { label: "Pages", meta: "Open apps and simulated memory", icon: "pages", keywords: ["multitasking", "memory", "switch"], go: "screen-pages" },
    { label: "Spaces", meta: "Switch, create or rename a Space", icon: "spaces", keywords: ["workspace", "context"], go: "screen-spaces" },
    { label: "Home", meta: "Go to the Desk", icon: "home", keywords: ["desk", "start"], go: "screen-home" },
  ];
  const MAX_PER_GROUP = 6, MAX_TOTAL = 20;
  const idx = { items: [], active: -1, navigated: false };
  let idxAnnounce = null;

  /** 0 label prefix, 1 word prefix, 2 substring, 3 keyword; -1 no match. */
  function rankOf(q, label, keywords) {
    if (!q) return 0;
    const l = String(label).toLowerCase();
    if (l.startsWith(q)) return 0;
    if (l.split(/[\s\-_.·]+/).some((w) => w.startsWith(q))) return 1;
    if (l.includes(q)) return 2;
    if ((keywords || []).some((k) => String(k).toLowerCase().includes(q))) return 3;
    return -1;
  }

  function appItems(q) {
    const out = [];
    appList.forEach((a) => {
      const r = rankOf(q, a.name, a.keywords);
      if (r < 0) return;
      const later = a.system && !(a.screen && screenEl(a.screen));
      out.push({ group: "apps", kind: "App", label: a.name, meta: later ? "Arrives in a later milestone" : "Open " + a.name, tile: a, rank: r, muted: later, action: () => openApp(a.id) });
    });
    return out;
  }

  function fileItems(q) {
    if (!q || !window.AuroraSystem) return [];
    const out = [];
    window.AuroraSystem.files.list().forEach((f) => {
      const sp = spaceName(f.space);
      const r = rankOf(q, f.name, [f.kind, sp]);
      if (r < 0) return;
      out.push({ group: "files", kind: "File", label: f.name, meta: sp + " · " + Math.max(1, Math.round(f.size / 1024)) + " KB · mock file", icon: f.kind === "image" ? "image" : "file", rank: r, action: () => openApp("vault") });
    });
    return out;
  }

  function actionItems(q) {
    const out = [];
    SYSTEM_ACTIONS.forEach((a) => {
      const r = rankOf(q, a.label, a.keywords.concat([a.meta]));
      if (r < 0 || !q) return;
      out.push({ group: "actions", kind: "Action", label: a.label, meta: a.meta, icon: a.icon, rank: r, action: () => showScreen(a.go) });
    });
    return out;
  }

  function spaceName(id) { const s = spaceById(id); return s ? s.name : id; }

  function sourceItems(q) {
    const out = [];
    indexSources.forEach((fn) => {
      let list = [];
      try { list = fn(q) || []; } catch (e) { list = []; }
      list.forEach((it) => {
        const r = rankOf(q.toLowerCase(), it.label, []);
        out.push({ group: KIND_GROUP[it.kind] || "actions", kind: it.kind, label: it.label, meta: it.meta, icon: it.icon || "settings", rank: r < 0 ? 4 : r, action: it.action });
      });
    });
    return out;
  }

  function auraItem(q) {
    const R = window.AuroraReview, m = models();
    if (!R || !q) return null;
    const d = R.describeCommand(q);
    if (!d) return null;
    if (m && m.qs.get("auraMode") === "off") return { group: "aura", kind: "Aura", label: "Aura is off", meta: "Turn it on in Quick Center to run commands", icon: "aura", muted: true, rank: 0, action: () => showScreen("screen-quick") };
    const pv = d.preview || {};
    const steps = pv.steps || [];
    let meta;
    if (steps.some((s) => s.decision === "deny")) meta = "Blocked in Passport";
    else if (steps.some((s) => s.supported === false)) meta = "Not available in this prototype";
    else if (d.kind === "plan") meta = "Opens a plan to review. Nothing runs yet.";
    else if (!pv.needsReview) meta = pv.expectedRollback === "full" ? "Runs now. You can undo it." : "Runs now";
    else meta = "Asks you to review first";
    return { group: "aura", kind: "Aura", label: d.title, meta, icon: "aura", risk: pv.risk, rank: 0, action: () => runAura(q) };
  }

  /** Quick Command from Index. A plan opens the trusted review screen; an S0 action runs and offers Undo only if the broker says it can. */
  async function runAura(text) {
    const R = window.AuroraReview, S = window.AuroraSystem;
    if (!R) return;
    const out = await R.ask(text);
    if (!out || !out.ok || !out.steps) return;                       // plans (no steps yet), cancelled, refused or failed: already explained
    const step = out.steps.find((x) => x.status === "done" && x.receiptId);
    if (!step) return;
    const rec = S.receipts().find((r) => r.id === step.receiptId);
    const title = rec ? rec.describe.title : "Done";
    if (rec && rec.undoState === "available") {
      UI.toast(title + " · done", { action: { label: "Undo", run: async () => { const u = await S.undo(step.receiptId); UI.toast(u.ok ? "Undone: " + title : u.text, { tone: u.ok ? "ok" : "error" }); } } });
    } else UI.toast(title + " · done", { tone: "ok" });
  }

  function collectIndex(q) {
    const lq = q.trim().toLowerCase();
    const items = [];
    if (!lq) {
      SUGGESTIONS.forEach((t) => items.push({ group: "suggest", kind: "Suggestion", label: t, meta: "Try this command", icon: "aura", rank: 0, action: () => openIndex(t) }));
      return items.concat(appItems(""));
    }
    const a = auraItem(q.trim()); if (a) items.push(a);
    return items.concat(appItems(lq), sourceItems(q.trim()), fileItems(lq), actionItems(lq));
  }

  function groupAndLimit(items, perGroup) {
    const out = [];
    INDEX_GROUPS.forEach(([g]) => {
      const mine = items.filter((i) => i.group === g).map((i, n) => Object.assign({ n }, i)).sort((x, y) => x.rank - y.rank || x.n - y.n).slice(0, perGroup);
      mine.forEach((i) => { if (out.length < MAX_TOTAL) out.push(i); });
    });
    return out;
  }

  function optionEl(it, i) {
    const lead = it.tile ? UI.appTile(it.tile.icon, it.tile.hue, "sm") : el("span", { class: "index-icon", "aria-hidden": "true" }, [UI.icon(it.icon || "info")]);
    const trailing = it.risk ? UI.riskBadge(it.risk, true) : (it.muted ? UI.chip("Not yet", "muted") : null);
    return el("div", {
      class: "index-result" + (it.muted ? " is-muted" : ""), role: "option", id: "idx-opt-" + i, "aria-selected": "false", "aria-describedby": "idx-g-" + it.group, "data-group": it.group,
      onclick: () => activateIndex(i),
    }, [lead, el("span", { class: "index-text" }, [el("strong", { class: "index-title", text: it.label }), el("span", { class: "meta", text: it.kind + " · " + it.meta })]), trailing]);
  }

  function renderIndex() {
    const box = $("index-results"), input = $("index-query");
    if (!box || !input) return;
    const q = state.indexQuery;
    const items = groupAndLimit(collectIndex(q), q.trim() ? MAX_PER_GROUP : 12);
    idx.items = items; idx.navigated = false;
    box.replaceChildren();
    let lastGroup = null;
    items.forEach((it, i) => {
      if (it.group !== lastGroup) {
        lastGroup = it.group;
        box.appendChild(el("div", { class: "index-group", role: "presentation", id: "idx-g-" + it.group, text: INDEX_GROUPS.find((g) => g[0] === it.group)[1] }));
      }
      box.appendChild(optionEl(it, i));
    });
    const emptyBox = $("index-empty");
    if (emptyBox) {
      emptyBox.replaceChildren(); emptyBox.hidden = items.length > 0;
      if (!items.length) {
        const row = el("div", { class: "index-suggest-row" }, SUGGESTIONS.slice(0, 3).map((t) => el("button", { type: "button", class: "btn outline btn-small", text: t, onclick: () => openIndex(t) })));
        emptyBox.appendChild(UI.emptyState("search", "No results", "Nothing matches \u201c" + q.trim() + "\u201d. Try asking:", row));
      }
    }
    box.hidden = items.length === 0;
    const clear = $("btn-index-clear"); if (clear) clear.hidden = !q;
    input.setAttribute("aria-expanded", items.length ? "true" : "false");
    setActive(items.length ? 0 : -1);
    clearTimeout(idxAnnounce);
    idxAnnounce = setTimeout(() => announce(items.length ? items.length + (items.length === 1 ? " result" : " results") : "No results"), 250);
  }

  function setActive(i, scroll) {
    const input = $("index-query"), box = $("index-results");
    if (!box) return;
    idx.active = i;
    box.querySelectorAll(".index-result").forEach((n, k) => { const on = k === i; n.classList.toggle("is-active", on); n.setAttribute("aria-selected", String(on)); if (on && scroll) n.scrollIntoView({ block: "nearest" }); });
    if (input) { if (i >= 0) input.setAttribute("aria-activedescendant", "idx-opt-" + i); else input.removeAttribute("aria-activedescendant"); }
  }

  function activateIndex(i) {
    const it = idx.items[i];
    if (!it || typeof it.action !== "function") return;
    announce("Opening " + it.label);
    it.action();
  }

  function onIndexKey(e) {
    const n = idx.items.length;
    if (e.key === "ArrowDown" || e.key === "ArrowUp") {
      if (!n) return;
      e.preventDefault(); idx.navigated = true;
      setActive(e.key === "ArrowDown" ? (idx.active + 1) % n : (idx.active - 1 + n) % n, true);
    } else if ((e.key === "Home" || e.key === "End") && idx.navigated && n) {
      e.preventDefault(); setActive(e.key === "Home" ? 0 : n - 1, true);
    } else if (e.key === "Enter") {
      e.preventDefault();
      if (n) activateIndex(idx.active >= 0 ? idx.active : 0);
    }
  }

  /** Show Index, set and focus the query, and render the results. */
  function openIndex(query) {
    const q = typeof query === "string" ? query : "";
    showScreen("screen-index", { focus: false });
    const input = $("index-query");
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

  // ---------- Home and Dock (rendered from real models; nothing here is sample data) ----------
  const $ = (id) => document.getElementById(id);
  const appFor = (appId) => appList.find((a) => a.id === appId) || null;
  const STATE_TEXT = { running: "Running", paused: "Paused", saved: "Saved" };

  /** Running pages first, then paused; saved (frozen) pages are not "in progress". */
  function livePages() {
    const m = models();
    if (!m) return [];
    const all = m.pages.list().filter((p) => p.state === "running" || p.state === "paused");
    return all.filter((p) => p.state === "running").concat(all.filter((p) => p.state === "paused"));
  }

  function hydrateIcons(rootNode) {
    (rootNode || document).querySelectorAll("[data-icon]:empty").forEach((n) => n.appendChild(UI.icon(n.dataset.icon)));
  }

  // -- clock: re-rendered on the minute boundary, never on a 1 s interval
  let clockTimer = null;
  function renderClock() {
    const now = new Date();
    const time = now.toLocaleTimeString([], { hour: "numeric", minute: "2-digit" });
    const clock = $("home-clock"), date = $("home-date"), dockTime = $("dock-time");
    if (clock) clock.textContent = time;
    if (date) date.textContent = now.toLocaleDateString([], { weekday: "long", day: "numeric", month: "long" });
    if (dockTime) { dockTime.textContent = time; dockTime.setAttribute("datetime", now.toISOString()); }
    clearTimeout(clockTimer);
    clockTimer = setTimeout(renderClock, 60000 - (now.getTime() % 60000) + 50);
  }

  function appTileFor(a, size) { return UI.appTile(a ? a.icon : "info", a ? a.hue : 215, size); }

  function renderHomeContinue() {
    const box = $("home-continue-list");
    if (!box) return;
    box.replaceChildren();
    const rows = livePages().slice(0, 4);
    if (!rows.length) {
      box.appendChild(UI.emptyState("pages", "Nothing in progress", "Open an app and it will wait for you here.",
        UI.button("Open Notes", { kind: "accent", icon: "notes", id: "btn-continue-notes", onclick: () => openApp("notes") })));
      return;
    }
    const list = el("div", { class: "list", role: "list", "aria-label": "Pages in progress" });
    rows.forEach((p) => {
      const a = appFor(p.appId), sp = spaceById(p.space);
      list.appendChild(el("button", {
        type: "button", class: "list-row", role: "listitem", id: "continue-" + p.appId, "data-page-id": p.id, "data-fk": "cont:" + p.id,
        "aria-label": "Open " + p.name + ", " + (sp ? sp.name : p.space) + " Space, " + STATE_TEXT[p.state].toLowerCase(),
        onclick: () => openApp(p.appId),
      }, [
        appTileFor(a, "sm"),
        el("span", { class: "row-main" }, [el("span", { class: "row-title", text: p.name }), el("span", { class: "row-meta", text: (sp ? sp.name : p.space) + " · " + STATE_TEXT[p.state] })]),
        el("span", { class: "row-trailing" }, [UI.icon(p.state === "running" ? "play" : "pause"), UI.icon("chevron")]),
      ]));
    });
    box.appendChild(list);
  }

  function renderHomePins() {
    const box = $("home-pins");
    if (!box) return;
    box.replaceChildren();
    const sp = spaceById(state.activeSpace);
    (sp ? sp.pinned : []).map(appFor).filter(Boolean).forEach((a) => {
      box.appendChild(el("button", { type: "button", class: "pin-btn", "data-app": a.id, "data-fk": "pin:" + a.id, "aria-label": "Open " + a.name, onclick: () => openApp(a.id) },
        [UI.appTile(a.icon, a.hue), el("span", { class: "pin-label", text: a.name })]));
    });
  }

  function renderHomeSpace() {
    const body = $("home-space-body"), m = models();
    if (!body) return;
    const sp = spaceById(state.activeSpace);
    body.replaceChildren();
    if (!sp) return;
    const open = m ? m.pages.list().filter((p) => p.space === sp.id).length : 0;
    body.appendChild(el("div", { class: "space-summary" }, [
      el("span", { class: "app-tile space-badge", style: { "--h": sp.hue }, "aria-hidden": "true", text: sp.name.charAt(0).toUpperCase() }),
      el("div", { class: "space-summary-text" }, [
        el("p", { class: "space-name", id: "home-space-name", text: sp.name }),
        el("p", { class: "space-desc", text: sp.description }),
        el("p", { class: "space-count", id: "home-space-count", text: open === 1 ? "1 page open here" : open + " pages open here" }),
      ]),
    ]));
  }

  function renderHomeChips() {
    const m = models();
    const focus = $("home-focus-chip");
    if (focus) focus.hidden = !(m && m.qs.get("focus"));
    const aura = $("home-aura");
    if (aura) aura.hidden = !!(m && m.qs.get("auraMode") === "off");
  }

  function renderHome() {
    const home = $("screen-home");
    if (!home) return;
    UI.keepFocus(home, () => { renderHomeContinue(); renderHomePins(); renderHomeSpace(); renderHomeChips(); }, "#btn-ask");
  }

  // -- Aurora Horizon: one status source (AuroraSystem "status"); text is set with textContent
  const horizon = { text: "Ready", running: false, done: 0, total: 0 };
  let horizonReset = null;
  function renderHorizon() {
    const text = $("horizon-text"), meter = $("horizon-meter"), fill = $("horizon-fill"), dot = $("horizon-dot");
    const pct = horizon.total > 0 ? Math.round((horizon.done / horizon.total) * 100) : 0;
    if (text) text.textContent = horizon.text;
    if (meter) { meter.hidden = !horizon.running; meter.setAttribute("aria-valuenow", String(pct)); meter.setAttribute("aria-valuetext", horizon.done + " of " + horizon.total + " steps"); }
    if (fill) fill.style.width = pct + "%";
    if (dot) dot.classList.toggle("horizon-active", horizon.running);
    const dh = $("dock-horizon"), df = $("dock-horizon-fill");
    if (dh) dh.hidden = !horizon.running;
    if (df) df.style.width = pct + "%";
  }
  function onStatus(s) {
    if (!s || typeof s.text !== "string") return;
    clearTimeout(horizonReset);
    horizon.text = s.text.slice(0, 120); horizon.running = !!s.running; horizon.done = s.done || 0; horizon.total = s.total || 0;
    renderHorizon();
    if (!horizon.running) horizonReset = setTimeout(() => { horizon.text = "Ready"; horizon.done = 0; horizon.total = 0; renderHorizon(); }, 12000);
  }

  // -- Dock: running apps (>= 768 px via CSS) and the tray
  function renderDockApps() {
    const box = $("dock-apps");
    if (!box) return;
    box.replaceChildren();
    const pages = livePages();
    pages.slice(0, 6).forEach((p) => {
      const a = appFor(p.appId);
      box.appendChild(el("li", {}, [el("button", {
        type: "button", class: "dock-app", "data-page-id": p.id, "data-state": p.state, "aria-label": p.name + ", " + p.state, title: p.name + " · " + p.state,
        onclick: () => openApp(p.appId),
      }, [appTileFor(a, "sm"), el("span", { class: "dock-state dock-state-" + p.state, "aria-hidden": "true" })])]));
    });
    if (pages.length > 6) box.appendChild(el("li", {}, [el("button", { type: "button", class: "dock-app dock-more", "aria-label": (pages.length - 6) + " more open apps", onclick: () => showScreen("screen-pages") }, [el("span", { text: "+" + (pages.length - 6) })])]));
  }

  function startHome() {
    hydrateIcons();
    renderClock();
    renderHome();
    renderDockApps();
    renderHorizon();
    const m = models();
    if (m) {
      m.pages.onChange(() => { renderHome(); renderDockApps(); renderSpaces(); });
      m.qs.onChange((k) => { if (k === "focus" || k === "auraMode") renderHomeChips(); });
    }
    spaceListeners.push(() => renderHome());
    if (window.AuroraSystem) window.AuroraSystem.on("status", onStatus);
    screenListeners.push((id) => { if (id === HOME) renderHome(); });
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

    ask("btn-ask");
    const addBtn = $("btn-space-new"); if (addBtn) addBtn.addEventListener("click", newSpaceFlow);
    go("btn-continue-all", "screen-pages");
    go("dock-quick", "screen-quick");

    const input = document.getElementById("index-query");
    if (input) {
      input.addEventListener("input", (e) => { state.indexQuery = e.target.value; renderIndex(); });
      input.addEventListener("keydown", onIndexKey);
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
    applyTheme();
    startHome();
    renderSpaces();
    showScreen(HOME);
    announce("Aurora Glass prototype ready. Home screen.");
  }

  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", init);
  else init();
})();
