/* aurora-glass/prototype/index-model.js - DOM-free relevance model for Index (window.AuroraIndexModel).
 *
 * SIMULATION ONLY. Pure functions that decide how well a query matches a label and in what order results appear. Deterministic: the same query, items and
 * recent list always give the same order. Match tiers, best first: exact, prefix, word-prefix (every typed word starts some word of the label), substring,
 * keyword, then a conservative typo match used only for APP NAMES. A typo match is a suggestion: it is flagged `fuzzy`, ranks last, and is never the
 * default (Enter does nothing until the person chooses it), so a misspelling can never run something by itself. Recency only breaks ties inside a tier; it
 * never lifts a weaker match above a stronger one. Loadable in the browser and in Node.
 */
(function (root, factory) {
  if (typeof module === "object" && module.exports) module.exports = factory();
  else root.AuroraIndexModel = factory();
})(typeof self !== "undefined" ? self : this, function () {
  "use strict";

  const TIER = Object.freeze({ EXACT: 0, PREFIX: 1, WORD_PREFIX: 2, SUBSTRING: 3, KEYWORD: 4, FUZZY: 5, NONE: -1 });
  const MAX_RECENT = 20, MIN_FUZZY_LEN = 4;

  /** Lower-case, strip accents, collapse whitespace. */
  function norm(s) { return String(s === undefined || s === null ? "" : s).normalize("NFKD").replace(/[̀-ͯ]/g, "").toLowerCase().replace(/\s+/g, " ").trim(); }
  const words = (s) => norm(s).split(/[\s\-_.·]+/).filter(Boolean);

  /** Strongest non-fuzzy tier of `q` against `label` and `keywords`; NONE when nothing matches. An empty query matches everything at PREFIX. */
  function matchTier(q, label, keywords) {
    const nq = norm(q); if (!nq) return TIER.PREFIX;
    const l = norm(label);
    if (l === nq) return TIER.EXACT;
    if (l.startsWith(nq)) return TIER.PREFIX;
    const lw = words(label), qw = words(q);
    if (lw.some((w) => w.startsWith(nq))) return TIER.WORD_PREFIX;
    if (qw.length > 1 && qw.every((t) => lw.some((w) => w.startsWith(t)))) return TIER.WORD_PREFIX;      // "mode dark" finds "Dark mode"
    if (l.includes(nq)) return TIER.SUBSTRING;
    if ((keywords || []).some((k) => norm(k).includes(nq))) return TIER.KEYWORD;
    return TIER.NONE;
  }

  /** Optimal-string-alignment distance (insert, delete, substitute, swap two neighbours each cost 1). Returns max + 1 as soon as it must exceed max. */
  function editDistance(a, b, max) {
    a = String(a); b = String(b);
    const lim = typeof max === "number" ? max : Infinity;
    if (Math.abs(a.length - b.length) > lim) return lim + 1;
    const n = a.length, m = b.length;
    let prev2 = null, prev = new Array(m + 1), cur = new Array(m + 1);
    for (let j = 0; j <= m; j++) prev[j] = j;
    for (let i = 1; i <= n; i++) {
      cur[0] = i; let rowMin = cur[0];
      for (let j = 1; j <= m; j++) {
        const cost = a[i - 1] === b[j - 1] ? 0 : 1;
        let v = Math.min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost);
        if (i > 1 && j > 1 && a[i - 1] === b[j - 2] && a[i - 2] === b[j - 1]) v = Math.min(v, prev2[j - 2] + 1);
        cur[j] = v; if (v < rowMin) rowMin = v;
      }
      if (rowMin > lim) return lim + 1;
      prev2 = prev; prev = cur; cur = new Array(m + 1);
    }
    return prev[m];
  }

  /**
   * Conservative typo match of a query against a name. Needs a query of at least 4 characters; allows 1 edit (2 from 8 characters); the first letter must
   * agree (people rarely mistype it, and it removes most false hits); only whole words or the whole name are compared, never fragments.
   * Anything that already matches (exact/prefix/substring) is not "fuzzy". Returns { ok, distance }.
   */
  function fuzzyMatch(q, name) {
    const nq = norm(q);
    if (nq.length < MIN_FUZZY_LEN || /\s/.test(nq)) return { ok: false, distance: -1 };
    if (matchTier(nq, name, []) !== TIER.NONE) return { ok: false, distance: -1 };
    const allow = nq.length >= 8 ? 2 : 1;
    let best = Infinity;
    const cands = [norm(name)].concat(words(name));
    cands.forEach((c) => {
      if (c.length < MIN_FUZZY_LEN || c[0] !== nq[0]) return;
      const d = editDistance(nq, c, allow);
      if (d <= allow && d < best) best = d;
    });
    return best === Infinity ? { ok: false, distance: -1 } : { ok: true, distance: best };
  }

  /** Recent list helpers: most recent first, no duplicates, bounded. Pure. */
  function pushRecent(list, key, max) {
    if (typeof key !== "string" || !key) return (list || []).slice();
    const m = typeof max === "number" && max > 0 ? max : MAX_RECENT;
    return [key].concat((list || []).filter((k) => k !== key)).slice(0, m);
  }
  const recencyOf = (list, key) => { const i = (list || []).indexOf(key); return i < 0 ? Infinity : i; };

  /**
   * Deterministic order for items of one group. Each item: { rank (a TIER), key?, n (source order), fuzzy? }.
   * Better tier first; inside a tier, more recently used first; then source order. Never mutates its input.
   */
  function order(items, recent) {
    return (items || []).map((it, i) => ({ it, i })).sort((x, y) => {
      if (x.it.rank !== y.it.rank) return x.it.rank - y.it.rank;
      const rx = recencyOf(recent, x.it.key), ry = recencyOf(recent, y.it.key);
      if (rx !== ry) return rx < ry ? -1 : 1;
      const nx = typeof x.it.n === "number" ? x.it.n : x.i, ny = typeof y.it.n === "number" ? y.it.n : y.i;
      return nx - ny || x.i - y.i;
    }).map((p) => p.it);
  }

  /** Index of the result Enter should open when nothing was chosen: the first non-fuzzy one, or -1 (a typo suggestion is never opened by default). */
  function defaultActive(items) {
    for (let i = 0; i < (items || []).length; i++) if (!items[i].fuzzy) return i;
    return -1;
  }

  return { TIER, MAX_RECENT, MIN_FUZZY_LEN, norm, matchTier, editDistance, fuzzyMatch, pushRecent, recencyOf, order, defaultActive };
});
