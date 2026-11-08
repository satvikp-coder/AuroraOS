/* aurora-glass/prototype/broker.js — Aura Action Broker contract + Quick Commands parser.
 *
 * HOST-TESTABLE SIMULATION. This defines the contract between "something that proposes an
 * action" (a deterministic parser today, possibly a local model later) and the OS-side
 * authority that decides whether the action may run. The proposer never grants itself
 * permission: validation, permission checks, risk class and confirmation are all enforced
 * here. It uses only explicitly registered mock adapters. No eval, no shell, no file access.
 */
(function (root, factory) {
  if (typeof module === "object" && module.exports) module.exports = factory();
  else root.AuroraBroker = factory();
})(typeof self !== "undefined" ? self : this, function () {
  "use strict";

  const has = (o, k) => Object.prototype.hasOwnProperty.call(o, k);
  const APPS = ["notes", "files", "calculator", "clock", "settings", "calendar"];
  const SPACES = ["study", "work", "projects", "personal"];
  const MAX_PLAN_STEPS = 12;

  // Risk classes: S0 safe, S1 reversible config, S2 edit/move user data, S3 delete/send/install.
  const ACTIONS = {
    "app.open":     { risk: "S0", permission: "apps.open",       reversible: true,  args: { app: { enum: APPS } } },
    "space.switch": { risk: "S0", permission: "spaces.switch",   reversible: true,  args: { space: { enum: SPACES } } },
    "timer.start":  { risk: "S0", permission: "timer.control",   reversible: true,  args: { seconds: { int: [1, 86400] } } },
    "search.query": { risk: "S0", permission: "index.search",    reversible: true,  args: { query: { str: [1, 100] } } },
    "focus.set":    { risk: "S1", permission: "settings.write",  reversible: true,  args: { enabled: { bool: true } } },
    "file.move":    { risk: "S2", permission: "files.edit",      reversible: true,  args: { id: { id: true }, destination: { enum: SPACES } } },
    "file.delete":  { risk: "S3", permission: "files.delete",    reversible: true,  args: { id: { id: true } } },        // moves to Trail
    "share.send":   { risk: "S3", permission: "external.send",   reversible: false, args: { id: { id: true }, to: { str: [1, 80] } } },
  };

  function validateArg(spec, v) {
    if (spec.enum) return typeof v === "string" && spec.enum.includes(v);
    if (spec.int) return Number.isInteger(v) && v >= spec.int[0] && v <= spec.int[1];
    if (spec.str) return typeof v === "string" && v.trim().length >= spec.str[0] && v.length <= spec.str[1] && !/[\u0000-\u001f]/.test(v);
    if (spec.bool) return typeof v === "boolean";
    if (spec.id) return typeof v === "string" && /^[a-z0-9][a-z0-9_-]{0,63}$/i.test(v);
    return false;
  }

  /** Strict schema validation. Returns {ok:true} or {ok:false, error}. */
  function validateAction(action) {
    if (action === null || typeof action !== "object" || Array.isArray(action)) return { ok: false, error: "malformed" };
    const keys = Object.keys(action);
    if (keys.some((k) => k !== "type" && k !== "arguments")) return { ok: false, error: "unexpected-field" };
    if (typeof action.type !== "string" || !has(ACTIONS, action.type)) return { ok: false, error: "unknown-action" };
    const def = ACTIONS[action.type];
    const a = action.arguments;
    if (a === null || typeof a !== "object" || Array.isArray(a)) return { ok: false, error: "missing-arguments" };
    const given = Object.keys(a);
    if (given.some((k) => !has(def.args, k))) return { ok: false, error: "unexpected-argument" };
    for (const k of Object.keys(def.args)) {
      if (!has(a, k)) return { ok: false, error: "missing-argument:" + k };
      if (!validateArg(def.args[k], a[k])) return { ok: false, error: "invalid-argument:" + k };
    }
    return { ok: true };
  }

  function canonical(action) {
    const def = ACTIONS[action.type];
    return action.type + "|" + Object.keys(def.args).sort().map((k) => k + "=" + JSON.stringify(action.arguments[k])).join("&");
  }

  /** Permission store: decisions are 'allow' | 'deny' | 'ask'. Unknown permissions default to 'deny'. */
  function createPermissions(initial) {
    const d = {};
    Object.keys(initial || {}).forEach((k) => { d[k] = initial[k]; });
    return {
      get(p) { return has(d, p) ? d[p] : "deny"; },
      set(p, v) {
        if (!["allow", "deny", "ask"].includes(v)) return false;
        d[p] = v; return true;
      },
    };
  }

  function createBroker(opts) {
    const permissions = opts.permissions;
    const adapters = opts.adapters || {};
    const nextNonce = opts.nonce || (() => Math.random().toString(36).slice(2) + Date.now().toString(36));
    const clock = opts.now || (() => Date.now());
    const receipts = [];
    const undoFns = new Map();
    const tokens = new Map(); // token -> canonical action (single use)
    let counter = 0;
    let chain = Promise.resolve();

    function receipt(action, status, extra) {
      const def = has(ACTIONS, action && action.type) ? ACTIONS[action.type] : null;
      const r = Object.assign({
        id: "r" + (++counter), at: clock(), type: action && typeof action.type === "string" ? action.type : "unknown",
        risk: def ? def.risk : null, status, undoable: false, undone: false,
      }, extra || {});
      receipts.push(r);
      return r;
    }

    /** What WOULD happen — no side effects, no execution. */
    function preview(action) {
      const v = validateAction(action);
      if (!v.ok) return { ok: false, error: v.error };
      const def = ACTIONS[action.type];
      const decision = permissions.get(def.permission);
      return {
        ok: true, type: action.type, risk: def.risk, permission: def.permission, decision,
        needsConfirmation: def.risk === "S3" || decision === "ask",
        reversible: def.reversible && !!adapters[action.type],
        supported: !!adapters[action.type],
      };
    }

    /** Issued only by trusted UI code after the user has seen the preview. Bound to this exact action. */
    function requestConfirmation(action) {
      if (!validateAction(action).ok) return null;
      const t = "c-" + nextNonce();
      tokens.set(t, canonical(action));
      return t;
    }

    async function runOne(action, confirmation) {
      const v = validateAction(action);
      if (!v.ok) return receipt(action, "invalid", { error: v.error });
      const def = ACTIONS[action.type];
      const decision = permissions.get(def.permission);
      if (decision === "deny") return receipt(action, "denied", { error: "permission-denied:" + def.permission });
      const adapter = adapters[action.type];
      if (!adapter) return receipt(action, "unsupported", { error: "no-adapter" });
      if (def.risk === "S3" || decision === "ask") {
        // A token must be one this broker issued for this exact action; it is consumed on first use.
        const ok = typeof confirmation === "string" && tokens.get(confirmation) === canonical(action);
        if (!ok) return receipt(action, "needs-confirmation", { preview: preview(action) });
        tokens.delete(confirmation);
      }
      try {
        const out = (await adapter(action.arguments)) || {};
        const r = receipt(action, "done", { result: out.result === undefined ? null : out.result });
        if (typeof out.undo === "function") { r.undoable = true; undoFns.set(r.id, out.undo); }
        return r;
      } catch (e) {
        return receipt(action, "failed", { error: String(e && e.message || e) });
      }
    }

    /** Executions are serialized so concurrent callers cannot interleave adapter effects. */
    function execute(action, confirmation) {
      const p = chain.then(() => runOne(action, confirmation));
      chain = p.catch(() => {});
      return p;
    }

    async function undo(receiptId) {
      const r = receipts.find((x) => x.id === receiptId);
      if (!r) return { ok: false, reason: "not-found" };
      if (!r.undoable || r.undone) return { ok: false, reason: r.undone ? "already-undone" : "not-undoable" };
      const fn = undoFns.get(receiptId);
      try { await fn(); } catch (e) { return { ok: false, reason: "undo-failed" }; }
      r.undone = true; r.undoable = false; undoFns.delete(receiptId);
      return { ok: true };
    }

    // ---------------- Plans ----------------
    function validatePlan(plan) {
      if (plan === null || typeof plan !== "object" || Array.isArray(plan)) return { ok: false, error: "malformed" };
      if (Object.keys(plan).some((k) => k !== "steps")) return { ok: false, error: "unexpected-field" };
      if (!Array.isArray(plan.steps) || plan.steps.length === 0) return { ok: false, error: "no-steps" };
      if (plan.steps.length > MAX_PLAN_STEPS) return { ok: false, error: "too-many-steps" };
      for (let i = 0; i < plan.steps.length; i++) {
        const v = validateAction(plan.steps[i]);
        if (!v.ok) return { ok: false, error: v.error, step: i };
      }
      // Conflicting operations: the same file may not be acted on by more than one step.
      const seen = new Map();
      for (let i = 0; i < plan.steps.length; i++) {
        const s = plan.steps[i];
        if (s.type === "file.move" || s.type === "file.delete" || s.type === "share.send") {
          const id = s.arguments.id;
          if (seen.has(id)) return { ok: false, error: "conflicting-steps", step: i };
          seen.set(id, i);
        }
      }
      return { ok: true };
    }

    function previewPlan(plan) {
      const v = validatePlan(plan);
      if (!v.ok) return v;
      const steps = plan.steps.map((s) => preview(s));
      const rank = { S0: 0, S1: 1, S2: 2, S3: 3 };
      const risk = steps.reduce((m, s) => (rank[s.risk] > rank[m] ? s.risk : m), "S0");
      const allReversible = steps.every((s) => s.reversible);
      const noneReversible = steps.every((s) => !s.reversible);
      return { ok: true, steps, risk, rollback: allReversible ? "full" : noneReversible ? "none" : "limited" };
    }

    /**
     * Run a plan in order. options: { skip:Set<number>, confirmations:{[index]:token}, isCancelled:()=>bool }.
     * Stops at the first step that does not complete. Returns step receipts and an honest rollback level.
     */
    async function runPlan(plan, options) {
      const o = options || {};
      const v = validatePlan(plan);
      if (!v.ok) return { ok: false, error: v.error, step: v.step, receipts: [] };
      const out = [];
      let stoppedAt = null;
      for (let i = 0; i < plan.steps.length; i++) {
        if (o.isCancelled && o.isCancelled()) { stoppedAt = i; out.push({ step: i, status: "cancelled" }); break; }
        if (o.skip && o.skip.has(i)) { out.push({ step: i, status: "skipped" }); continue; }
        const r = await execute(plan.steps[i], o.confirmations && o.confirmations[i]);
        out.push({ step: i, status: r.status, receiptId: r.id, error: r.error });
        if (r.status !== "done") { stoppedAt = i; break; }
      }
      const done = out.filter((s) => s.status === "done").map((s) => receipts.find((r) => r.id === s.receiptId));
      const undoableCount = done.filter((r) => r.undoable).length;
      const rollback = done.length === 0 ? "none" : undoableCount === done.length ? "full" : undoableCount === 0 ? "none" : "limited";
      return { ok: stoppedAt === null, stoppedAt, steps: out, rollback };
    }

    /** Undo every undoable step of a plan result, newest first. */
    async function undoPlan(result) {
      const ids = result.steps.filter((s) => s.status === "done").map((s) => s.receiptId).reverse();
      const outcomes = [];
      for (const id of ids) outcomes.push(Object.assign({ receiptId: id }, await undo(id)));
      return outcomes;
    }

    /** Return a NEW plan with step i replaced; the result is re-validated. */
    function editStep(plan, i, action) {
      if (!plan || !Array.isArray(plan.steps) || !Number.isInteger(i) || i < 0 || i >= plan.steps.length) return { ok: false, error: "bad-index" };
      const next = { steps: plan.steps.slice() };
      next.steps[i] = action;
      const v = validatePlan(next);
      return v.ok ? { ok: true, plan: next } : v;
    }

    return {
      preview, previewPlan, validatePlan, requestConfirmation, execute, undo, runPlan, undoPlan, editStep,
      receipts: () => receipts.map((r) => Object.assign({}, r)),
    };
  }

  // ------------------------------------------------------------------
  // Quick Commands: deterministic text -> typed proposal. No model involved.
  // The result is only a PROPOSAL; the broker still decides everything.
  // ------------------------------------------------------------------
  function parseCommand(text) {
    if (typeof text !== "string") return { ok: false, error: "not-text" };
    const t = text.trim().replace(/\s+/g, " ").replace(/[.!?]+$/, "");
    if (!t || t.length > 200) return { ok: false, error: "empty-or-too-long" };
    const lc = t.toLowerCase();
    let m;
    if (/^prepare my (phone|device) for stud(y|ying)$/.test(lc)) {
      return { ok: true, kind: "plan", plan: { steps: [
        { type: "space.switch", arguments: { space: "study" } },
        { type: "focus.set", arguments: { enabled: true } },
        { type: "app.open", arguments: { app: "notes" } },
        { type: "timer.start", arguments: { seconds: 1500 } },
      ] } };
    }
    if ((m = lc.match(/^open (?:the )?(\w+)(?: app)?$/)) && APPS.includes(m[1])) return one("app.open", { app: m[1] });
    if ((m = lc.match(/^start (?:a |an )?(\d{1,4}) ?(?:-)?(minute|min|second|sec|hour)s? timer$/))) {
      const n = Number(m[1]); const mult = m[2].startsWith("h") ? 3600 : m[2].startsWith("m") ? 60 : 1;
      return one("timer.start", { seconds: n * mult });
    }
    if ((m = lc.match(/^(?:go to|switch to) (?:the )?(\w+)(?: space)?$/)) && SPACES.includes(m[1])) return one("space.switch", { space: m[1] });
    if ((m = lc.match(/^turn (on|off) focus(?: mode)?$/))) return one("focus.set", { enabled: m[1] === "on" });
    if (/^show (?:me )?my files$/.test(lc)) return one("app.open", { app: "files" });
    if ((m = t.match(/^search (?:for )?(.{1,100})$/i))) return one("search.query", { query: m[1].trim() });
    return { ok: false, error: "unrecognized" };
  }
  function one(type, args) {
    const action = { type, arguments: args };
    // The parser only ever returns schema-valid proposals (e.g. rejects "0 minute" or absurd durations).
    return validateAction(action).ok ? { ok: true, kind: "action", action } : { ok: false, error: "invalid-arguments" };
  }

  /** Mock adapters over a plain state object (used by tests and the prototype). Never touches a real OS. */
  function createMockAdapters(s) {
    return {
      "app.open": async ({ app }) => { const was = s.openApp; s.openApp = app; return { result: app, undo: () => { s.openApp = was; } }; },
      "space.switch": async ({ space }) => { const was = s.space; s.space = space; return { result: space, undo: () => { s.space = was; } }; },
      "timer.start": async ({ seconds }) => { const was = s.timer; s.timer = seconds; return { result: seconds, undo: () => { s.timer = was; } }; },
      "search.query": async ({ query }) => { s.lastSearch = query; return { result: query }; },
      "focus.set": async ({ enabled }) => { const was = s.focus; s.focus = enabled; return { undo: () => { s.focus = was; } }; },
      "file.move": async ({ id, destination }) => {
        const f = s.files[id]; if (!f) throw new Error("no-such-file");
        const was = f.space; f.space = destination; return { undo: () => { f.space = was; } };
      },
      "file.delete": async ({ id }) => {
        const f = s.files[id]; if (!f) throw new Error("no-such-file");
        s.trail.push(id); delete s.files[id];
        return { undo: () => { s.files[id] = f; s.trail = s.trail.filter((x) => x !== id); } };
      },
      // share.send intentionally has no mock adapter in the default set: external sending is unsupported here.
    };
  }

  return { ACTIONS, APPS, SPACES, MAX_PLAN_STEPS, validateAction, createPermissions, createBroker, parseCommand, createMockAdapters };
});
