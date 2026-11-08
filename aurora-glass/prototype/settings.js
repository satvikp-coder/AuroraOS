/* aurora-glass/prototype/settings.js — one settings registry for the Aurora Glass prototype (DOM-free).
 *
 * SIMULATION ONLY. Values live in memory (nothing is persisted, nothing touches a real device). Each entry says
 * honestly whether changing it has a visible effect in this prototype ("live") or is merely remembered
 * ("stored"); locked entries are shown but cannot be changed (e.g. S2/S3 approval is not optional).
 * The simplified and the advanced view are filters over the SAME registry, and search covers every entry.
 * Loadable in the browser (window.AuroraSettings) and in Node for unit tests.
 */
(function (root, factory) {
  if (typeof module === "object" && module.exports) module.exports = factory();
  else root.AuroraSettings = factory();
})(typeof self !== "undefined" ? self : this, function () {
  "use strict";

  const has = (o, k) => Object.prototype.hasOwnProperty.call(o, k);

  const SECTIONS = [
    { id: "appearance", label: "Appearance" },
    { id: "accessibility", label: "Accessibility" },
    { id: "notifications", label: "Notifications" },
    { id: "privacy", label: "Privacy" },
    { id: "battery", label: "Battery" },
    { id: "storage", label: "Storage" },
    { id: "aura", label: "Aura" },
    { id: "developer", label: "Developer options" },
  ];

  // type: toggle | choice | range | info.  effect: live (visible in this prototype) | stored (remembered only) | locked.
  const DEFS = [
    { id: "appearance.theme", section: "appearance", label: "Theme", type: "choice", options: ["light", "dark"], def: "light", effect: "live", keywords: ["dark mode", "light mode", "colour", "color"] },
    { id: "appearance.glass", section: "appearance", label: "Glass style", type: "choice", options: ["soft", "clear"], def: "soft", effect: "live", keywords: ["blur", "translucent", "opaque", "transparency", "tier"] },
    { id: "accessibility.textSize", section: "accessibility", label: "Text size", type: "choice", options: ["100", "125", "150", "200"], def: "100", effect: "live", keywords: ["font", "large text", "zoom", "bigger"] },
    { id: "accessibility.highContrast", section: "accessibility", label: "High contrast", type: "toggle", def: false, effect: "live", keywords: ["contrast", "readability", "vision"] },
    { id: "accessibility.reduceMotion", section: "accessibility", label: "Reduce motion", type: "toggle", def: false, effect: "live", keywords: ["animation", "transitions", "vestibular"] },
    { id: "notifications.focus", section: "notifications", label: "Focus mode", type: "toggle", def: false, effect: "live", keywords: ["do not disturb", "dnd", "quiet", "silence"] },
    { id: "notifications.glassCorner", section: "notifications", label: "Show the Glass Corner handle", type: "toggle", def: true, effect: "live", keywords: ["notification inbox", "corner", "handle"] },
    { id: "notifications.lockPreviews", section: "notifications", label: "Show previews on the lock screen", type: "toggle", def: false, effect: "stored", keywords: ["lock screen", "privacy", "content"] },
    { id: "privacy.indicators", section: "privacy", label: "Show privacy indicators in Quick Center", type: "toggle", def: true, effect: "live", keywords: ["microphone", "camera", "location", "sensor"] },
    { id: "privacy.auraHistory", section: "privacy", label: "Keep Aura request history on this device", type: "toggle", def: false, effect: "stored", advanced: true, keywords: ["history", "log", "assistant"] },
    { id: "battery.saver", section: "battery", label: "Battery saver", type: "toggle", def: false, effect: "live", keywords: ["power", "low power", "clear mode", "save battery"] },
    { id: "storage.usage", section: "storage", label: "Storage used", type: "info", def: "12.4 GB of 64 GB (simulated sample data)", effect: "locked", keywords: ["space", "disk", "capacity", "files"] },
    { id: "storage.trailDays", section: "storage", label: "Keep Trail history for (days)", type: "range", min: 7, max: 90, def: 30, effect: "stored", advanced: true, keywords: ["history", "recovery", "undo", "retention"] },
    { id: "aura.mode", section: "aura", label: "Aura mode", type: "choice", options: ["off", "ask", "quick"], def: "ask", effect: "live", keywords: ["assistant", "ai", "quick commands"] },
    { id: "aura.approval", section: "aura", label: "Approval for file changes and deletions", type: "info", def: "Always required (cannot be turned off)", effect: "locked", keywords: ["s2", "s3", "confirm", "safety", "permissions", "passport"] },
    { id: "developer.showAdvanced", section: "developer", label: "Show advanced settings", type: "toggle", def: false, effect: "live", keywords: ["expert", "more options", "developer"] },
  ];

  function createSettings() {
    const values = Object.create(null);
    const byId = Object.create(null);
    DEFS.forEach((d) => { byId[d.id] = d; values[d.id] = d.def; });
    const listeners = [];

    function def(id) { return typeof id === "string" && has(byId, id) ? byId[id] : null; }
    function valid(d, v) {
      if (d.type === "toggle") return typeof v === "boolean";
      if (d.type === "choice") return typeof v === "string" && d.options.includes(v);
      if (d.type === "range") return Number.isInteger(v) && v >= d.min && v <= d.max;
      return false;
    }
    function view(d) {
      return { id: d.id, section: d.section, label: d.label, type: d.type, value: values[d.id], options: d.options ? d.options.slice() : null,
        min: d.min === undefined ? null : d.min, max: d.max === undefined ? null : d.max, effect: d.effect, advanced: !!d.advanced, keywords: d.keywords.slice() };
    }
    const api = {
      sections: SECTIONS.map((s) => Object.assign({}, s)),
      onChange(fn) { listeners.push(fn); },
      get(id) { return def(id) ? values[id] : undefined; },
      entry(id) { const d = def(id); return d ? view(d) : null; },
      /** Entries of one section; advanced ones only when includeAdvanced. */
      list(sectionId, includeAdvanced) {
        return DEFS.filter((d) => d.section === sectionId && (includeAdvanced || !d.advanced)).map(view);
      },
      all() { return DEFS.map(view); },
      set(id, value) {
        const d = def(id);
        if (!d) return { ok: false, reason: "unknown-setting" };
        if (d.type === "info" || d.effect === "locked") return { ok: false, reason: "read-only" };
        if (!valid(d, value)) return { ok: false, reason: "invalid-value" };
        const old = values[id];
        values[id] = value;
        if (old !== value) listeners.slice().forEach((fn) => fn(id, value, old));
        return { ok: true, value };
      },
      reset() {
        DEFS.forEach((d) => { if (d.type !== "info" && values[d.id] !== d.def) api.set(d.id, d.def); });
        return { ok: true };
      },
      /** Search EVERY entry (advanced ones too). Ranked: label prefix, label word, label substring, section, keyword. */
      search(query) {
        const q = typeof query === "string" ? query.trim().toLowerCase() : "";
        if (!q) return [];
        const out = [];
        DEFS.forEach((d) => {
          const label = d.label.toLowerCase(), section = SECTIONS.find((s) => s.id === d.section).label.toLowerCase();
          let rank = 0;
          if (label.startsWith(q)) rank = 5;
          else if (label.split(/[^a-z0-9]+/).some((w) => w.startsWith(q))) rank = 4;
          else if (label.includes(q)) rank = 3;
          else if (section.includes(q)) rank = 2;
          else if (d.keywords.some((k) => k.includes(q))) rank = 1;
          if (rank) out.push({ rank, entry: view(d) });
        });
        return out.sort((a, b) => b.rank - a.rank || a.entry.label.localeCompare(b.entry.label)).map((r) => r.entry);
      },
      /** JSON-safe snapshot of every non-info value (what a "settings export" would contain). */
      snapshot() { const o = {}; DEFS.forEach((d) => { if (d.type !== "info") o[d.id] = values[d.id]; }); return o; },
    };
    return api;
  }

  return { createSettings, SECTIONS, DEFS };
});
