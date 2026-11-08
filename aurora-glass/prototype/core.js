/* aurora-glass/prototype/core.js — pure, DOM-free mock models for the Aurora Glass prototype.
 *
 * SIMULATION ONLY. These models describe how the AuroraOS UI is meant to behave.
 * They do not manage kernel processes, memory, radios or any real device setting.
 * Loadable in the browser (window.AuroraCore) and in Node (require) for unit tests.
 */
(function (root, factory) {
  if (typeof module === "object" && module.exports) module.exports = factory();
  else root.AuroraCore = factory();
})(typeof self !== "undefined" ? self : this, function () {
  "use strict";

  // ------------------------------------------------------------------
  // Pages: simulated multitasking model
  // ------------------------------------------------------------------
  const PAGE_STATES = ["running", "paused", "saved"];

  function createPages(options) {
    const opts = options || {};
    const budgetMB = opts.budgetMB || 2048; // simulated RAM budget, not a hardware claim
    const warnRatio = opts.warnRatio || 0.85;
    let pages = (opts.pages || []).map((p) => ({ ...p }));
    const listeners = [];

    function usedMB() {
      // Saved (frozen-to-storage) pages hold no RAM in this model.
      return pages.reduce((n, p) => n + (p.state === "saved" ? 0 : p.memMB), 0);
    }
    function emit() { listeners.forEach((fn) => fn()); }
    function find(id) { return pages.find((p) => p.id === id); }
    function fail(reason) { return { ok: false, reason }; }

    const api = {
      budgetMB,
      onChange(fn) { listeners.push(fn); },
      list() { return pages.map((p) => ({ ...p })); },
      get(id) { const p = find(id); return p ? { ...p } : null; },
      usedMB,
      usage() {
        const used = usedMB();
        return { usedMB: used, budgetMB, ratio: used / budgetMB, low: used / budgetMB >= warnRatio };
      },
      pause(id) {
        const p = find(id);
        if (!p) return fail("not-found");
        if (p.state !== "running") return fail("not-running");
        p.state = "paused"; emit();
        return { ok: true };
      },
      save(id) {
        const p = find(id);
        if (!p) return fail("not-found");
        if (p.state === "saved") return fail("already-saved");
        p.state = "saved"; emit();
        return { ok: true };
      },
      resume(id) {
        const p = find(id);
        if (!p) return fail("not-found");
        if (p.state === "running") return fail("already-running");
        const extra = p.state === "saved" ? p.memMB : 0;
        if (usedMB() + extra > budgetMB) return fail("low-memory");
        p.state = "running"; emit();
        return { ok: true };
      },
      close(id) {
        const i = pages.findIndex((p) => p.id === id);
        if (i < 0) return fail("not-found");
        const [removed] = pages.splice(i, 1);
        emit();
        return { ok: true, removed: { ...removed } };
      },
      /** Save every paused page to free simulated RAM. Returns freed MB. */
      freeMemory() {
        let freed = 0;
        pages.forEach((p) => { if (p.state === "paused") { p.state = "saved"; freed += p.memMB; } });
        if (freed) emit();
        return { ok: true, freedMB: freed };
      },
      /** Open (or surface) an app. Re-uses an existing page for the same app. */
      open(appId, name, space, memMB) {
        const existing = pages.find((p) => p.appId === appId);
        if (existing) {
          if (existing.state === "running") return { ok: true, id: existing.id };
          const r = api.resume(existing.id);
          return r.ok ? { ok: true, id: existing.id } : r;
        }
        if (usedMB() + memMB > budgetMB) return fail("low-memory");
        const id = "page-" + appId;
        pages.push({ id, appId, name, space, state: "running", memMB });
        emit();
        return { ok: true, id };
      },
    };
    return api;
  }

  // ------------------------------------------------------------------
  // Quick settings: validated key/value model
  // ------------------------------------------------------------------
  const QUICK_SCHEMA = {
    brightness: { type: "range", min: 0, max: 100, def: 100 },
    volume: { type: "range", min: 0, max: 100, def: 40 },
    wifi: { type: "bool", def: true },
    bluetooth: { type: "bool", def: false },
    batterySaver: { type: "bool", def: false },
    focus: { type: "bool", def: false },
    rotationLock: { type: "bool", def: true },
    appearance: { type: "enum", values: ["light", "dark"], def: "light" },
    auraMode: { type: "enum", values: ["off", "ask", "quick"], def: "ask" },
  };

  function createQuickSettings(initial) {
    const values = {};
    Object.keys(QUICK_SCHEMA).forEach((k) => { values[k] = QUICK_SCHEMA[k].def; });
    const listeners = [];
    const api = {
      schema: QUICK_SCHEMA,
      onChange(fn) { listeners.push(fn); },
      get(key) { return Object.prototype.hasOwnProperty.call(values, key) ? values[key] : undefined; },
      all() { return { ...values }; },
      /** Validate and set. Returns {ok, value} or {ok:false, reason}. */
      set(key, value) {
        const spec = Object.prototype.hasOwnProperty.call(QUICK_SCHEMA, key) ? QUICK_SCHEMA[key] : null;
        if (!spec) return { ok: false, reason: "unknown-setting" };
        let v = value;
        if (spec.type === "bool") {
          if (typeof v !== "boolean") return { ok: false, reason: "invalid-value" };
        } else if (spec.type === "range") {
          if (typeof v !== "number" || !Number.isFinite(v)) return { ok: false, reason: "invalid-value" };
          v = Math.min(spec.max, Math.max(spec.min, Math.round(v)));
        } else if (spec.type === "enum") {
          if (!spec.values.includes(v)) return { ok: false, reason: "invalid-value" };
        }
        const old = values[key];
        values[key] = v;
        if (old !== v) listeners.forEach((fn) => fn(key, v, old));
        return { ok: true, value: v };
      },
      toggle(key) {
        if (!Object.prototype.hasOwnProperty.call(QUICK_SCHEMA, key) || QUICK_SCHEMA[key].type !== "bool") return { ok: false, reason: "not-a-toggle" };
        return api.set(key, !values[key]);
      },
      /** Simulated privacy indicators, derived from which fake sessions are active. */
      privacy(sessions) {
        const s = sessions || {};
        return {
          microphone: !!s.microphone,
          camera: !!s.camera,
          location: !!s.location,
        };
      },
    };
    if (initial) Object.keys(initial).forEach((k) => api.set(k, initial[k]));
    return api;
  }

  return { PAGE_STATES, createPages, createQuickSettings, QUICK_SCHEMA };
});
