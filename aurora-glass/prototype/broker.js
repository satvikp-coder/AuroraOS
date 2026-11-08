/* aurora-glass/prototype/broker.js — Aura Action Broker contract + Quick Commands parser.
 *
 * HOST-TESTABLE SIMULATION. This defines how a *proposal* (from a deterministic parser today,
 * possibly a local model later) becomes an *authorized action*. Only explicitly registered mock
 * adapters run. No eval, no shell, no filesystem, no network.
 *
 * TRUST MODEL (read this):
 *  - Untrusted proposers (parser, future model) receive ONLY `broker.proposer`: propose / preview /
 *    getProposal. They hold no authority to approve, edit, execute or undo.
 *  - Approval is granted only through the trusted-UI facet, obtainable exactly once via
 *    `broker.takeTrustedUI()` by the shell code that renders the review screen.
 *  - SIMULATION BOUNDARY: this is plain JavaScript in one browser context. Any script that runs
 *    before the shell takes the trusted facet, or that can reach the full `broker` object, could
 *    misuse it. This does NOT create an OS trust boundary. The future native Action Broker MUST
 *    enforce approvals through a privileged OS service and a secure input path, never through
 *    strings or objects supplied by a model or an app.
 *
 * PERMISSION SEMANTICS: decisions are exactly allow | ask | deny. Anything else (missing, malformed, a throwing
 *  or corrupted store) is deny. Permissions are re-read before approval and before EVERY step. Approval records
 *  the decision each step had; later changes: -> deny always stops; allow -> ask voids the approval (no step runs
 *  if detected before start; the changed step and later ones do not run if detected mid-plan); ask -> allow is
 *  accepted. Steps that already completed are never reverted automatically; their undo stays the user's choice.
 *  A step already running in an adapter cannot be recalled, so a change applies from the next step.
 * RISK POLICY: S0 single step may run directly; S1/S2/S3 and every plan need approval (DESIGN.md says S1 is a
 *  "plan preview"; the blueprint says "preview if in a plan" - ambiguous for a standalone S1, so the
 *  conservative reading applies).
 *
 * LIFECYCLE: DRAFT -> VALIDATED -> PREVIEWED -> APPROVED -> RUNNING -> COMPLETED | FAILED | CANCELLED
 * (also EXPIRED / REJECTED / SUPERSEDED). Proposals are immutable; editing or skipping a step
 * creates a successor proposal with a new digest and invalidates the old one and its approval.
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
  const MAX_PAYLOAD_CHARS = 4096;
  const MAX_LIVE_PROPOSALS = 100;
  const RISK_RANK = { S0: 0, S1: 1, S2: 2, S3: 3 };
  const TERMINAL = ["COMPLETED", "FAILED", "CANCELLED", "EXPIRED", "REJECTED", "SUPERSEDED"];

  // Risk classes: S0 safe, S1 reversible config, S2 edit/move user data, S3 delete/send/install.
  // `reversible` is CONTRACT METADATA ("undo is expected"); actual undo is known only from receipts.
  const ACTIONS = {
    "app.open":     { risk: "S0", permission: "apps.open",      reversible: true,  args: { app: { enum: APPS } } },
    "space.switch": { risk: "S0", permission: "spaces.switch",  reversible: true,  args: { space: { enum: SPACES } } },
    "timer.start":  { risk: "S0", permission: "timer.control",  reversible: true,  args: { seconds: { int: [1, 86400] } } },
    "search.query": { risk: "S0", permission: "index.search",   reversible: false, args: { query: { str: [1, 100] } } },
    "focus.set":    { risk: "S1", permission: "settings.write", reversible: true,  args: { enabled: { bool: true } } },
    "file.move":    { risk: "S2", permission: "files.edit",     reversible: true,  args: { id: { id: true }, destination: { enum: SPACES } } },
    "file.delete":  { risk: "S3", permission: "files.delete",   reversible: true,  args: { id: { id: true } } },      // moves to Trail
    "share.send":   { risk: "S3", permission: "external.send",  reversible: false, args: { id: { id: true }, to: { str: [1, 80] } } },
  };

  const BAD_TEXT = /[\u0000-\u001f\u007f-\u009f‪-‮⁦-⁩​-‏﻿]/;
  function validateArg(spec, v) {
    if (spec.enum) return typeof v === "string" && spec.enum.includes(v);
    if (spec.int) return Number.isInteger(v) && v >= spec.int[0] && v <= spec.int[1];
    if (spec.str) return typeof v === "string" && v.trim().length >= spec.str[0] && v.length <= spec.str[1] && !BAD_TEXT.test(v);
    if (spec.bool) return typeof v === "boolean";
    if (spec.id) return typeof v === "string" && /^[a-z0-9][a-z0-9_-]{0,63}$/i.test(v);
    return false;
  }

  /** Strict schema validation of an already-plain action. Returns {ok:true} or {ok:false, error}. */
  function validateAction(action) {
    if (action === null || typeof action !== "object" || Array.isArray(action)) return { ok: false, error: "malformed" };
    const keys = Object.keys(action);
    if (keys.some((k) => k !== "type" && k !== "arguments")) return { ok: false, error: "unexpected-field" };
    if (typeof action.type !== "string" || !has(ACTIONS, action.type)) return { ok: false, error: "unknown-action" };
    const def = ACTIONS[action.type];
    const a = action.arguments;
    if (a === null || typeof a !== "object" || Array.isArray(a)) return { ok: false, error: "missing-arguments" };
    if (Object.keys(a).some((k) => !has(def.args, k))) return { ok: false, error: "unexpected-argument" };
    for (const k of Object.keys(def.args)) {
      if (!has(a, k)) return { ok: false, error: "missing-argument:" + k };
      if (!validateArg(def.args[k], a[k])) return { ok: false, error: "invalid-argument:" + k };
    }
    return { ok: true };
  }

  /** Copy caller data once into a plain object (no getters/proxies/prototype tricks). null if unsafe. */
  function snapshotAction(action) {
    try {
      if (action === null || typeof action !== "object" || Array.isArray(action)) return null;
      const out = {};
      for (const k of Object.keys(action)) {
        const d = Object.getOwnPropertyDescriptor(action, k);
        if (!d || !has(d, "value")) return null; // accessor
        let val = d.value;
        if (k === "arguments") {
          if (val === null || typeof val !== "object" || Array.isArray(val)) return null;
          const inner = {};
          for (const ak of Object.keys(val)) {
            const ad = Object.getOwnPropertyDescriptor(val, ak);
            if (!ad || !has(ad, "value")) return null;
            if (ad.value !== null && typeof ad.value === "object") return null; // arguments are primitives only
            Object.defineProperty(inner, ak, { value: ad.value, enumerable: true, writable: true, configurable: true });
          }
          val = inner;
        }
        Object.defineProperty(out, k, { value: val, enumerable: true, writable: true, configurable: true });
      }
      return out;
    } catch (e) { return null; }
  }

  function canonicalAction(action) {
    const def = ACTIONS[action.type];
    return action.type + "|" + Object.keys(def.args).sort().map((k) => k + "=" + JSON.stringify(action.arguments[k])).join("&");
  }
  function deepFreezeAction(a) { Object.freeze(a.arguments); return Object.freeze(a); }

  const DECISIONS = ["allow", "deny", "ask"];
  /** Fail closed: anything that is not exactly 'allow' or 'ask' is 'deny'. */
  function normalizeDecision(v) { return v === "allow" || v === "ask" ? v : "deny"; }

  /** Permission store: 'allow' | 'deny' | 'ask'. Missing, unknown or corrupted entries are 'deny'. */
  function createPermissions(initial) {
    const d = Object.create(null);
    if (initial !== null && typeof initial === "object") {
      Object.keys(initial).forEach((k) => {
        const desc = Object.getOwnPropertyDescriptor(initial, k);
        const v = desc && has(desc, "value") ? desc.value : undefined; // never run getters from caller data
        if (k.length > 0) d[k] = DECISIONS.includes(v) ? v : "deny";    // invalid initial value => deny, not 'allow'
      });
    }
    return {
      get(p) { return typeof p === "string" && p in d ? normalizeDecision(d[p]) : "deny"; },
      set(p, v) {
        if (typeof p !== "string" || p.length === 0 || !DECISIONS.includes(v)) return false;
        d[p] = v; return true;
      },
    };
  }

  function createBroker(opts) {
    const permissions = opts.permissions;
    const adapters = opts.adapters || {};
    const nextNonce = opts.nonce || (() => Math.random().toString(36).slice(2) + Date.now().toString(36));
    const clock = opts.now || (() => Date.now());
    const previewTtlMs = opts.previewTtlMs || 5 * 60 * 1000;
    const approvalTtlMs = opts.approvalTtlMs || 60 * 1000;

    const receipts = [];
    const undoFns = new Map();
    const proposals = new Map();
    let counter = 0;
    let rcounter = 0;
    let busyWith = null; // one active run/undo at a time; always cleared in finally
    let trustedTaken = false;

    // Every permission read goes through here: a throwing or malformed store means 'deny'.
    function decide(permission) {
      try { return normalizeDecision(permissions.get(permission)); } catch (e) { return "deny"; }
    }

    // ---------------- helpers ----------------
    function setState(p, state, reason) {
      p.state = state; p.history.push(state);
      if (reason) p.reason = reason;
    }
    function refreshExpiry(p) {
      const t = clock();
      if (p.state === "PREVIEWED" && t > p.previewExpiresAt) setState(p, "EXPIRED", "preview-expired");
      else if (p.state === "APPROVED" && t > p.approvalExpiresAt) setState(p, "EXPIRED", "approval-expired");
    }
    function stepList(p) { return p.steps.map((s, i) => ({ s, i })).filter((x) => !p.skips.includes(x.i)); }

    function previewStep(action) {
      const def = ACTIONS[action.type];
      const decision = decide(def.permission);
      const supported = !!adapters[action.type];
      // DESIGN.md: "S1 reversible settings: plan preview"; blueprint: "Preview if in a plan". The wording is
      // ambiguous for a standalone S1 change, so the conservative reading applies: S1 also needs review.
      return {
        type: action.type, risk: def.risk, permission: def.permission, decision, supported,
        needsConfirmation: def.risk === "S3", needsReview: def.risk !== "S0" || decision === "ask",
        undoExpected: def.reversible && supported, // contract expectation only; not a guarantee
      };
    }
    function buildPreview(p) {
      const steps = p.steps.map((s, i) => Object.assign({ index: i, skipped: p.skips.includes(i) }, previewStep(s)));
      const live = steps.filter((s) => !s.skipped);
      const risk = live.reduce((m, s) => (RISK_RANK[s.risk] > RISK_RANK[m] ? s.risk : m), "S0");
      const exp = live.length === 0 ? "none" : live.every((s) => s.undoExpected) ? "full" : live.every((s) => !s.undoExpected) ? "none" : "limited";
      const needsReview = !p.single || live.some((s) => s.needsReview);
      return { steps, risk, needsReview, expectedRollback: exp, rollbackNote: "Expected from contract; actual undo is known only after execution.", s3Indices: live.filter((s) => s.needsConfirmation).map((s) => s.index) };
    }

    function digestOf(p) {
      return (p.single ? "action" : "plan") + "\n" + p.steps.map((s, i) => (p.skips.includes(i) ? "~" : "") + canonicalAction(s)).join("\n");
    }

    function newProposal(steps, single, skips) {
      const live = [...proposals.values()].filter((p) => !TERMINAL.includes(p.state)).length;
      if (live >= MAX_LIVE_PROPOSALS) return { ok: false, error: "too-many-proposals" };
      const p = {
        id: "p" + (++counter) + "-" + nextNonce(), single, steps: steps.map(deepFreezeAction), skips: (skips || []).slice().sort(),
        state: "DRAFT", history: ["DRAFT"], createdAt: clock(), previewExpiresAt: clock() + previewTtlMs,
        approvalExpiresAt: null, reason: null, confirmedS3: null, cancelRequested: false, result: null,
        previewDecisions: null, approvedDecisions: null,
      };
      p.previewDecisions = steps.map((s) => decide(ACTIONS[s.type].permission));
      Object.freeze(p.steps);
      p.digest = digestOf(p);
      setState(p, "VALIDATED"); setState(p, "PREVIEWED");
      proposals.set(p.id, p);
      return { ok: true, p };
    }
    function publicView(p) {
      refreshExpiry(p);
      return {
        id: p.id, kind: p.single ? "action" : "plan", state: p.state, history: p.history.slice(), reason: p.reason, digest: p.digest,
        steps: p.steps.map((s) => ({ type: s.type, arguments: Object.assign({}, s.arguments) })), skips: p.skips.slice(),
        preview: buildPreview(p), result: p.result ? JSON.parse(JSON.stringify(p.result)) : null,
      };
    }

    /** Validates and snapshots a payload. Returns {ok, steps, single} or {ok:false, error}. */
    function intake(payload) {
      let raw;
      try { raw = JSON.stringify(payload); } catch (e) { return { ok: false, error: "malformed" }; }
      if (typeof raw !== "string" || raw.length > MAX_PAYLOAD_CHARS) return { ok: false, error: "too-large" };
      if (payload === null || typeof payload !== "object" || Array.isArray(payload)) return { ok: false, error: "malformed" };
      if (has(payload, "steps")) {
        if (Object.keys(payload).some((k) => k !== "steps")) return { ok: false, error: "unexpected-field" };
        const list = payload.steps;
        if (!Array.isArray(list) || list.length === 0) return { ok: false, error: "no-steps" };
        if (list.length > MAX_PLAN_STEPS) return { ok: false, error: "too-many-steps" };
        const steps = [];
        for (let i = 0; i < list.length; i++) {
          const snap = snapshotAction(list[i]);
          const v = snap ? validateAction(snap) : { ok: false, error: "malformed" };
          if (!v.ok) return { ok: false, error: v.error, step: i };
          steps.push(snap);
        }
        const seen = new Set();
        for (let i = 0; i < steps.length; i++) {
          if (["file.move", "file.delete", "share.send"].includes(steps[i].type)) {
            const id = steps[i].arguments.id;
            if (seen.has(id)) return { ok: false, error: "conflicting-steps", step: i };
            seen.add(id);
          }
        }
        return { ok: true, steps, single: false };
      }
      const snap = snapshotAction(payload);
      const v = snap ? validateAction(snap) : { ok: false, error: "malformed" };
      return v.ok ? { ok: true, steps: [snap], single: true } : { ok: false, error: v.error };
    }

    // ---------------- untrusted-safe surface ----------------
    function propose(payload) {
      const i = intake(payload);
      if (!i.ok) return i;
      const r = newProposal(i.steps, i.single, []);
      return r.ok ? { ok: true, proposal: publicView(r.p) } : r;
    }
    function preview(payload) {
      const i = intake(payload);
      if (!i.ok) return i;
      const tmp = { single: i.single, steps: i.steps, skips: [] };
      return Object.assign({ ok: true }, buildPreview(tmp));
    }
    function getProposal(id) { const p = proposals.get(id); return p ? publicView(p) : null; }

    // ---------------- receipts ----------------
    function addReceipt(action, status, extra) {
      const def = action && has(ACTIONS, action.type) ? ACTIONS[action.type] : null;
      const r = Object.assign({ id: "r" + (++rcounter), at: clock(), type: def ? action.type : "unknown", risk: def ? def.risk : null,
        status, undoState: "unavailable" }, extra || {});
      receipts.push(r);
      return r;
    }
    const copyReceipt = (r) => Object.assign({ undoable: r.undoState === "available" }, JSON.parse(JSON.stringify(r)));

    // ---------------- execution ----------------
    async function runStep(p, index, step) {
      const def = ACTIONS[step.type];
      const meta = { proposalId: p.id, stepIndex: index };
      // Permissions are re-checked before EVERY step. A change that only reduces friction (ask -> allow) is
      // fine; deny always stops; allow -> ask means the user's approval no longer covers this step.
      const now = decide(def.permission);
      if (now === "deny") return addReceipt(step, "denied", Object.assign({ error: "permission-denied:" + def.permission }, meta));
      if (now === "ask" && p.approvedDecisions && p.approvedDecisions[index] !== "ask")
        return addReceipt(step, "needs-reapproval", Object.assign({ error: "permission-changed:" + def.permission }, meta));
      const adapter = adapters[step.type];
      if (!adapter) return addReceipt(step, "unsupported", Object.assign({ error: "no-adapter" }, meta));
      try {
        const out = (await adapter(step.arguments)) || {};
        const r = addReceipt(step, "done", Object.assign({ result: out.result === undefined ? null : out.result }, meta));
        if (typeof out.undo === "function") { r.undoState = "available"; undoFns.set(r.id, out.undo); }
        return r;
      } catch (e) {
        return addReceipt(step, "failed", Object.assign({ error: String((e && e.message) || e) }, meta));
      }
    }

    function actualRollback(stepResults) {
      const done = stepResults.filter((s) => s.status === "done").map((s) => receipts.find((r) => r.id === s.receiptId));
      if (done.length === 0) return "none";
      const avail = done.filter((r) => r.undoState === "available").length;
      return avail === done.length ? "full" : avail === 0 ? "none" : "limited";
    }

    async function execute(id, options) {
      const o = options || {};
      const p = proposals.get(id);
      if (!p) return { ok: false, reason: "not-found" };
      refreshExpiry(p);
      if (busyWith) return { ok: false, reason: "busy" };
      const view = buildPreview(p);
      if (view.needsReview) {
        if (p.state !== "APPROVED") return { ok: false, reason: p.state === "EXPIRED" ? p.reason : "approval-required", state: p.state };
      } else if (p.state !== "PREVIEWED" && p.state !== "APPROVED") {
        return { ok: false, reason: p.state === "EXPIRED" ? p.reason : "wrong-state", state: p.state };
      }
      // Preflight: if a permission now needs MORE interaction than the user approved, the approval is void.
      if (p.approvedDecisions) {
        for (const { s, i } of stepList(p)) {
          if (decide(ACTIONS[s.type].permission) === "ask" && p.approvedDecisions[i] !== "ask") {
            setState(p, "EXPIRED", "permission-changed");
            return { ok: false, reason: "permission-changed", state: p.state };
          }
        }
      }
      // Claim synchronously (before any await): the approval is single-use and only one run may be active.
      busyWith = p.id; setState(p, "RUNNING");
      const stepResults = [];
      const progress = (e) => { try { if (o.onProgress) o.onProgress(e); } catch (x) { /* observer errors must not break the run */ } };
      try {
        let stopped = null;
        for (const { s, i } of stepList(p)) {
          if (p.cancelRequested || (o.isCancelled && o.isCancelled())) { stopped = "cancelled"; stepResults.push({ index: i, status: "cancelled" }); break; }
          progress({ index: i, status: "running", total: p.steps.length });
          const r = await runStep(p, i, s);
          stepResults.push({ index: i, status: r.status, receiptId: r.id, error: r.error });
          progress({ index: i, status: r.status, total: p.steps.length });
          if (r.status !== "done") { stopped = "failed"; break; }
        }
        if (stopped === null && p.cancelRequested) stopped = "cancelled";
        for (const { i } of p.steps.map((s, i) => ({ i })).filter((x) => p.skips.includes(x.i))) stepResults.push({ index: i, status: "skipped" });
        stepResults.sort((a, b) => a.index - b.index);
        setState(p, stopped === "cancelled" ? "CANCELLED" : stopped === "failed" ? "FAILED" : "COMPLETED");
        p.result = { steps: stepResults, actualRollback: actualRollback(stepResults), expectedRollback: view.expectedRollback };
        return Object.assign({ ok: stopped === null, state: p.state }, JSON.parse(JSON.stringify(p.result)));
      } catch (e) {
        setState(p, "FAILED", "internal-error");
        return { ok: false, reason: "internal-error", state: p.state };
      } finally {
        busyWith = null;
      }
    }

    function cancel(id) {
      const p = proposals.get(id);
      if (!p) return { ok: false, reason: "not-found" };
      refreshExpiry(p);
      if (p.state === "RUNNING") { p.cancelRequested = true; return { ok: true, pending: true }; } // current step finishes, rest do not run
      if (TERMINAL.includes(p.state)) return { ok: false, reason: "already-" + p.state.toLowerCase() };
      setState(p, "CANCELLED", "cancelled"); return { ok: true };
    }

    async function undoOne(receiptId) {
      const r = receipts.find((x) => x.id === receiptId);
      if (!r) return { ok: false, reason: "not-found" };
      if (r.undoState === "used") return { ok: false, reason: "already-undone" };
      if (r.undoState === "in-progress") return { ok: false, reason: "in-progress" };
      if (r.undoState === "failed") return { ok: false, reason: "undo-failed-before" };
      if (r.undoState !== "available") return { ok: false, reason: "not-undoable" };
      r.undoState = "in-progress"; // synchronous claim: concurrent callers cannot both run the same undo
      const fn = undoFns.get(receiptId);
      try { await fn(); } catch (e) { r.undoState = "failed"; undoFns.delete(receiptId); return { ok: false, reason: "undo-failed" }; }
      r.undoState = "used"; undoFns.delete(receiptId);
      return { ok: true };
    }
    async function undo(receiptId) {
      if (busyWith) return { ok: false, reason: "busy" };
      busyWith = "undo:" + receiptId;
      try { return await undoOne(receiptId); } finally { busyWith = null; }
    }
    /** Undo every currently-available step of a finished proposal, newest first. */
    async function undoProposal(id) {
      const p = proposals.get(id);
      if (!p || !p.result) return { ok: false, reason: "nothing-to-undo" };
      if (busyWith) return { ok: false, reason: "busy" };
      busyWith = "undo:" + id;
      try {
        const out = [];
        for (const s of p.result.steps.filter((x) => x.status === "done").reverse()) out.push(Object.assign({ receiptId: s.receiptId }, await undoOne(s.receiptId)));
        return { ok: out.every((x) => x.ok), steps: out };
      } finally { busyWith = null; }
    }

    // ---------------- trusted UI facet (one holder only) ----------------
    function takeTrustedUI() {
      if (trustedTaken) throw new Error("trusted-ui-already-taken");
      trustedTaken = true;
      return Object.freeze({
        /** Approve the exact content the user was shown. S3 steps must be confirmed explicitly by index. */
        approve(id, a) {
          const p = proposals.get(id);
          if (!p) return { ok: false, reason: "not-found" };
          refreshExpiry(p);
          if (p.state !== "PREVIEWED") return { ok: false, reason: p.state === "EXPIRED" ? p.reason : "wrong-state", state: p.state };
          if (!a || typeof a.digest !== "string" || a.digest !== p.digest) return { ok: false, reason: "digest-mismatch" };
          const live = stepList(p);
          if (live.length === 0) return { ok: false, reason: "nothing-to-run" };
          for (const { s, i } of live) {
            const d = decide(ACTIONS[s.type].permission);
            if (d === "deny") return { ok: false, reason: "permission-denied" };
            if (d !== p.previewDecisions[i]) return { ok: false, reason: "permission-changed" }; // user reviewed a stale picture
            if (!adapters[s.type]) return { ok: false, reason: "unsupported-step" };
          }
          const need = buildPreview(p).s3Indices.slice().sort((x, y) => x - y);
          const given = Array.isArray(a.confirmS3) ? a.confirmS3.slice().sort((x, y) => x - y) : [];
          if (JSON.stringify(need) !== JSON.stringify(given)) return { ok: false, reason: "s3-confirmation-required", required: need };
          p.confirmedS3 = need; p.approvalExpiresAt = clock() + approvalTtlMs;
          p.approvedDecisions = p.steps.map((s) => decide(ACTIONS[s.type].permission));
          setState(p, "APPROVED");
          return { ok: true, expiresAt: p.approvalExpiresAt };
        },
        reject(id) {
          const p = proposals.get(id);
          if (!p || TERMINAL.includes(p.state) || p.state === "RUNNING") return { ok: false, reason: "not-rejectable" };
          setState(p, "REJECTED", "rejected-by-user"); return { ok: true };
        },
        /** Editing creates a NEW proposal; the old proposal and its approval are invalidated. */
        editStep(id, index, action) {
          const p = proposals.get(id);
          if (!p || p.single || !Number.isInteger(index) || index < 0 || index >= p.steps.length) return { ok: false, error: "bad-index" };
          if (p.state === "RUNNING" || TERMINAL.includes(p.state)) return { ok: false, error: "not-editable" };
          const steps = p.steps.map((s) => ({ type: s.type, arguments: Object.assign({}, s.arguments) }));
          steps[index] = action;
          const i = intake({ steps });
          if (!i.ok) return i;
          const r = newProposal(i.steps, false, p.skips);
          if (!r.ok) return r;
          setState(p, "SUPERSEDED", "edited");
          return { ok: true, proposal: publicView(r.p) };
        },
        /** Re-preview with current permissions (needed after a permission change); old proposal is superseded. */
        refreshPreview(id) {
          const p = proposals.get(id);
          if (!p || p.state === "RUNNING" || TERMINAL.includes(p.state)) return { ok: false, error: "not-refreshable" };
          const r = newProposal(p.steps.map((s) => ({ type: s.type, arguments: Object.assign({}, s.arguments) })), p.single, p.skips);
          if (!r.ok) return r;
          setState(p, "SUPERSEDED", "refreshed");
          return { ok: true, proposal: publicView(r.p) };
        },
        skipStep(id, index, skipped) {
          const p = proposals.get(id);
          if (!p || p.single || !Number.isInteger(index) || index < 0 || index >= p.steps.length) return { ok: false, error: "bad-index" };
          if (p.state === "RUNNING" || TERMINAL.includes(p.state)) return { ok: false, error: "not-editable" };
          const set = new Set(p.skips); if (skipped === false) set.delete(index); else set.add(index);
          const r = newProposal(p.steps.map((s) => ({ type: s.type, arguments: Object.assign({}, s.arguments) })), false, [...set]);
          if (!r.ok) return r;
          setState(p, "SUPERSEDED", "skips-changed");
          return { ok: true, proposal: publicView(r.p) };
        },
      });
    }

    return {
      /** Hand ONLY this to untrusted proposers (parser / model). */
      proposer: Object.freeze({ propose, preview, getProposal }),
      propose, preview, getProposal, execute, cancel, undo, undoProposal, takeTrustedUI,
      receipts: () => receipts.map(copyReceipt),
    };
  }

  // ------------------------------------------------------------------
  // Quick Commands: deterministic text -> typed proposal. No model, no authority.
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
    return validateAction(action).ok ? { ok: true, kind: "action", action } : { ok: false, error: "invalid-arguments" };
  }

  // Caller-supplied ids index a dictionary: only OWN properties count. Plain objects inherit names such as
  // constructor/toString/valueOf, which match the id syntax and must never resolve to a file.
  const ownGet = (d, k) => (Object.prototype.hasOwnProperty.call(d, k) ? d[k] : undefined);
  const ownSet = (d, k, v) => { Object.defineProperty(d, k, { value: v, enumerable: true, writable: true, configurable: true }); };
  const ownDel = (d, k) => { if (Object.prototype.hasOwnProperty.call(d, k)) delete d[k]; };

  /** Mock adapters over a plain state object. Never touches a real OS. */
  function createMockAdapters(s) {
    return {
      "app.open": async ({ app }) => { const was = s.openApp; s.openApp = app; return { result: app, undo: () => { s.openApp = was; } }; },
      "space.switch": async ({ space }) => { const was = s.space; s.space = space; return { result: space, undo: () => { s.space = was; } }; },
      "timer.start": async ({ seconds }) => { const was = s.timer; s.timer = seconds; return { result: seconds, undo: () => { s.timer = was; } }; },
      "search.query": async ({ query }) => { s.lastSearch = query; return { result: query }; },
      "focus.set": async ({ enabled }) => { const was = s.focus; s.focus = enabled; return { undo: () => { s.focus = was; } }; },
      "file.move": async ({ id, destination }) => {
        const f = ownGet(s.files, id); if (!f) throw new Error("no-such-file");
        const was = f.space; f.space = destination; return { undo: () => { f.space = was; } };
      },
      "file.delete": async ({ id }) => {
        const f = ownGet(s.files, id); if (!f) throw new Error("no-such-file");
        s.trail.push(id); ownDel(s.files, id);
        return { undo: () => { ownSet(s.files, id, f); s.trail = s.trail.filter((x) => x !== id); } };
      },
      // share.send intentionally has no mock adapter: external sending is unsupported in this simulation.
    };
  }

  return { ACTIONS, APPS, SPACES, MAX_PLAN_STEPS, TERMINAL, validateAction, createPermissions, createBroker, parseCommand, createMockAdapters };
});
