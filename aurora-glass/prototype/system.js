/* aurora-glass/prototype/system.js — the prototype's single Action Broker instance and its simulated world
 * (window.AuroraSystem).
 *
 * SIMULATION ONLY. "Files" here are mock objects in this page's memory. They are NOT AuroraFS, not the host
 * disk, and nothing is persisted. The broker contract (broker.js) is the reviewed security model; this file
 * only gives it adapters that drive the visible prototype, plus the permission catalogue Passport shows.
 *
 * WHO MAY DO WHAT
 *  - Any screen may call System.request(payload). It can only PROPOSE. Whether it runs is decided by the broker
 *    and, when review is needed, by the person through the review surface (review.js).
 *  - The trusted-UI facet is handed out exactly once (takeTrusted) to review.js. No app screen may hold it.
 *  - Permissions are changed only through System.permissions.set(), which applies the policy below and logs
 *    every change. S3 permissions can never be set to "allow".
 */
(function () {
  "use strict";
  const Broker = window.AuroraBroker;
  if (!Broker) return;

  // ---------------- events ----------------
  const listeners = Object.create(null);
  function on(evt, fn) { (listeners[evt] = listeners[evt] || []).push(fn); }
  function emit(evt, data) { (listeners[evt] || []).slice().forEach((fn) => { try { fn(data); } catch (e) { /* a broken listener must not break the system */ } }); }

  // ---------------- simulated world ----------------
  const T0 = Date.now();
  const MIN = 60e3, HOUR = 3600e3, DAY = 86400e3;
  function file(id, name, kind, space, size, ago, preview) { return { id, name, kind, space, size, modified: T0 - ago, preview }; }
  const files = Object.create(null);     // null prototype: ids such as "constructor" can never resolve
  [
    file("study-notes", "Study notes.md", "note", "study", 4210, 10 * MIN, "Chapter 4: memory management. Pages, frames and the translation table. Revise TLB shootdown before Friday."),
    file("reading-list", "Reading list.md", "note", "study", 1320, 3 * HOUR, "1. Operating Systems: Three Easy Pieces\n2. ARMv8-A architecture overview\n3. Notes on capability systems"),
    file("timetable", "Exam timetable.txt", "text", "study", 860, 2 * DAY, "Mon 09:00 Systems\nWed 14:00 Algorithms\nFri 09:00 Networks"),
    file("project-alpha", "Project Alpha brief.md", "note", "work", 9800, 26 * MIN, "Goal: ship the storage milestone. Open questions: recovery after an uncertain commit, review ownership."),
    file("budget-q4", "Budget Q4.csv", "table", "work", 15400, 5 * HOUR, "item,planned,actual\nhardware,1200,1180\nhosting,300,340"),
    file("meeting-1107", "Meeting 7 Nov.txt", "text", "work", 2300, 3 * DAY, "Attendees: 4. Decisions: keep QEMU-only until review completes."),
    file("kernel-sketch", "Kernel diagram.svg", "image", "projects", 48200, 1 * DAY, null),
    file("release-plan", "Release plan.md", "note", "projects", 6100, 6 * DAY, "Milestones A to E. Nothing ships without an independent review."),
    file("journal-nov", "Journal November.md", "note", "personal", 7300, 8 * HOUR, "A quiet week. Walked by the river on Sunday."),
    file("recipe-dal", "Dal recipe.txt", "text", "personal", 940, 12 * DAY, "Toor dal, turmeric, cumin, ghee, tomato. Pressure cook 3 whistles."),
  ].forEach((f) => { files[f.id] = f; });

  const world = {
    files,
    deleted: Object.create(null),   // id -> {file, at}: what Trail can restore (prototype memory only)
    timer: null,                    // {seconds, startedAt}
    lastSearch: null,
  };

  const dev = { stepDelayMs: 320, failNextType: null };
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  async function pace(type) {
    if (dev.stepDelayMs > 0) await sleep(dev.stepDelayMs);
    if (dev.failNextType === type) { dev.failNextType = null; throw new Error("simulated-failure"); }
  }
  const shell = () => window.AuroraShell;
  const models = () => window.AuroraModels;

  // ---------------- adapters: each drives the visible prototype and returns a real undo ----------------
  const adapters = {
    "app.open": async ({ app }) => {
      await pace("app.open");
      const r = shell().openApp(app, { background: true });
      if (!r.ok) throw new Error(r.reason || "cannot-open");
      return { result: app, undo: r.created ? () => { models().pages.close(r.id); } : undefined };
    },
    "space.switch": async ({ space }) => {
      await pace("space.switch");
      const was = shell().state.activeSpace;
      shell().switchSpace(space, { quiet: true });
      return { result: space, undo: () => { shell().switchSpace(was, { quiet: true }); } };
    },
    "timer.start": async ({ seconds }) => {
      await pace("timer.start");
      const was = world.timer;
      world.timer = { seconds, startedAt: Date.now() };
      emit("timer", world.timer);
      return { result: seconds, undo: () => { world.timer = was; emit("timer", world.timer); } };
    },
    "search.query": async ({ query }) => {
      await pace("search.query");
      world.lastSearch = query;
      shell().openIndex(query);
      return { result: query };
    },
    "focus.set": async ({ enabled }) => {
      await pace("focus.set");
      const qs = models().qs; const was = qs.get("focus");
      qs.set("focus", enabled);
      return { result: enabled, undo: () => { qs.set("focus", was); } };
    },
    "file.move": async ({ id, destination }) => {
      await pace("file.move");
      const f = files[id]; if (!f) throw new Error("no-such-file");
      const was = f.space; f.space = destination; f.modified = Date.now();
      emit("files");
      return { result: destination, undo: () => { if (!files[id]) throw new Error("no-such-file"); files[id].space = was; emit("files"); } };
    },
    "file.delete": async ({ id }) => {
      await pace("file.delete");
      const f = files[id]; if (!f) throw new Error("no-such-file");
      delete files[id]; world.deleted[id] = { file: f, at: Date.now() };
      emit("files");
      return { result: f.name, undo: () => { if (!world.deleted[id]) throw new Error("not-in-trail"); files[id] = world.deleted[id].file; delete world.deleted[id]; emit("files"); } };
    },
    // share.send has NO adapter on purpose: nothing in this prototype can leave the page. It previews as "unsupported".
  };

  // ---------------- permissions ----------------
  /** What Passport shows. `risk` is the highest risk class the permission gates. */
  const PERMISSIONS = [
    { id: "apps.open", label: "Open apps", risk: "S0", actions: ["app.open"], explain: "Bring an app to the front. Nothing is changed or shared." },
    { id: "spaces.switch", label: "Switch Space", risk: "S0", actions: ["space.switch"], explain: "Change which Space is active. Your content stays where it is." },
    { id: "timer.control", label: "Start timers", risk: "S0", actions: ["timer.start"], explain: "Start a countdown. It can be cancelled at any time." },
    { id: "index.search", label: "Search with Index", risk: "S0", actions: ["search.query"], explain: "Look things up on this device. The query is not sent anywhere." },
    { id: "settings.write", label: "Change settings", risk: "S1", actions: ["focus.set"], explain: "Change reversible settings such as Focus mode. Every change can be undone from its receipt." },
    { id: "files.edit", label: "Move and edit files", risk: "S2", actions: ["file.move"], explain: "Move files between Spaces. You review every change before it happens." },
    { id: "files.delete", label: "Delete files", risk: "S3", actions: ["file.delete"], explain: "Send files to Trail. Always needs a hold-to-confirm from you; it cannot be pre-approved." },
    { id: "external.send", label: "Send outside this device", risk: "S3", actions: ["share.send"], explain: "Share something with another person or service. Not available in this prototype, and never pre-approvable." },
  ];
  const DEFAULTS = { "apps.open": "allow", "spaces.switch": "allow", "timer.control": "allow", "index.search": "allow", "settings.write": "ask", "files.edit": "ask", "files.delete": "ask", "external.send": "deny" };
  const store = Broker.createPermissions(DEFAULTS);
  const permLog = [];
  const permDef = (id) => PERMISSIONS.find((p) => p.id === id) || null;

  const permissions = {
    catalog: () => PERMISSIONS.map((p) => Object.assign({}, p, { actions: p.actions.slice(), decision: store.get(p.id), supported: p.actions.some((a) => !!adapters[a]), canAllow: p.risk !== "S3" })),
    get: (id) => store.get(id),
    /** POLICY: only allow | ask | deny; unknown permissions are refused; S3 permissions can never be "allow". */
    set(id, decision) {
      const def = permDef(id);
      if (!def) return { ok: false, reason: "unknown-permission" };
      if (!["allow", "ask", "deny"].includes(decision)) return { ok: false, reason: "invalid-decision" };
      if (decision === "allow" && def.risk === "S3") return { ok: false, reason: "always-ask" };
      const from = store.get(id);
      if (from === decision) return { ok: true, unchanged: true };
      if (!store.set(id, decision)) return { ok: false, reason: "refused" };
      const entry = { id: "pl" + (permLog.length + 1), at: Date.now(), permission: id, label: def.label, from, to: decision };
      permLog.push(entry);
      emit("permission", entry);
      return { ok: true, entry };
    },
    resetAll() { PERMISSIONS.forEach((p) => permissions.set(p.id, DEFAULTS[p.id])); return { ok: true }; },
    defaults: () => Object.assign({}, DEFAULTS),
    log: () => permLog.map((e) => Object.assign({}, e)),
  };

  // ---------------- the one broker ----------------
  const broker = Broker.createBroker({ permissions: store, adapters });
  let trustedTaken = false;

  // ---------------- descriptions (plain text only; callers must use textContent) ----------------
  const SPACE_NAME = { study: "Study", work: "Work", projects: "Projects", personal: "Personal" };
  const cap = (s) => s.charAt(0).toUpperCase() + s.slice(1);
  function duration(seconds) {
    if (seconds % 3600 === 0) return seconds / 3600 + " hour" + (seconds === 3600 ? "" : "s");
    if (seconds % 60 === 0) return seconds / 60 + " minute" + (seconds === 60 ? "" : "s");
    return seconds + " second" + (seconds === 1 ? "" : "s");
  }
  function fileName(id) { return files[id] ? files[id].name : world.deleted[id] ? world.deleted[id].file.name : id; }
  /** {title, detail, icon, kind} for one action. Used by the review surface, receipts and Trail. */
  function describe(action) {
    const a = (action && action.arguments) || {};
    switch (action && action.type) {
      case "app.open": return { kind: "App", icon: a.app, title: "Open " + cap(String(a.app)), detail: "Brings the app to the front." };
      case "space.switch": return { kind: "Space", icon: "spaces", title: "Switch to " + (SPACE_NAME[a.space] || a.space) + " Space", detail: "Pinned tools and context follow the Space." };
      case "timer.start": return { kind: "Timer", icon: "timer", title: "Start a timer for " + duration(a.seconds), detail: "Runs in Clock. Can be cancelled." };
      case "search.query": return { kind: "Search", icon: "search", title: "Search for “" + a.query + "”", detail: "Looks on this device only." };
      case "focus.set": return { kind: "Setting", icon: "focus", title: "Turn Focus mode " + (a.enabled ? "on" : "off"), detail: "A reversible setting." };
      case "file.move": return { kind: "File", icon: "move", title: "Move “" + fileName(a.id) + "” to " + (SPACE_NAME[a.destination] || a.destination), detail: "The file changes Space. Can be undone." };
      case "file.delete": return { kind: "File", icon: "trash", title: "Delete “" + fileName(a.id) + "”", detail: "Moves it to Trail, where it can be restored during this session." };
      case "share.send": return { kind: "Share", icon: "share", title: "Send “" + fileName(a.id) + "” to " + a.to, detail: "Leaves this device and cannot be undone." };
      default: return { kind: "Action", icon: "info", title: "Unknown action", detail: "" };
    }
  }
  const REASONS = {
    "no-adapter": "Not available in this prototype.",
    "simulated-failure": "Simulated failure (developer option).",
    "no-such-file": "That file no longer exists.",
    "not-in-trail": "That item is no longer in Trail.",
    "low-memory": "Not enough simulated memory. Save paused pages first.",
    "approval-required": "This needs your review first.",
    "approval-expired": "The approval expired. Review it again.",
    "preview-expired": "The preview expired. Review it again.",
    "permission-changed": "A permission changed after you reviewed this. Review it again.",
    "permission-denied": "Blocked by a Passport permission.",
    "busy": "Another action is still running.",
    "already-undone": "This was already undone.",
    "not-undoable": "This action cannot be undone.",
    "undo-failed": "Undo did not work. Nothing else was changed.",
    "undo-failed-before": "Undo already failed for this action.",
  };
  /** Human sentence for a broker error/reason code. Never shows raw internals to the person. */
  function explain(code) {
    if (typeof code !== "string" || !code) return "Something went wrong.";
    if (code.startsWith("permission-denied:")) { const d = permDef(code.slice(18)); return "Blocked by Passport: “" + (d ? d.label : code.slice(18)) + "” is set to Deny."; }
    if (code.startsWith("permission-changed:")) { const d = permDef(code.slice(19)); return "“" + (d ? d.label : code.slice(19)) + "” now needs your approval. Review it again."; }
    return Object.prototype.hasOwnProperty.call(REASONS, code) ? REASONS[code] : "Could not complete (" + code.slice(0, 40) + ").";
  }

  // ---------------- receipts joined with their actions ----------------
  const receiptActions = new Map();   // receiptId -> {action, source, requestText}
  function noteRun(proposal, result, meta) {
    (result.steps || []).forEach((s) => { if (s.receiptId) receiptActions.set(s.receiptId, { action: proposal.steps[s.index], source: (meta && meta.source) || "system", requestText: (meta && meta.requestText) || null, proposalId: proposal.id }); });
    emit("receipts");
  }
  /** Every broker receipt, newest first, with its action, a description and the current undo state. */
  function receipts() {
    return broker.receipts().map((r) => {
      const m = receiptActions.get(r.id) || {};
      return Object.assign({}, r, { action: m.action || null, source: m.source || "system", requestText: m.requestText || null, describe: m.action ? describe(m.action) : describe(null), errorText: r.error ? explain(r.error) : null });
    }).reverse();
  }
  async function undo(receiptId) { const r = await broker.undo(receiptId); emit("receipts"); return r.ok ? r : Object.assign(r, { text: explain(r.reason) }); }
  async function undoProposal(id) { const r = await broker.undoProposal(id); emit("receipts"); return r; }

  let reviewer = null;
  window.AuroraSystem = {
    on, emit, dev, world, describe, explain, duration,
    SPACE_NAME, APPS: Broker.APPS.slice(), SPACES: Broker.SPACES.slice(), ACTIONS: Broker.ACTIONS,
    parseCommand: Broker.parseCommand,
    permissions,
    /** Untrusted-safe broker surface: propose / preview / getProposal only. */
    proposer: broker.proposer,
    files: {
      list: () => Object.keys(files).map((k) => Object.assign({}, files[k])),
      get: (id) => (typeof id === "string" && files[id] ? Object.assign({}, files[id]) : null),
      deleted: () => Object.keys(world.deleted).map((k) => Object.assign({ deletedAt: world.deleted[k].at }, world.deleted[k].file)),
    },
    receipts, undo, undoProposal,
    /** Ask for an action or plan. Resolves {ok, cancelled?, reason?, state?, steps?, proposalId?}. Never throws. */
    request(payload, meta) { return reviewer ? reviewer(payload, meta || {}) : Promise.resolve({ ok: false, reason: "no-review-surface" }); },
    /** review.js only: the trusted facet plus the privileged run functions, handed out exactly once. */
    takeTrusted(reviewFn) {
      if (trustedTaken) throw new Error("trusted-surface-already-taken");
      trustedTaken = true; reviewer = reviewFn;
      return Object.freeze({ ui: broker.takeTrustedUI(), execute: broker.execute, cancel: broker.cancel, noteRun });
    },
  };
})();
