/* aurora-glass/prototype/notes-model.js - DOM-free model for the Notes screen (window.AuroraNotesModel).
 *
 * SIMULATION ONLY. Notes live in this browser's localStorage under one versioned envelope (aurora-glass.notes.v1). They are NOT
 * AuroraOS storage, never reach the Action Broker, and are not a native AuroraFS file. The store takes an injected storage, clock and id
 * generator so unit tests can fail it on purpose. It never throws on bad storage or bad input: every operation returns {ok, ...}.
 * Loadable in the browser and in Node.
 */
(function (root, factory) {
  if (typeof module === "object" && module.exports) module.exports = factory();
  else root.AuroraNotesModel = factory();
})(typeof self !== "undefined" ? self : this, function () {
  "use strict";

  const KEY = "aurora-glass.notes.v1";
  const BACKUP_KEY = KEY + ".unreadable";
  const VERSION = 1;
  const LIMITS = Object.freeze({ title: 120, body: 20000, notes: 200 });
  const ID_RE = /^[A-Za-z0-9_-]{1,64}$/;
  const SPACE_RE = /^[a-z0-9][a-z0-9-]{0,39}$/;
  const has = (o, k) => Object.prototype.hasOwnProperty.call(o, k);
  const fmt = (n) => String(n).replace(/\B(?=(\d{3})+(?!\d))/g, ",");

  const fail = (code, message) => ({ ok: false, error: { code, message } });

  // ---------------- pure helpers ----------------
  function wordCount(text) { const m = String(text == null ? "" : text).match(/\S+/g); return m ? m.length : 0; }

  /** Validate one stored/new note. Returns null when fine, else a short reason. */
  function noteProblem(n) {
    if (!n || typeof n !== "object" || Array.isArray(n)) return "not an object";
    if (typeof n.id !== "string" || !ID_RE.test(n.id)) return "bad id";
    if (typeof n.title !== "string") return "title is not text";
    if (typeof n.body !== "string") return "body is not text";
    if (n.title.length > LIMITS.title) return "title is longer than " + LIMITS.title;
    if (n.body.length > LIMITS.body) return "body is longer than " + fmt(LIMITS.body);
    if (typeof n.pinned !== "boolean") return "pinned is not true/false";
    if (typeof n.space !== "string" || !SPACE_RE.test(n.space)) return "bad space";
    if (!Number.isSafeInteger(n.created) || n.created < 0) return "bad created time";
    if (!Number.isSafeInteger(n.updated) || n.updated < 0) return "bad updated time";
    return null;
  }

  function checkTitle(t) {
    if (typeof t !== "string") return fail("title-type", "The title must be text.");
    if (t.length > LIMITS.title) return fail("title-too-long", "Titles are limited to " + LIMITS.title + " characters (" + (t.length - LIMITS.title) + " too many).");
    return null;
  }
  function checkBody(b) {
    if (typeof b !== "string") return fail("body-type", "The note text must be text.");
    if (b.length > LIMITS.body) return fail("body-too-long", "Notes are limited to " + fmt(LIMITS.body) + " characters (" + fmt(b.length - LIMITS.body) + " too many).");
    return null;
  }

  /** Pinned first, then most recently updated, then id (so the order is always the same). */
  function sortNotes(list) {
    return list.slice().sort((a, b) => (a.pinned === b.pinned ? 0 : a.pinned ? -1 : 1) || b.updated - a.updated || (a.id < b.id ? -1 : a.id > b.id ? 1 : 0));
  }

  /** Case-insensitive search of title and body; every word of the query must appear somewhere. Empty query matches all. */
  function matches(note, query) {
    const words = String(query || "").toLowerCase().split(/\s+/).filter(Boolean);
    if (!words.length) return true;
    const hay = (note.title + "\n" + note.body).toLowerCase();
    return words.every((w) => hay.indexOf(w) !== -1);
  }

  function snippet(body, max) {
    const s = String(body).replace(/\s+/g, " ").trim();
    const m = max || 90;
    return s.length > m ? s.slice(0, m - 1) + "…" : s;
  }

  /** "14:02" today, otherwise "12 Nov" (local time). */
  function stamp(ts, now) {
    const d = new Date(ts), n = new Date(now === undefined ? Date.now() : now);
    const pad = (x) => (x < 10 ? "0" : "") + x;
    if (d.getFullYear() === n.getFullYear() && d.getMonth() === n.getMonth() && d.getDate() === n.getDate()) return pad(d.getHours()) + ":" + pad(d.getMinutes());
    return d.getDate() + " " + ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"][d.getMonth()];
  }

  /** Turn a thrown storage error into the sentence the UI shows after "Not saved: ". */
  function storageReason(e) {
    const name = e && e.name ? String(e.name) : "";
    if (name === "QuotaExceededError" || name === "NS_ERROR_DOM_QUOTA_REACHED" || (e && (e.code === 22 || e.code === 1014))) return "browser storage is full";
    if (name === "SecurityError") return "browser storage is blocked";
    return "browser storage is unavailable";
  }

  /** Parse the stored envelope. Never throws. Returns {notes, dropped, problem?}; problem set means the data was unusable as a whole. */
  function parse(raw) {
    if (raw === null || raw === undefined) return { notes: [], dropped: 0 };
    let data;
    try { data = JSON.parse(raw); } catch (e) { return { notes: [], dropped: 0, problem: "the saved data is not valid JSON" }; }
    if (!data || typeof data !== "object" || Array.isArray(data)) return { notes: [], dropped: 0, problem: "the saved data has an unknown shape" };
    if (data.v !== VERSION) return { notes: [], dropped: 0, problem: typeof data.v === "number" && data.v > VERSION ? "the saved data is from a newer version" : "the saved data has an unknown version" };
    if (!Array.isArray(data.notes)) return { notes: [], dropped: 0, problem: "the saved data has no note list" };
    const seen = new Set(), out = [];
    let dropped = 0;
    for (const n of data.notes) {
      if (noteProblem(n) || seen.has(n.id) || out.length >= LIMITS.notes) { dropped++; continue; }
      seen.add(n.id);
      out.push({ id: n.id, title: n.title, body: n.body, pinned: n.pinned, space: n.space, created: n.created, updated: n.updated });
    }
    return { notes: out, dropped };
  }

  const serialize = (notes) => JSON.stringify({ v: VERSION, notes: notes.map((n) => ({ id: n.id, title: n.title, body: n.body, pinned: n.pinned, space: n.space, created: n.created, updated: n.updated })) });

  // ---------------- the store ----------------
  /**
   * createStore({ storage, now, newId }). storage: object with getItem/setItem/removeItem, or null/undefined when the browser has none.
   * Edits mark the store dirty; flush() writes. A failed flush keeps every note in memory and stays dirty so the next flush retries.
   */
  function createStore(opts) {
    const o = opts || {};
    const now = o.now || (() => Date.now());
    let counter = 0;
    const newId = o.newId || (() => "n" + now().toString(36) + Math.random().toString(36).slice(2, 8) + (counter++).toString(36));
    let notes = [], trash = null, dirty = false, lastSaved = null, lastError = null;
    let storage = o.storage || null;

    function find(id) { return notes.find((n) => n.id === id) || null; }
    const copy = (n) => (n ? { id: n.id, title: n.title, body: n.body, pinned: n.pinned, space: n.space, created: n.created, updated: n.updated } : null);

    /** Read the stored notes. status: empty | loaded | unavailable | recovered (unusable data was moved to a backup key, or some notes dropped). */
    function load() {
      notes = []; trash = null; dirty = false; lastError = null;
      if (!storage) return { ok: false, status: "unavailable", message: "This browser does not allow saving notes here. Notes you write stay on this page until you leave it." };
      let raw;
      try { raw = storage.getItem(KEY); } catch (e) { storage = null; return { ok: false, status: "unavailable", message: "This browser does not allow saving notes here. Notes you write stay on this page until you leave it." }; }
      if (raw === null || raw === undefined) return { ok: true, status: "empty", dropped: 0 };
      const p = parse(raw);
      if (p.problem) {
        let kept = false;
        try { storage.setItem(BACKUP_KEY, raw); kept = true; } catch (e) { /* no room for a backup: say so below */ }
        return { ok: true, status: "recovered", dropped: 0, message: "Your saved notes could not be read (" + p.problem + "), so Notes started empty. " + (kept ? "The unreadable data was kept in this browser under " + BACKUP_KEY + "." : "A backup copy could not be made.") };
      }
      notes = p.notes;
      if (p.dropped) {
        try { storage.setItem(BACKUP_KEY, raw); } catch (e) { /* reported below */ }
        return { ok: true, status: "recovered", dropped: p.dropped, message: p.dropped + (p.dropped === 1 ? " saved note was" : " saved notes were") + " not valid and " + (p.dropped === 1 ? "was" : "were") + " left out. The original data was kept in this browser under " + BACKUP_KEY + "." };
      }
      return { ok: true, status: notes.length ? "loaded" : "empty", dropped: 0 };
    }

    /** Write all notes now. {ok:true, at} | {ok:false, error:{code:"not-saved", message}}. Clean store: ok without writing. */
    function flush() {
      if (!dirty) return { ok: true, at: lastSaved, wrote: false };
      if (!storage) { lastError = "browser storage is unavailable"; return fail("not-saved", lastError); }
      try {
        storage.setItem(KEY, serialize(notes));
      } catch (e) { lastError = storageReason(e); return fail("not-saved", lastError); }
      dirty = false; lastError = null; lastSaved = now();
      return { ok: true, at: lastSaved, wrote: true };
    }

    function create(init) {
      const i = init || {};
      if (notes.length >= LIMITS.notes) return fail("too-many", "Up to " + LIMITS.notes + " notes are supported. Delete one first.");
      const title = i.title === undefined ? "" : i.title, body = i.body === undefined ? "" : i.body;
      const space = i.space === undefined ? "study" : i.space;
      const e = checkTitle(title) || checkBody(body) || (typeof space === "string" && SPACE_RE.test(space) ? null : fail("bad-space", "That Space is not valid."));
      if (e) return e;
      let id = newId(); while (find(id) || !ID_RE.test(id)) id = "n" + now().toString(36) + Math.random().toString(36).slice(2, 10);
      const t = now();
      const n = { id, title, body, pinned: false, space, created: t, updated: t };
      notes.push(n); dirty = true;
      return { ok: true, note: copy(n) };
    }

    /** Change title, body and/or space. Nothing changes if any field is refused. Unchanged values do not touch `updated`. */
    function update(id, patch) {
      const n = find(id); if (!n) return fail("not-found", "That note no longer exists.");
      const p = patch || {};
      if (has(p, "title")) { const e = checkTitle(p.title); if (e) return e; }
      if (has(p, "body")) { const e = checkBody(p.body); if (e) return e; }
      if (has(p, "space") && !(typeof p.space === "string" && SPACE_RE.test(p.space))) return fail("bad-space", "That Space is not valid.");
      let changed = false;
      for (const k of ["title", "body", "space"]) if (has(p, k) && p[k] !== n[k]) { n[k] = p[k]; changed = true; }
      if (changed) { n.updated = Math.max(now(), n.updated); dirty = true; }
      return { ok: true, note: copy(n), changed };
    }

    function pin(id, value) {
      const n = find(id); if (!n) return fail("not-found", "That note no longer exists.");
      const v = value === undefined ? !n.pinned : !!value;
      if (v !== n.pinned) { n.pinned = v; dirty = true; }                           // pinning is not an edit of the text: `updated` stays
      return { ok: true, note: copy(n) };
    }

    /** Delete a note; the one-level undo buffer holds it (a second delete replaces the buffer). */
    function remove(id) {
      const i = notes.findIndex((n) => n.id === id); if (i < 0) return fail("not-found", "That note no longer exists.");
      trash = notes.splice(i, 1)[0]; dirty = true;
      return { ok: true, note: copy(trash) };
    }

    function restore() {
      if (!trash) return fail("nothing-to-restore", "There is nothing to restore.");
      if (notes.length >= LIMITS.notes) return fail("too-many", "Up to " + LIMITS.notes + " notes are supported, so the deleted note cannot be restored.");
      if (find(trash.id)) return fail("exists", "That note already exists.");
      notes.push(trash); const back = trash; trash = null; dirty = true;
      return { ok: true, note: copy(back) };
    }

    function list(q) {
      const f = q || {};
      return sortNotes(notes.filter((n) => (!f.space || f.space === "all" || n.space === f.space) && matches(n, f.query))).map(copy);
    }

    return {
      load, flush, create, update, pin, remove, restore, list,
      get: (id) => copy(find(id)), count: () => notes.length, canUndo: () => !!trash, undoTitle: () => (trash ? trash.title : null),
      isDirty: () => dirty, lastSaved: () => lastSaved, lastError: () => lastError, hasStorage: () => !!storage,
    };
  }

  return { KEY, BACKUP_KEY, VERSION, LIMITS, wordCount, noteProblem, checkTitle, checkBody, sortNotes, matches, snippet, stamp, storageReason, parse, serialize, createStore };
});
