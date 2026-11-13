/* aurora-glass/prototype/vault-model.js — DOM-free model for the Vault screen (window.AuroraVaultModel).
 *
 * SIMULATION ONLY. Vault lists the prototype's sample files (AuroraSystem.files.list()). This module only sorts, filters,
 * formats and builds request payloads. It owns no files and changes nothing: every move or delete is proposed through
 * AuroraSystem.request and decided by the trusted review surface.
 * Loadable in the browser and in Node for unit tests.
 */
(function (root, factory) {
  if (typeof module === "object" && module.exports) module.exports = factory();
  else root.AuroraVaultModel = factory();
})(typeof self !== "undefined" ? self : this, function () {
  "use strict";

  const has = (o, k) => Object.prototype.hasOwnProperty.call(o, k);
  /** The broker's hard limit for one plan (Broker.MAX_PLAN_STEPS). Larger selections are split into several reviews. */
  const MAX_PLAN_STEPS = 12;
  const SORTS = Object.freeze([["name", "Name"], ["modified", "Modified"], ["size", "Size"]]);
  const KIND_LABEL = Object.freeze({ note: "Note", text: "Text file", table: "Table", image: "Image" });
  const KIND_ICON = Object.freeze({ note: "notes", text: "file", table: "list", image: "image" });

  const kindLabel = (k) => (typeof k === "string" && has(KIND_LABEL, k) ? KIND_LABEL[k] : "File");
  const kindIcon = (k) => (typeof k === "string" && has(KIND_ICON, k) ? KIND_ICON[k] : "file");

  /** "860 B", "4.1 KB", "1.2 MB". Never negative or NaN. */
  function formatSize(bytes) {
    const n = typeof bytes === "number" && isFinite(bytes) && bytes > 0 ? bytes : 0;
    if (n < 1024) return Math.round(n) + " B";
    if (n < 1048576) return (Math.round((n / 1024) * 10) / 10) + " KB";
    return (Math.round((n / 1048576) * 10) / 10) + " MB";
  }

  // One shared collator: String.prototype.localeCompare with options builds a new collator on every call, which dominated sorting hundreds of files per render
  // (measured: tests/perf/profile.js). Same ordering as localeCompare(b, undefined, { numeric: true, sensitivity: "base" }), just not rebuilt per comparison.
  const NAME_ORDER = new Intl.Collator(undefined, { numeric: true, sensitivity: "base" }), ID_ORDER = new Intl.Collator();
  const byName = (a, b) => NAME_ORDER.compare(String(a.name), String(b.name)) || ID_ORDER.compare(String(a.id), String(b.id));
  /**
   * Sorted copy. key: name | modified | size (anything else sorts by name). dir: "asc" | "desc" (anything else is asc).
   * Ties always fall back to name then id so the order is stable and total.
   */
  function sortFiles(list, key, dir) {
    const k = SORTS.some((s) => s[0] === key) ? key : "name";
    const sign = dir === "desc" ? -1 : 1;
    const val = { name: null, modified: (f) => f.modified || 0, size: (f) => f.size || 0 }[k];
    return (Array.isArray(list) ? list : []).slice().sort((a, b) => {
      const d = val ? val(a) - val(b) : 0;
      return (k === "name" ? byName(a, b) : d || byName(a, b)) * sign;
    });
  }

  /** Filter by search text (name, kind, Space, case-insensitive) and Space id ("all" or a Space id). */
  function filterFiles(list, opts) {
    const o = opts || {};
    const q = typeof o.query === "string" ? o.query.trim().toLowerCase() : "";
    const space = typeof o.space === "string" && o.space !== "all" ? o.space : null;
    return (Array.isArray(list) ? list : []).filter((f) => {
      if (space && f.space !== space) return false;
      if (!q) return true;
      return [f.name, kindLabel(f.kind), f.kind, f.space].some((x) => typeof x === "string" && x.toLowerCase().includes(q));
    });
  }

  /** Ids between two ids (inclusive) in the visible order. Unknown ids select just the target. */
  function rangeIds(visibleIds, anchorId, targetId) {
    const a = visibleIds.indexOf(anchorId), b = visibleIds.indexOf(targetId);
    if (b < 0) return [];
    if (a < 0) return [targetId];
    return visibleIds.slice(Math.min(a, b), Math.max(a, b) + 1);
  }

  /** Split ids into runs of at most `size` (the plan limit). Never drops an id. */
  function chunk(ids, size) {
    const n = size > 0 ? Math.floor(size) : MAX_PLAN_STEPS, out = [];
    for (let i = 0; i < ids.length; i += n) out.push(ids.slice(i, i + n));
    return out;
  }

  /** One move action, or a plan of them. */
  function movePayload(ids, destination) {
    const steps = ids.map((id) => ({ type: "file.move", arguments: { id, destination } }));
    return steps.length === 1 ? steps[0] : { steps };
  }
  function deletePayload(ids) {
    const steps = ids.map((id) => ({ type: "file.delete", arguments: { id } }));
    return steps.length === 1 ? steps[0] : { steps };
  }

  /** Which files a move to `destination` would really change (the others are already there). */
  function movable(files, destination) { return files.filter((f) => f.space !== destination); }

  /** "Moved 2 files to Work" style sentences. */
  const noun = (n) => n + (n === 1 ? " file" : " files");

  /**
   * Turn a broker result into a truthful summary. `names` maps step index -> file name; `explain` maps a reason code to words.
   * ok is only true when every step ran. Nothing here invents success: it reads result.steps.
   */
  function summarize(verb, past, result, names, explain) {
    const steps = (result && Array.isArray(result.steps)) ? result.steps : [];
    const done = steps.filter((s) => s.status === "done");
    const bad = steps.filter((s) => s.status !== "done" && s.status !== "skipped");
    const total = names.length;
    if (result && result.cancelled) return { ok: false, tone: "info", text: "Cancelled. Nothing was changed." };
    if (result && result.blocked) return { ok: false, tone: "error", text: "Blocked. Nothing was changed." };
    if (!steps.length) return { ok: false, tone: "error", text: "Could not " + verb + ": " + explain(result && result.reason) };
    if (done.length === total && !bad.length) return { ok: true, tone: "ok", text: past + " " + noun(total) + "." };
    const failed = bad.map((s) => "“" + (names[s.index] || "file") + "”" + (s.error ? " (" + String(explain(s.error)).replace(/[.]$/, "") + ")" : "")).slice(0, 3).join(", ");
    const notRun = total - done.length - bad.length;
    return { ok: false, tone: "error", text: past + " " + done.length + " of " + noun(total) + ". " + (failed ? "Failed: " + failed + ". " : "") + (notRun > 0 ? notRun + " did not run." : "") };
  }

  return { MAX_PLAN_STEPS, SORTS, KIND_LABEL, kindLabel, kindIcon, formatSize, sortFiles, filterFiles, rangeIds, chunk, movePayload, deletePayload, movable, summarize, noun };
});
