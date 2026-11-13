/* aurora-glass/prototype/notes.js - the Notes screen (window.AuroraNotes).
 *
 * SIMULATION ONLY. Notes are kept in THIS BROWSER's localStorage (key aurora-glass.notes.v1, see notes-model.js). They are not AuroraOS
 * storage, not an AuroraFS file, and never go through the Action Broker. The screen says so. All note text reaches the DOM through
 * textContent / .value only. A failed save never discards text: the editor keeps it and the status says "Not saved: ...".
 */
(function () {
  "use strict";
  const UI = window.AuroraUI, Shell = window.AuroraShell, M = window.AuroraNotesModel;
  if (!UI || !Shell || !M) return;
  const { el, icon } = UI;

  const AUTOSAVE_MS = 600;
  const screen = Shell.addScreen("screen-notes", "Notes");
  const phone = () => window.matchMedia("(max-width: 767px)").matches;

  function getStorage() { try { return window.localStorage || null; } catch (e) { return null; } }
  const store = M.createStore({ storage: getStorage() });
  const loaded = store.load();

  const ui = { query: "", space: "all", selected: null, view: "list", overLimit: null, saveTimer: null, banner: loaded.message && loaded.status !== "empty" ? loaded.message : "" };
  let els = {};

  // ---------------- status ----------------
  function setStatus(text, tone) {
    if (!els.status) return;
    els.status.textContent = text;
    els.status.dataset.tone = tone || "";
    els.retry.hidden = tone !== "error" || !!ui.overLimit;
  }
  function refreshStatus() {
    if (ui.overLimit) { setStatus("Not saved: " + ui.overLimit, "error"); return; }
    if (store.isDirty()) { setStatus(store.lastError() ? "Not saved: " + store.lastError() : "Saving…", store.lastError() ? "error" : "busy"); return; }
    const n = ui.selected && store.get(ui.selected);
    const at = store.lastSaved() || (n && n.updated);
    setStatus(n ? "Saved " + M.stamp(at) : "", "ok");
  }

  /** Write now. Returns the model's result and updates the status/announcements. */
  function saveNow(opts) {
    clearTimeout(ui.saveTimer); ui.saveTimer = null;
    const r = store.flush();
    refreshStatus();
    if (!r.ok) Shell.announce("Not saved: " + r.error.message);
    else if (opts && opts.announce) Shell.announce("Saved in this browser");
    renderList();
    return r;
  }
  function scheduleSave() {
    clearTimeout(ui.saveTimer);
    refreshStatus();
    ui.saveTimer = setTimeout(() => { ui.saveTimer = null; saveNow(); }, AUTOSAVE_MS);
  }

  // ---------------- selection and navigation ----------------
  const current = () => (ui.selected ? store.get(ui.selected) : null);

  /** True when the editor holds text the model refused (over a limit): leaving the note would lose it, so navigation is blocked. */
  function guardOverLimit() {
    if (!ui.overLimit) return false;
    Shell.showToast("Not saved: " + ui.overLimit + " Shorten the text first.", { tone: "warn" });
    return true;
  }

  function showEditorFields() {
    const n = current();
    els.form.hidden = !n; els.hint.hidden = !!n;
    if (!n) return;
    if (els.title.value !== n.title) els.title.value = n.title;
    if (els.body.value !== n.body) els.body.value = n.body;
    renderSpaceSelect(n.space);
    syncPin(n);
    updateCount();
  }

  function openNote(id, o) {
    const opts = o || {};
    if (id !== ui.selected && guardOverLimit()) return false;
    if (id !== ui.selected && store.isDirty()) saveNow();
    ui.selected = id; ui.overLimit = null;
    if (phone()) ui.view = "editor";
    els.layout.dataset.view = ui.view;
    showEditorFields(); refreshStatus(); renderList();
    if (opts.focus !== false) (opts.focusBody ? els.body : els.title).focus({ preventScroll: true });
    return true;
  }

  function backToList(focusItem) {
    if (guardOverLimit()) return;
    if (store.isDirty()) saveNow();
    ui.view = "list"; els.layout.dataset.view = "list";
    renderList();
    if (focusItem !== false) {
      const b = ui.selected && els.list.querySelector('[data-note-id="' + ui.selected + '"]');
      (b || els.search).focus({ preventScroll: true });
    }
  }

  function newNote() {
    // An untouched empty note is reused instead of piling up blank ones.
    const cur = current();
    if (cur && !cur.title && !cur.body && !ui.overLimit) { openNote(cur.id); return; }
    if (guardOverLimit()) return;
    const sp = Shell.state.activeSpace;
    const r = store.create({ space: /^[a-z0-9][a-z0-9-]{0,39}$/.test(sp) ? sp : "study" });
    if (!r.ok) { Shell.showToast(r.error.message, { tone: "warn" }); return; }
    if (ui.query || (ui.space !== "all" && ui.space !== r.note.space)) { ui.query = ""; ui.space = "all"; els.search.value = ""; renderChips(); }
    ui.selected = r.note.id; ui.overLimit = null;
    if (phone()) ui.view = "editor";
    els.layout.dataset.view = ui.view;
    showEditorFields();
    saveNow();
    els.title.focus({ preventScroll: true });
    Shell.announce("New note");
  }

  // ---------------- editing ----------------
  function onEdit() {
    const n = current(); if (!n) return;
    const t = els.title.value, b = els.body.value;
    const err = M.checkTitle(t) || M.checkBody(b);
    if (err) {                                     // refused by the model: keep the text in the editor, never write it, say so
      ui.overLimit = err.error.message;
      refreshStatus(); updateCount(); return;
    }
    ui.overLimit = null;
    const r = store.update(n.id, { title: t, body: b });
    if (!r.ok) { setStatus("Not saved: " + r.error.message, "error"); return; }
    updateCount();
    if (r.changed) scheduleSave(); else refreshStatus();
  }

  function updateCount() {
    const b = els.body.value, t = els.title.value;
    const words = M.wordCount(b);
    const near = b.length >= M.LIMITS.body * 0.9 || b.length > M.LIMITS.body || t.length > M.LIMITS.title;
    els.count.textContent = words + (words === 1 ? " word" : " words") + (near ? " · " + b.length.toLocaleString("en-US") + " / " + M.LIMITS.body.toLocaleString("en-US") + " characters" : "");
    els.count.classList.toggle("is-warn", b.length > M.LIMITS.body || t.length > M.LIMITS.title);
    els.titleCount.textContent = t.length >= M.LIMITS.title * 0.8 || t.length > M.LIMITS.title ? t.length + " / " + M.LIMITS.title : "";
  }

  function syncPin(n) {
    els.pin.setAttribute("aria-pressed", String(!!n.pinned));
    els.pin.setAttribute("aria-label", n.pinned ? "Unpin note" : "Pin note");
    els.pin.querySelector(".pin-label").textContent = n.pinned ? "Pinned" : "Pin";
    els.pin.classList.toggle("is-on", !!n.pinned);
  }

  function togglePin() {
    const n = current(); if (!n) return;
    const r = store.pin(n.id); if (!r.ok) return;
    syncPin(r.note); saveNow();
    Shell.announce(r.note.pinned ? "Note pinned" : "Note unpinned");
  }

  function changeSpace(space) {
    const n = current(); if (!n) return;
    const r = store.update(n.id, { space });
    if (!r.ok) { Shell.showToast(r.error.message, { tone: "warn" }); return; }
    saveNow();
  }

  async function deleteNote() {
    const n = current(); if (!n) return;
    const label = n.title || "Untitled note";
    const ok = await UI.confirm({ title: "Delete this note?", body: "“" + label + "” will be removed from this browser. You can undo right after.", confirmLabel: "Delete note", cancelLabel: "Keep note", danger: true, confirmId: "btn-notes-delete-confirm", cancelId: "btn-notes-delete-cancel" });
    if (!ok) { els.del.focus({ preventScroll: true }); return; }
    const r = store.remove(n.id);
    if (!r.ok) { Shell.showToast(r.error.message, { tone: "warn" }); return; }
    ui.selected = null; ui.overLimit = null;
    if (phone()) { ui.view = "list"; els.layout.dataset.view = "list"; }
    showEditorFields();
    const w = saveNow();
    Shell.showToast(w.ok ? "Note deleted" : "Note deleted, but not saved: " + w.error.message, {
      tone: w.ok ? undefined : "warn",
      action: { label: "Undo", run: () => undoDelete() },
    });
    (phone() ? els.search : els.newBtn).focus({ preventScroll: true });
  }

  function undoDelete() {
    const r = store.restore();
    if (!r.ok) { Shell.showToast(r.error.message, { tone: "warn" }); return; }
    ui.query = ""; els.search.value = ""; ui.space = "all"; renderChips();
    saveNow();
    ui.selected = r.note.id;
    if (!phone()) showEditorFields();
    refreshStatus(); renderList();
    Shell.announce("Note restored");
  }

  // ---------------- rendering ----------------
  function renderSpaceSelect(value) {
    UI.clear(els.space);
    const sp = Shell.spaces();
    if (!sp.some((s) => s.id === value)) sp.push({ id: value, name: value });
    sp.forEach((s) => els.space.appendChild(el("option", { value: s.id, text: s.name, selected: s.id === value ? true : undefined })));
    els.space.value = value;
  }

  function spaceName(id) { const s = Shell.spaces().find((x) => x.id === id); return s ? s.name : id; }

  function renderChips() {
    UI.clear(els.chips);
    [{ id: "all", name: "All" }].concat(Shell.spaces()).forEach((s) => els.chips.appendChild(el("button", {
      type: "button", class: "trail-filter", id: "btn-notes-space-" + s.id, "data-space": s.id, "aria-pressed": String(ui.space === s.id), text: s.name,
      onclick: () => { ui.space = s.id; renderChips(); renderList(); },
    })));
  }

  function renderList() {
    if (!els.list) return;
    const hadFocus = els.list.contains(document.activeElement) ? document.activeElement.getAttribute("data-note-id") : null;
    const all = store.count();
    const shown = store.list({ query: ui.query, space: ui.space });
    UI.clear(els.list);
    els.emptyBox.hidden = true; UI.clear(els.emptyBox);
    if (!all) {
      els.emptyBox.hidden = false;
      els.emptyBox.appendChild(UI.emptyState("notes", "No notes yet", "Write something down. Notes are saved in this browser only.", UI.button("New note", { kind: "accent", icon: "plus", id: "btn-notes-empty-new", onclick: newNote })));
    } else if (!shown.length) {
      els.emptyBox.hidden = false;
      const filtered = ui.query.trim() ? "No notes match “" + ui.query.trim() + "”" : "No notes in " + spaceName(ui.space);
      els.emptyBox.appendChild(UI.emptyState("search", filtered, "Try a different word, or clear the search and Space filter.", UI.button("Clear search and filter", { kind: "outline", id: "btn-notes-clear-filter", onclick: () => { ui.query = ""; ui.space = "all"; els.search.value = ""; renderChips(); renderList(); } })));
    }
    shown.forEach((n) => {
      const label = (n.title || "Untitled note");
      const meta = [n.pinned ? "Pinned" : null, spaceName(n.space), M.stamp(n.updated)].filter(Boolean).join(" · ");
      els.list.appendChild(el("li", {}, [el("button", {
        type: "button", class: "note-item" + (n.id === ui.selected ? " is-selected" : ""), id: "btn-note-" + n.id, "data-note-id": n.id, "data-pinned": n.pinned ? "true" : "false",
        "aria-current": n.id === ui.selected ? "true" : undefined,
        "aria-label": label + ". " + meta + (n.body ? ". " + M.snippet(n.body, 60) : ""),
        onclick: () => openNote(n.id),
      }, [
        el("span", { class: "note-item-title" }, [n.pinned ? icon("pin") : null, el("span", { class: "note-item-name" + (n.title ? "" : " is-untitled"), text: label })]),
        n.body ? el("span", { class: "note-item-snippet", text: M.snippet(n.body, 90) }) : null,
        el("span", { class: "note-item-meta", text: meta }),
      ])]));
    });
    els.listCount.textContent = !all ? "" : (shown.length === all ? all + (all === 1 ? " note" : " notes") : "Showing " + shown.length + " of " + all + (all === 1 ? " note" : " notes"));
    if (hadFocus) { const b = els.list.querySelector('[data-note-id="' + hadFocus + '"]'); if (b) b.focus({ preventScroll: true }); }
  }

  function build() {
    UI.clear(screen);
    screen.appendChild(el("header", { class: "screen-header" }, [
      el("button", { type: "button", class: "btn btn-icon ghost", id: "btn-notes-home", "aria-label": "Back", title: "Back", onclick: () => Shell.back() }, [icon("back")]),
      el("div", { class: "screen-heading" }, [el("p", { class: "eyebrow", text: "Write things down" }), el("h1", { class: "screen-title", text: "Notes" })]),
    ]));
    els.banner = el("p", { class: "callout notes-banner", id: "notes-banner", role: "status", hidden: true }, [icon("alert"), el("span", { id: "notes-banner-text" }),
      el("button", { type: "button", class: "btn ghost btn-small", id: "btn-notes-banner-dismiss", onclick: () => { ui.banner = ""; syncBanner(); } }, [el("span", { text: "Dismiss" })])]);
    screen.appendChild(els.banner);
    screen.appendChild(el("p", { class: "callout notes-scope", id: "notes-scope" }, [icon("info"), el("span", { text: "Saved in this browser only · not AuroraOS storage" })]));

    els.search = el("input", { type: "search", class: "field", id: "notes-search", placeholder: "Search notes", "aria-label": "Search notes", autocomplete: "off", oninput: (e) => { ui.query = e.target.value; renderList(); } });
    els.chips = el("div", { class: "trail-chips", id: "notes-spaces", role: "group", "aria-label": "Filter by Space" });
    els.newBtn = el("button", { type: "button", class: "btn accent", id: "btn-notes-new", onclick: newNote, title: "New note (Ctrl+N)" }, [icon("plus"), el("span", { text: "New note" })]);
    els.listCount = el("p", { class: "row-meta", id: "notes-list-count", role: "status" });
    els.list = el("ul", { class: "notes-list", id: "notes-list", "aria-label": "Notes", onkeydown: onListKey });
    els.emptyBox = el("div", { id: "notes-empty", hidden: true });
    const listPane = el("section", { class: "notes-listpane", id: "notes-listpane", "aria-label": "Note list" }, [
      el("div", { class: "notes-listtools" }, [el("div", { class: "search-field" }, [icon("search"), els.search]), els.newBtn]),
      els.chips, els.listCount, els.emptyBox, els.list]);

    els.title = el("input", { type: "text", class: "field notes-title", id: "notes-title", placeholder: "Title", "aria-label": "Note title", autocomplete: "off", oninput: onEdit });
    els.titleCount = el("span", { class: "row-meta", id: "notes-title-count" });
    els.body = el("textarea", { class: "field notes-body", id: "notes-body", placeholder: "Start writing…", "aria-label": "Note text", spellcheck: "true", oninput: onEdit });
    els.space = el("select", { id: "notes-space", onchange: (e) => changeSpace(e.target.value) });
    els.pin = el("button", { type: "button", class: "btn outline btn-small notes-pin", id: "btn-notes-pin", "aria-pressed": "false", onclick: togglePin }, [icon("pin"), el("span", { class: "pin-label", text: "Pin" })]);
    els.del = el("button", { type: "button", class: "btn outline btn-small notes-delete", id: "btn-notes-delete", onclick: deleteNote }, [icon("trash"), el("span", { text: "Delete" })]);
    els.back = el("button", { type: "button", class: "btn ghost btn-small notes-back", id: "btn-notes-back", onclick: () => backToList() }, [icon("back"), el("span", { text: "All notes" })]);
    els.status = el("p", { class: "notes-status", id: "notes-status", "aria-live": "off" });
    els.retry = el("button", { type: "button", class: "btn outline btn-small", id: "btn-notes-retry", hidden: true, onclick: () => saveNow({ announce: true }) }, [el("span", { text: "Try again" })]);
    els.count = el("p", { class: "row-meta notes-count", id: "notes-count" });
    els.form = el("div", { class: "notes-form", id: "notes-form", hidden: true }, [
      el("div", { class: "notes-bar" }, [els.back, el("div", { class: "control-group notes-spacepick" }, [el("label", { for: "notes-space", text: "Space" }), els.space]), els.pin, els.del]),
      els.title, els.titleCount,
      el("div", { class: "notes-foot" }, [els.status, els.retry, els.count]),     // above the text so "Not saved" is never pushed below the fold
      els.body,
    ]);
    els.hint = el("div", { class: "notes-hint", id: "notes-hint" }, [UI.emptyState("notes", "No note open", "Choose a note from the list, or start a new one.", UI.button("New note", { kind: "outline", icon: "plus", id: "btn-notes-hint-new", onclick: newNote }))]);
    const editor = el("section", { class: "card opaque notes-editor", id: "notes-editor", "aria-label": "Note editor" }, [els.form, els.hint]);

    els.layout = el("div", { class: "notes-layout", id: "notes-layout", "data-view": ui.view }, [el("div", { class: "card opaque notes-listcard" }, [listPane]), editor]);
    screen.appendChild(els.layout);
    renderChips(); syncBanner(); renderList(); showEditorFields(); refreshStatus();
  }

  function syncBanner() {
    const text = ui.banner || (store.hasStorage() ? "" : loaded.message || "");
    els.banner.hidden = !text;
    els.banner.querySelector("#notes-banner-text").textContent = text;
  }

  function onListKey(e) {
    const items = Array.from(els.list.querySelectorAll(".note-item")); if (!items.length) return;
    const i = items.indexOf(document.activeElement); if (i < 0) return;
    let j = null;
    if (e.key === "ArrowDown") j = Math.min(items.length - 1, i + 1);
    else if (e.key === "ArrowUp") j = Math.max(0, i - 1);
    else if (e.key === "Home") j = 0;
    else if (e.key === "End") j = items.length - 1;
    if (j === null) return;
    e.preventDefault(); items[j].focus();
  }

  // ---------------- keyboard: only while this screen is active and no dialog or Index overlay is open ----------------
  const live = () => screen.classList.contains("active");
  function onKey(e) {
    if (!live() || e.defaultPrevented) return;
    if (document.querySelector(".scrim, [aria-modal='true']")) return;
    const mod = (e.ctrlKey || e.metaKey) && !e.altKey && !e.shiftKey, k = (e.key || "").toLowerCase();
    if (mod && k === "n") { e.preventDefault(); newNote(); return; }
    if (mod && k === "s") { e.preventDefault(); saveNow({ announce: true }); return; }
    if (mod && k === "f") {
      e.preventDefault();
      if (phone() && ui.view === "editor") { if (guardOverLimit()) return; backToList(false); }
      els.search.focus(); els.search.select(); return;
    }
    if (e.key === "Escape") {
      if (e.target === els.search && els.search.value) { e.preventDefault(); e.stopPropagation(); els.search.value = ""; ui.query = ""; renderList(); return; }
      if (phone() && ui.view === "editor") { e.preventDefault(); e.stopPropagation(); backToList(); }
    }
  }
  document.addEventListener("keydown", onKey, true);

  // ---------------- never leave unsaved text behind silently ----------------
  function flushQuietly() { if (ui.saveTimer || store.isDirty()) saveNow(); }
  document.addEventListener("visibilitychange", () => { if (document.visibilityState === "hidden") flushQuietly(); });
  window.addEventListener("pagehide", flushQuietly);
  window.addEventListener("beforeunload", (e) => { if (ui.overLimit || (store.isDirty() && store.lastError())) { e.preventDefault(); e.returnValue = ""; } });
  Shell.onScreenShow((id) => {
    if (id !== "screen-notes") { if (ui.saveTimer || store.isDirty()) saveNow(); return; }
    renderChips(); syncBanner();
    if (!phone() && !ui.selected) { const first = store.list({ query: ui.query, space: ui.space })[0]; if (first) { ui.selected = first.id; showEditorFields(); } }
    els.layout.dataset.view = ui.view; renderList(); refreshStatus();
  });
  Shell.onSpaceChange(() => { if (els.chips) { renderChips(); const n = current(); if (n) renderSpaceSelect(n.space); } });

  build();

  /** Re-read localStorage and rebuild the screen (used after the stored data was replaced from outside, e.g. by the layout matrix). */
  function reload() {
    clearTimeout(ui.saveTimer); ui.saveTimer = null;
    const r = store.load();
    ui.selected = null; ui.view = "list"; ui.overLimit = null; ui.query = ""; ui.space = "all";
    ui.banner = r.message && r.status !== "empty" ? r.message : "";
    loaded.message = r.message; loaded.status = r.status;
    build();
    if (!phone()) { const first = store.list({})[0]; if (first) { ui.selected = first.id; showEditorFields(); refreshStatus(); renderList(); } }
    return r.status;
  }

  window.AuroraNotes = {
    reload, open: (id) => openNote(id, { focus: false }),
    /** Test/automation hooks. */
    flush: () => saveNow(),
    state: () => ({ selected: ui.selected, view: ui.view, query: ui.query, space: ui.space, count: store.count(), dirty: store.isDirty(), overLimit: ui.overLimit, lastError: store.lastError(), hasStorage: store.hasStorage() }),
    notes: () => store.list({}),
    AUTOSAVE_MS,
  };
})();
