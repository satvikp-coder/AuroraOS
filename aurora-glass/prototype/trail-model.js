/* aurora-glass/prototype/trail-model.js — DOM-free model for the Trail screen (window.AuroraTrailModel).
 *
 * SIMULATION ONLY. Trail shows what the prototype Action Broker really recorded. This model only GROUPS, FILTERS and
 * DESCRIBES receipts that AuroraSystem.receipts() returned. It never stores history of its own, never decides whether
 * something can be undone (the receipt's undoState does), and never changes anything.
 * Loadable in the browser and in Node for unit tests.
 */
(function (root, factory) {
  if (typeof module === "object" && module.exports) module.exports = factory();
  else root.AuroraTrailModel = factory();
})(typeof self !== "undefined" ? self : this, function () {
  "use strict";

  const has = (o, k) => Object.prototype.hasOwnProperty.call(o, k);
  const STATUS_FILTERS = Object.freeze([["all", "All"], ["done", "Done"], ["problem", "Failed or blocked"], ["undoable", "Undoable"], ["undone", "Undone"]]);
  const RISK_FILTERS = Object.freeze([["all", "Any risk"], ["S0", "S0 · Safe"], ["S1", "S1 · Setting"], ["S2", "S2 · Files"], ["S3", "S3 · Destructive"]]);

  /** [label, tone, icon] for the receipt's own outcome. Unknown statuses are shown as such, never as success. */
  const STATUS = Object.freeze({
    done: ["Done", "ok", "check"],
    failed: ["Failed", "error", "alert"],
    denied: ["Blocked", "error", "lock"],
    "needs-reapproval": ["Needs review again", "warn", "eye"],
    unsupported: ["Not available", "muted", "info"],
  });
  const UNKNOWN_STATUS = Object.freeze(["Unknown", "muted", "info"]);
  const statusInfo = (status) => (typeof status === "string" && has(STATUS, status) ? STATUS[status] : UNKNOWN_STATUS);

  /** Why a successful action has no Undo. Keyed by action type; the broker is the authority, this is only the words. */
  const NO_UNDO_WHY = Object.freeze({
    "search.query": "searching changes nothing",
    "share.send": "it left this device",
  });

  /** Filter category of one receipt. A receipt can be both "done" and "undoable". */
  function isProblem(r) { return r.status !== "done"; }
  function isUndone(r) { return r.undoState === "used"; }
  function isUndoable(r) { return r.status === "done" && r.undoState === "available"; }

  function undoInfo(r) {
    if (!r) return { state: "none", label: "", why: "" };
    if (r.status !== "done") return { state: "none", label: "Nothing to undo", why: "Nothing ran, so there is nothing to undo." };
    switch (r.undoState) {
      case "available": return { state: "available", label: "Can be undone", why: "Undo is available for this action." };
      case "in-progress": return { state: "running", label: "Undoing…", why: "Undo is running." };
      case "used": return { state: "used", label: "Undone", why: "Already undone." };
      case "failed": return { state: "failed", label: "Undo failed", why: "Undo failed: it did not work and nothing else was changed." };
      default: {
        const why = r.type && has(NO_UNDO_WHY, r.type) ? NO_UNDO_WHY[r.type] : "this action has no undo";
        return { state: "unavailable", label: "Can’t be undone", why: "Can’t be undone: " + why + "." };
      }
    }
  }

  /** The proposal-level outcome. `proposal` is AuroraSystem.proposer.getProposal(id) or null. */
  function groupOutcome(receipts, proposal, totalSteps) {
    const done = receipts.filter((r) => r.status === "done").length;
    const single = totalSteps <= 1;
    const state = proposal && proposal.state;
    if (state === "CANCELLED") return done > 0 ? ["Stopped", "warn", "pause"] : ["Stopped", "warn", "pause"];
    if (done === receipts.length && done >= totalSteps) return ["Complete", "ok", "check"];
    if (done === 0) {
      const only = receipts[0];
      if (single && only && only.status === "denied") return ["Blocked", "error", "lock"];
      if (single && only && only.status === "needs-reapproval") return ["Needs review again", "warn", "eye"];
      return ["Failed", "error", "alert"];
    }
    return ["Partial", "warn", "alert"];
  }

  /**
   * Group receipts (newest first, as AuroraSystem.receipts() returns them) by proposal.
   * getProposal(id) is optional and may return null; only broker facts are used. Groups come back newest first and the
   * receipts inside a group in step order.
   */
  function group(receipts, getProposal) {
    const byId = new Map(), order = [];
    (Array.isArray(receipts) ? receipts : []).forEach((r) => {
      if (!r || typeof r.id !== "string") return;
      const key = typeof r.proposalId === "string" && r.proposalId ? r.proposalId : "single:" + r.id;
      if (!byId.has(key)) { byId.set(key, []); order.push(key); }
      byId.get(key).push(r);
    });
    const groups = order.map((key) => {
      const rs = byId.get(key).slice().sort((a, b) => (a.stepIndex || 0) - (b.stepIndex || 0) || (a.at - b.at));
      const proposal = typeof getProposal === "function" && !key.startsWith("single:") ? safe(getProposal, key) : null;
      const totalSteps = proposal && Array.isArray(proposal.steps) ? Math.max(proposal.steps.length - ((proposal.skips && proposal.skips.length) || 0), rs.length) : rs.length;
      const first = rs[0] || {};
      const at = Math.max.apply(null, rs.map((r) => r.at || 0));
      return {
        id: key, proposalId: key.startsWith("single:") ? null : key, kind: totalSteps > 1 ? "plan" : "action",
        requestText: rs.map((r) => r.requestText).find((t) => typeof t === "string" && t) || null,
        source: first.source || "system", at, receipts: rs, totalSteps,
        notRun: Math.max(0, totalSteps - rs.length), outcome: groupOutcome(rs, proposal, totalSteps),
        undoable: rs.filter(isUndoable).length, proposalState: proposal ? proposal.state : null,
      };
    });
    groups.sort((a, b) => b.at - a.at);
    return groups;
  }
  function safe(fn, arg) { try { return fn(arg) || null; } catch (e) { return null; } }

  /**
   * Filter groups. opts = {query, status, risk}. Filtering is by receipt: a group stays if any receipt matches and then
   * lists only the matching ones (matched/total tell the UI to say so). Unknown filter values match nothing special:
   * they fall back to "all" so a bad value can never hide history.
   */
  function filter(groups, opts) {
    const o = opts || {};
    const status = STATUS_FILTERS.some((s) => s[0] === o.status) ? o.status : "all";
    const risk = RISK_FILTERS.some((s) => s[0] === o.risk) ? o.risk : "all";
    const q = typeof o.query === "string" ? o.query.trim().toLowerCase() : "";
    const statusOk = (r) => status === "all" || (status === "done" && r.status === "done") || (status === "problem" && isProblem(r)) || (status === "undoable" && isUndoable(r)) || (status === "undone" && isUndone(r));
    const riskOk = (r) => risk === "all" || r.risk === risk;
    const hay = (g, r) => [r.describe && r.describe.title, r.describe && r.describe.kind, r.source, g.requestText, r.errorText, statusInfo(r.status)[0], undoInfo(r).label, r.type].filter((x) => typeof x === "string").join("\n").toLowerCase();
    const out = [];
    groups.forEach((g) => {
      const kept = g.receipts.filter((r) => statusOk(r) && riskOk(r) && (!q || hay(g, r).includes(q)));
      if (kept.length) out.push(Object.assign({}, g, { receipts: kept, matched: kept.length, total: g.receipts.length }));
    });
    return out;
  }

  /** "Today", "Yesterday" or a short date. `now` is injectable for tests. */
  function dayLabel(ts, now) {
    const n = new Date(now === undefined ? Date.now() : now), d = new Date(ts);
    const start = (x) => new Date(x.getFullYear(), x.getMonth(), x.getDate()).getTime();
    const diff = Math.round((start(n) - start(d)) / 86400e3);
    if (diff <= 0) return "Today";
    if (diff === 1) return "Yesterday";
    return d.toLocaleDateString(undefined, { weekday: "long", day: "numeric", month: "short" });
  }
  /** Split newest-first groups into [{label, groups}] by day, keeping order. */
  function byDay(groups, now) {
    const days = [];
    groups.forEach((g) => {
      const label = dayLabel(g.at, now);
      const last = days[days.length - 1];
      if (last && last.label === label) last.groups.push(g); else days.push({ label, groups: [g] });
    });
    return days;
  }

  /**
   * Restore for a deleted file = undo of its delete receipt. Returns the newest receipt that deleted `fileId` and can
   * still be undone, or null (then the UI says restoring is unavailable and offers nothing).
   */
  function restoreReceipt(fileId, receipts) {
    const list = (Array.isArray(receipts) ? receipts : []).filter((r) => r && r.type === "file.delete" && r.status === "done" && r.undoState === "available" && r.action && r.action.arguments && r.action.arguments.id === fileId);
    return list.length ? list.reduce((a, b) => ((b.at || 0) >= (a.at || 0) ? b : a)) : null;
  }
  function restoreWhy(fileId, receipts) {
    const all = (Array.isArray(receipts) ? receipts : []).filter((r) => r && r.type === "file.delete" && r.action && r.action.arguments && r.action.arguments.id === fileId);
    if (!all.length) return "No delete receipt was found for this file, so it can’t be restored from here.";
    if (all.some((r) => r.undoState === "in-progress")) return "Restoring…";
    if (all.some((r) => r.undoState === "failed")) return "Restoring failed earlier and can’t be retried.";
    if (all.some((r) => r.undoState === "used")) return "The delete was already undone.";
    return "The delete has no available undo.";
  }

  return { STATUS_FILTERS, RISK_FILTERS, statusInfo, undoInfo, group, filter, dayLabel, byDay, restoreReceipt, restoreWhy, isUndoable, isUndone, isProblem };
});
