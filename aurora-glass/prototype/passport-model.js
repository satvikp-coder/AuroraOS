/* aurora-glass/prototype/passport-model.js — DOM-free model for the Passport screen.
 *
 * SIMULATION ONLY. The Action Broker decides permissions per CAPABILITY (apps.open, files.edit, ...), not per app.
 * This model only describes which capabilities each prototype app can ask for, so Passport can show them per app.
 * It never changes a permission: Passport calls AuroraSystem.permissions.set / resetAll and nothing else.
 * Loadable in the browser (window.AuroraPassportModel) and in Node for unit tests.
 */
(function (root, factory) {
  if (typeof module === "object" && module.exports) module.exports = factory();
  else root.AuroraPassportModel = factory();
})(typeof self !== "undefined" ? self : this, function () {
  "use strict";

  const freeze = (list) => Object.freeze(list.map((x) => Object.freeze(Object.assign({}, x, x.permissions ? { permissions: Object.freeze(x.permissions.slice()) } : {}))));
  const has = (o, k) => Object.prototype.hasOwnProperty.call(o, k);
  const ORDER = { deny: 0, ask: 1, allow: 2 };           // how much may happen without asking you
  const DECISIONS = ["allow", "ask", "deny"];

  const ALL = Object.freeze(["apps.open", "spaces.switch", "timer.control", "index.search", "settings.write", "files.edit", "files.delete", "external.send"]);
  /** Which capabilities each app can request. Aura can request everything, always through the broker. */
  const APPS_SRC = [
    { id: "aura", name: "Aura", icon: "aura", hue: 190, permissions: ALL },
    { id: "notes", name: "Notes", icon: "notes", hue: 42, permissions: ["files.edit"] },
    { id: "files", name: "Files", icon: "files", hue: 212, permissions: ["files.edit", "files.delete", "external.send"] },
    { id: "vault", name: "Vault", icon: "vault", hue: 188, permissions: ["files.edit", "files.delete", "external.send"] },
    { id: "clock", name: "Clock", icon: "clock", hue: 262, permissions: ["timer.control"] },
    { id: "settings", name: "Settings", icon: "settings", hue: 220, permissions: ["settings.write"] },
    { id: "calculator", name: "Calculator", icon: "calculator", hue: 228, permissions: [] },
    { id: "calendar", name: "Calendar", icon: "calendar", hue: 6, permissions: [] },
  ];
  /** Sensors the prototype cannot offer: shown so the list is honest about what exists on real hardware. */
  const HARDWARE_SRC = [
    { id: "camera", label: "Camera", icon: "camera" },
    { id: "microphone", label: "Microphone", icon: "mic" },
    { id: "location", label: "Location", icon: "location" },
  ];
  const APPS = freeze(APPS_SRC), HARDWARE = freeze(HARDWARE_SRC);
  /** What changes when a capability is set to Allow (shown in the confirmation). */
  const CONSEQUENCE = {
    "apps.open": "open apps",
    "spaces.switch": "switch Spaces",
    "timer.control": "start timers",
    "index.search": "run searches",
    "settings.write": "change reversible settings such as Focus mode",
    "files.edit": "move and edit your files",
    "files.delete": "delete your files",
    "external.send": "send things outside this device",
  };
  const DECISION_WORD = { allow: "Allow", ask: "Ask", deny: "Deny" };

  /** "tighten" (less may happen unasked), "loosen", or "same". Unknown decisions are treated as "same" (nothing to do). */
  function classify(from, to) {
    if (!has(ORDER, from) || !has(ORDER, to) || from === to) return "same";
    return ORDER[to] < ORDER[from] ? "tighten" : "loosen";
  }

  /** Only moving to Allow lets something run without asking you; that is the one change that needs a confirmation. */
  function needsConfirm(from, to) { return to === "allow" && classify(from, to) === "loosen"; }

  function summarize(catalog) {
    const n = { allow: 0, ask: 0, deny: 0 };
    (catalog || []).forEach((c) => { if (has(n, c.decision)) n[c.decision]++; });
    const parts = [n.allow + " allowed", n.ask + " ask each time", n.deny + " blocked"];
    return { allow: n.allow, ask: n.ask, deny: n.deny, sentence: parts.join(", ") };
  }

  function consequence(permissionId) { return has(CONSEQUENCE, permissionId) ? CONSEQUENCE[permissionId] : "do this"; }

  function appById(id) { return APPS.find((a) => a.id === id) || null; }
  /** Apps that can request this capability (a change to the capability affects all of them). */
  function appsUsing(permissionId) { return APPS.filter((a) => a.permissions.includes(permissionId)).map((a) => a.id); }

  /** The permissions "Revoke all" would set to Deny for an app: its non-S0 capabilities that are not already denied. */
  function revokeTargets(appId, catalog) {
    const a = appById(appId);
    if (!a) return [];
    return (catalog || []).filter((c) => a.permissions.includes(c.id) && c.risk !== "S0" && c.decision !== "deny").map((c) => c.id);
  }

  /** Every capability a revoke would also affect through OTHER apps (so the confirmation can say so). */
  function sharedWith(appId, permissionIds) {
    const out = {};
    (permissionIds || []).forEach((p) => { const others = appsUsing(p).filter((x) => x !== appId); if (others.length) out[p] = others; });
    return out;
  }

  // Everything exported is deeply frozen: no caller can change what another caller sees.
  return { DECISIONS: Object.freeze(DECISIONS), DECISION_WORD: Object.freeze(DECISION_WORD), APPS, HARDWARE, ALL, classify, needsConfirm, summarize, consequence, appById, appsUsing, revokeTargets, sharedWith };
});
