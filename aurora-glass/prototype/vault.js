/* aurora-glass/prototype/vault.js — Vault: a file manager over the prototype's SAMPLE files (window.AuroraVault).
 *
 * SIMULATION ONLY. These are not AuroraFS files and nothing is saved. Vault reads AuroraSystem.files.list() and proposes
 * every change with AuroraSystem.request(): moving is reviewed (S2), deleting needs the trusted hold-to-confirm (S3).
 * Vault never writes AuroraSystem.world, never approves or runs anything, and keeps no history of its own: Undo and the
 * outcome come from the broker's receipts (the same ones Trail shows).
 * All text, including file names and previews, is inserted with textContent (AuroraUI.el).
 */
(function () {
  "use strict";
  const UI = window.AuroraUI, System = window.AuroraSystem, Shell = window.AuroraShell, M = window.AuroraVaultModel;
  if (!UI || !System || !Shell || !M) return;
  const { el, icon, clear } = UI;

  const screen = Shell.addScreen("screen-vault", "Vault");
  const ui = { view: "list", sort: "name", dir: "asc", space: "all", query: "", select: false, sel: new Set(), anchor: null, previewId: null, focusId: null, result: null, busy: false };
  const wide = window.matchMedia ? window.matchMedia("(min-width: 1024px)") : { matches: false, addEventListener() {} };
  const spaceName = (id) => System.spaceName(id);
  const decision = (id) => System.permissions.get(id);

  // ---------------- data (always read fresh from the system; Vault caches nothing) ----------------
  const allFiles = () => System.files.list();
  const byId = (id) => System.files.get(id);
  function visible() { return M.sortFiles(M.filterFiles(allFiles(), ui), ui.sort, ui.dir); }
  function pruneState(list) {
    const ids = new Set(list.map((f) => f.id));
    ui.sel.forEach((id) => { if (!ids.has(id)) ui.sel.delete(id); });
    if (ui.previewId && !ids.has(ui.previewId)) ui.previewId = null;
    if (ui.anchor && !ids.has(ui.anchor)) ui.anchor = null;
    if (!ui.sel.size && ui.select && ui.autoSelect) { ui.select = false; ui.autoSelect = false; }
  }

  // ---------------- requests: propose only; the trusted review decides ----------------
  const mine = () => ({ source: "Vault" });
  const sentence = (code) => System.explain(code);

  function pickDestination(files) {
    return UI.dialog({
      id: "vault-move-dialog", title: files.length === 1 ? "Move “" + files[0].name + "”" : "Move " + M.noun(files.length), icon: "move", dismissValue: null,
      body: el("div", { class: "review-body" }, [el("p", { text: "Choose a Space. You will review the change before anything moves." })]),
      actions: [{ label: "Cancel", kind: "outline", value: null, id: "btn-vault-move-cancel" }].concat(System.SPACES.filter((s) => files.some((f) => f.space !== s)).map((s) => ({ label: spaceName(s), kind: "accent", value: s, id: "btn-vault-dest-" + s }))),
    });
  }

  /** Run one or more reviewed batches of the same kind. Stops at the first cancel, block or failure. */
  async function runBatches(batches, build, verb, past, requestText, nameOf) {
    const proposalIds = [], texts = [];
    let ok = true, tone = "ok";
    for (let i = 0; i < batches.length; i++) {
      const ids = batches[i];
      const title = (batches.length > 1 ? "(" + (i + 1) + " of " + batches.length + ") " : "") + requestText;
      const r = await System.request(build(ids), { source: "Vault", requestText, title: ids.length > 1 || batches.length > 1 ? title : undefined });
      if (r && r.proposalId) proposalIds.push(r.proposalId);
      const s = M.summarize(verb, past, r, ids.map(nameOf), sentence);
      texts.push(s.text);
      if (!s.ok) { ok = false; tone = s.tone; if (i < batches.length - 1) texts.push((batches.length - 1 - i) + " more " + (batches.length - 1 - i === 1 ? "review was" : "reviews were") + " not started."); break; }
    }
    return { ok, tone, text: texts.join(" "), proposalIds };
  }

  async function moveFiles(ids) {
    if (ui.busy) return;
    const files = ids.map(byId).filter(Boolean);
    if (!files.length) { ui.result = { tone: "error", text: "Those files no longer exist.", proposalIds: [] }; render(); return; }
    ui.busy = true;
    try {
      const dest = await pickDestination(files);
      if (!dest) { ui.result = null; return; }
      const todo = M.movable(files, dest), skipped = files.length - todo.length;
      const names = Object.fromEntries(todo.map((f) => [f.id, f.name]));
      const batches = M.chunk(todo.map((f) => f.id), M.MAX_PLAN_STEPS);
      if (batches.length > 1) {
        const go = await UI.confirm({ id: "vault-batch-dialog", icon: "info", title: "Review " + batches.length + " times?", body: "This prototype reviews at most " + M.MAX_PLAN_STEPS + " changes at a time. Moving " + M.noun(todo.length) + " takes " + batches.length + " reviews (" + batches.map((b) => b.length).join(" + ") + "). You can cancel any of them.", confirmLabel: "Start", confirmId: "btn-vault-batch-ok", cancelLabel: "Cancel", cancelId: "btn-vault-batch-cancel" });
        if (!go) { ui.result = { tone: "info", text: "Cancelled. Nothing was changed.", proposalIds: [] }; return; }
      }
      const res = await runBatches(batches, (b) => M.movePayload(b, dest), "move", "Moved", "Move " + M.noun(todo.length) + " to " + spaceName(dest), (id) => names[id] || "file");
      ui.resultFresh = true;
      ui.result = Object.assign(res, { text: res.text + (skipped ? " " + M.noun(skipped) + " already in " + spaceName(dest) + " " + (skipped === 1 ? "was" : "were") + " skipped." : ""), label: "move" });
      if (res.ok) ui.sel.clear();
    } finally { ui.busy = false; render(); }
  }

  async function deleteFiles(ids) {
    if (ui.busy) return;
    const files = ids.map(byId).filter(Boolean);
    if (!files.length) { ui.result = { tone: "error", text: "Those files no longer exist.", proposalIds: [] }; render(); return; }
    ui.busy = true;
    try {
      const names = Object.fromEntries(files.map((f) => [f.id, f.name]));
      const batches = M.chunk(files.map((f) => f.id), M.MAX_PLAN_STEPS);
      if (batches.length > 1) {
        const go = await UI.confirm({ id: "vault-batch-dialog", icon: "info", title: "Review " + batches.length + " times?", body: "This prototype reviews at most " + M.MAX_PLAN_STEPS + " deletions at a time. Deleting " + M.noun(files.length) + " takes " + batches.length + " reviews (" + batches.map((b) => b.length).join(" + ") + "), each with its own hold. You can cancel any of them.", confirmLabel: "Start", confirmId: "btn-vault-batch-ok", cancelLabel: "Cancel", cancelId: "btn-vault-batch-cancel" });
        if (!go) { ui.result = { tone: "info", text: "Cancelled. Nothing was changed.", proposalIds: [] }; return; }
      }
      const res = await runBatches(batches, M.deletePayload, "delete", "Deleted", "Delete " + M.noun(files.length), (id) => names[id] || "file");
      ui.resultFresh = true;
      ui.result = Object.assign(res, { label: "delete" });
      if (res.ok) ui.sel.clear();
    } finally { ui.busy = false; render(); }
  }

  /** Receipts of the proposals Vault just made that can still be undone (the broker's word, read live). */
  function undoableCount() {
    const r = ui.result; if (!r || !r.proposalIds || !r.proposalIds.length) return 0;
    return System.receipts().filter((x) => r.proposalIds.indexOf(x.proposalId) >= 0 && x.status === "done" && x.undoState === "available").length;
  }
  async function undoResult() {
    const r = ui.result; if (!r || ui.busy) return;
    ui.busy = true; render();
    let okN = 0, bad = [];
    try {
      for (const pid of r.proposalIds.slice().reverse()) {
        const out = await System.undoProposal(pid);
        (out.steps || []).forEach((s) => { if (s.ok) okN++; else if (s.reason !== "not-undoable" && s.reason !== "already-undone") bad.push(sentence(s.reason)); });
        if (!out.steps && out.reason) bad.push(sentence(out.reason));
      }
    } finally { ui.busy = false; }
    ui.result = Object.assign({}, r, bad.length ? { tone: "error", text: (okN ? "Undid " + okN + (okN === 1 ? " step" : " steps") + ". " : "") + "Undo failed: " + bad[0] + (bad.length > 1 ? " (+" + (bad.length - 1) + " more)" : "") } : { tone: "ok", text: "Undone: " + (okN === 1 ? "1 step" : okN + " steps") + " reversed." });
    UI.announce(ui.result.text);
    render();
  }

  // ---------------- selection ----------------
  function toggle(id, on) { if (on === undefined ? ui.sel.has(id) : !on) ui.sel.delete(id); else ui.sel.add(id); }
  function pick(id, ev) {
    const vis = visible().map((f) => f.id);
    ui.focusId = id;
    if (ev && ev.shiftKey && ui.anchor) { M.rangeIds(vis, ui.anchor, id).forEach((x) => ui.sel.add(x)); ui.select = true; }
    else if ((ev && (ev.ctrlKey || ev.metaKey)) || ui.select) { toggle(id); ui.anchor = id; if (!ui.select) { ui.select = true; ui.autoSelect = true; } }
    else { ui.previewId = ui.previewId === id ? null : id; ui.anchor = id; }
    render();
  }
  function setSelectMode(on) { ui.select = on; ui.autoSelect = false; if (!on) ui.sel.clear(); render(); }
  function targetIds() { return ui.sel.size ? visible().map((f) => f.id).filter((id) => ui.sel.has(id)) : (ui.previewId ? [ui.previewId] : []); }

  // ---------------- rendering ----------------
  const DEC = { allow: ["Allowed", "ok", "check"], ask: ["Asks you", "info", "eye"], deny: ["Blocked", "error", "lock"] };
  const editDenied = () => decision("files.edit") === "deny", deleteDenied = () => decision("files.delete") === "deny";

  function fileItem(f, idx, tabbable) {
    const sel = ui.sel.has(f.id), open = ui.previewId === f.id;
    const check = el("button", { type: "button", class: "vault-check", role: "checkbox", "aria-checked": String(sel), "aria-label": "Select " + f.name, "data-fk": "check:" + f.id, tabindex: -1, onclick: (e) => { e.stopPropagation(); pick(f.id, Object.assign({ ctrlKey: !e.shiftKey }, { shiftKey: e.shiftKey })); } }, [icon(sel ? "check" : "plus")]);
    const main = el("button", { type: "button", class: "vault-open", "data-fk": "file:" + f.id, tabindex: tabbable ? 0 : -1, "aria-pressed": open ? "true" : undefined, onclick: (e) => pick(f.id, e) }, [
      el("span", { class: "vault-kind", "aria-hidden": "true" }, [icon(M.kindIcon(f.kind))]),
      el("span", { class: "vault-text" }, [
        el("span", { class: "vault-name", text: f.name }),
        el("span", { class: "vault-meta", text: M.kindLabel(f.kind) + " · " + spaceName(f.space) + " · " + M.formatSize(f.size) + " · " + UI.timeAgo(f.modified) }),
      ]),
    ]);
    return el("li", { class: "vault-item" + (sel ? " is-selected" : "") + (open ? " is-open" : ""), role: "listitem", dataset: { file: f.id, selected: String(sel) } }, [ui.select ? check : null, main]);
  }

  function previewBody(f) {
    const edit = decision("files.edit"), del = decision("files.delete");
    return [
      el("div", { class: "vault-preview-head" }, [
        el("span", { class: "vault-kind big", "aria-hidden": "true" }, [icon(M.kindIcon(f.kind))]),
        el("div", { class: "vault-preview-title" }, [el("h3", { class: "section-title", id: "vault-preview-name", text: f.name }), el("p", { class: "row-meta", text: M.kindLabel(f.kind) })]),
      ]),
      el("dl", { class: "trail-facts vault-facts" }, [].concat(...[["Space", spaceName(f.space)], ["Size", M.formatSize(f.size)], ["Modified", new Date(f.modified).toLocaleString() + " (" + UI.timeAgo(f.modified) + ")"]].map(([k, v]) => [el("dt", { text: k }), el("dd", { text: v })]))),
      f.kind === "image" || typeof f.preview !== "string"
        ? el("div", { class: "vault-placeholder", role: "img", "aria-label": f.kind === "image" ? "Image preview not available" : "No preview available" }, [icon(f.kind === "image" ? "image" : "file"), el("span", { text: f.kind === "image" ? "Image preview isn’t available: the sample data has no pixels." : "No text preview for this file." })])
        : el("pre", { class: "vault-text-preview", tabindex: 0, "aria-label": "Text preview", text: f.preview }),
      el("div", { class: "vault-perms" }, [
        el("p", { class: "eyebrow", text: "Passport decisions" }),
        el("div", { class: "vault-perm-row" }, [el("span", { text: "Move files" }), UI.chip(...DEC[edit])]),
        el("div", { class: "vault-perm-row" }, [el("span", { text: "Delete files" }), UI.chip(...DEC[del])]),
        el("button", { type: "button", class: "btn-link", id: "btn-vault-passport", "data-fk": "passport", onclick: () => Shell.openApp("passport") }, [el("span", { text: "Open Passport" })]),
      ]),
      el("div", { class: "action-bar" }, [
        el("button", { type: "button", class: "btn outline btn-small", id: "btn-vault-preview-move", "data-fk": "pmove", disabled: editDenied() || ui.busy ? true : undefined, onclick: () => moveFiles([f.id]) }, [icon("move"), el("span", { text: "Move" })]),
        el("button", { type: "button", class: "btn ghost btn-small", id: "btn-vault-preview-close", "data-fk": "pclose", "aria-label": "Close preview", onclick: () => { ui.previewId = null; render(); } }, [icon("close"), el("span", { text: "Close" })]),
        el("button", { type: "button", class: "btn outline btn-small", id: "btn-vault-preview-delete", "data-fk": "pdelete", disabled: deleteDenied() || ui.busy ? true : undefined, onclick: () => deleteFiles([f.id]) }, [icon("trash"), el("span", { text: "Delete" })]),
      ]),
    ];
  }
  function previewCard() {
    const f = ui.previewId && byId(ui.previewId);
    return f
      ? el("section", { class: "card opaque vault-preview", id: "vault-preview", "aria-labelledby": "vault-preview-name" }, previewBody(f))
      : (wide.matches ? el("section", { class: "card opaque vault-preview", id: "vault-preview", "aria-label": "Preview" }, [UI.emptyState("files", "Select a file", "Its details and a preview appear here.")]) : null);
  }

  function toolbar(total, shown) {
    const search = el("div", { class: "search-field" }, [icon("search"), el("input", { type: "search", class: "field", id: "vault-search", "data-fk": "search", placeholder: "Search files", "aria-label": "Search files", autocomplete: "off", value: ui.query, oninput: (e) => { ui.query = e.target.value; render(); } })]);
    const spaces = el("div", { class: "trail-chips", role: "group", "aria-label": "Filter by Space" }, [["all", "All"]].concat(System.SPACES.map((s) => [s, spaceName(s)])).map(([v, label]) =>
      el("button", { type: "button", class: "trail-filter", "data-fk": "vspace:" + v, "aria-pressed": String(ui.space === v), text: label, onclick: () => { ui.space = v; render(); } })));
    const sort = el("div", { class: "vault-sort" }, [
      el("div", { class: "control-group" }, [el("label", { for: "vault-sort", text: "Sort by" }), el("select", { id: "vault-sort", "data-fk": "sort", onchange: (e) => { ui.sort = e.target.value; render(); } }, M.SORTS.map(([v, l]) => el("option", { value: v, text: l, selected: ui.sort === v ? true : undefined })))]),
      el("button", { type: "button", class: "btn outline btn-small", id: "btn-vault-dir", "data-fk": "dir", "aria-label": "Order: " + (ui.dir === "asc" ? "ascending" : "descending") + ". Switch.", onclick: () => { ui.dir = ui.dir === "asc" ? "desc" : "asc"; render(); } }, [icon("sort"), el("span", { text: ui.dir === "asc" ? "Ascending" : "Descending" })]),
    ]);
    const view = UI.segmented("View", [{ value: "list", label: "List" }, { value: "grid", label: "Grid" }], ui.view, (v) => { ui.view = v; render(); return true; });
    view.querySelectorAll("button").forEach((b) => b.setAttribute("data-fk", "view:" + b.dataset.value));
    const selectBtn = el("button", { type: "button", class: "btn outline btn-small", id: "btn-vault-select", "data-fk": "select", "aria-pressed": String(ui.select), onclick: () => setSelectMode(!ui.select) }, [icon("check"), el("span", { text: ui.select ? "Done selecting" : "Select" })]);
    return el("div", { class: "vault-toolbar" }, [search, spaces, el("div", { class: "vault-row2" }, [sort, view, selectBtn]), el("p", { class: "row-meta", id: "vault-count", role: "status", text: shown === total ? M.noun(total) : "Showing " + shown + " of " + M.noun(total) })]);
  }

  function selectionBar(vis) {
    if (!ui.sel.size) return null;
    const n = ui.sel.size;
    const noEdit = editDenied() || ui.busy, noDel = deleteDenied() || ui.busy;
    return el("div", { class: "card opaque vault-selbar", id: "vault-selbar", role: "region", "aria-label": "Selection" }, [
      el("span", { class: "vault-selcount", id: "vault-selcount", text: n + " selected" }),
      el("div", { class: "action-bar" }, [
        el("button", { type: "button", class: "btn outline btn-small", id: "btn-vault-move", "data-fk": "move", disabled: noEdit ? true : undefined, "aria-describedby": editDenied() ? "vault-edit-denied" : undefined, onclick: () => moveFiles(targetIds()) }, [icon("move"), el("span", { text: "Move" })]),
        el("button", { type: "button", class: "btn danger btn-small", id: "btn-vault-delete", "data-fk": "delete", disabled: noDel ? true : undefined, "aria-describedby": deleteDenied() ? "vault-delete-denied" : undefined, onclick: () => deleteFiles(targetIds()) }, [icon("trash"), el("span", { text: "Delete" })]),
        el("button", { type: "button", class: "btn outline btn-small", id: "btn-vault-all", "data-fk": "all", onclick: () => { vis.forEach((f) => ui.sel.add(f.id)); render(); } }, [el("span", { text: "Select all" })]),
        el("button", { type: "button", class: "btn ghost btn-small", id: "btn-vault-clear", "data-fk": "clearsel", onclick: () => { ui.sel.clear(); render(); } }, [el("span", { text: "Clear" })]),
      ]),
    ]);
  }

  function notices() {
    const out = [];
    out.push(el("p", { class: "callout", id: "vault-banner" }, [icon("info"), el("span", { text: "Sample files in this browser prototype. Not AuroraFS, nothing is saved." })]));
    if (editDenied()) out.push(el("p", { class: "callout callout-warn", id: "vault-edit-denied", role: "status" }, [icon("lock"), el("span", { text: "Passport blocks moving files, so Move is off. " }), el("button", { type: "button", class: "btn-link", "data-fk": "passport-edit", onclick: () => Shell.openApp("passport") }, [el("span", { text: "Open Passport" })])]));
    if (deleteDenied()) out.push(el("p", { class: "callout callout-warn", id: "vault-delete-denied", role: "status" }, [icon("lock"), el("span", { text: "Passport blocks deleting files, so Delete is off. " }), el("button", { type: "button", class: "btn-link", "data-fk": "passport-del", onclick: () => Shell.openApp("passport") }, [el("span", { text: "Open Passport" })])]));
    if (ui.result) {
      const r = ui.result, n = undoableCount();
      out.push(el("div", { class: "callout vault-result" + (r.tone === "error" ? " callout-error" : r.tone === "ok" ? " callout-ok" : ""), id: "vault-result", role: "status" }, [
        icon(r.tone === "error" ? "alert" : r.tone === "ok" ? "check" : "info"), el("span", { class: "vault-result-text", text: r.text }),
        el("span", { class: "vault-result-actions" }, [
          n > 0 ? el("button", { type: "button", class: "btn outline btn-small", id: "btn-vault-undo", "data-fk": "undo", disabled: ui.busy ? true : undefined, onclick: undoResult }, [icon("undo"), el("span", { text: ui.busy ? "Undoing…" : "Undo" })]) : null,
          r.proposalIds && r.proposalIds.length ? el("button", { type: "button", class: "btn outline btn-small", id: "btn-vault-trail", "data-fk": "trail", onclick: () => Shell.openApp("trail") }, [icon("trail"), el("span", { text: "View in Trail" })]) : null,
          el("button", { type: "button", class: "btn btn-icon ghost", "aria-label": "Dismiss", title: "Dismiss", "data-fk": "dismiss", onclick: () => { ui.result = null; render(); } }, [icon("close")]),
        ]),
      ]));
    }
    return out;
  }

  function onListKey(e) {
    const t = e.target;
    if (t.matches && t.matches("input, select, textarea")) return;
    const items = [...screen.querySelectorAll(".vault-open")];
    const i = items.indexOf(t.closest(".vault-open") || document.activeElement);
    const idOf = (btn) => btn && btn.getAttribute("data-fk").slice(5);
    const go = (j) => { if (j >= 0 && j < items.length) { e.preventDefault(); items.forEach((b) => { b.tabIndex = -1; }); items[j].tabIndex = 0; items[j].focus(); ui.focusId = idOf(items[j]); } };
    if (e.key === "ArrowDown" || e.key === "ArrowRight") go(i + 1);
    else if (e.key === "ArrowUp" || e.key === "ArrowLeft") go(i - 1);
    else if (e.key === "Home") go(0);
    else if (e.key === "End") go(items.length - 1);
    else if (e.key === " " && i >= 0) { e.preventDefault(); const id = idOf(items[i]); toggle(id); ui.anchor = id; ui.focusId = id; if (!ui.select) { ui.select = true; ui.autoSelect = true; } render(); }
    else if ((e.key === "a" || e.key === "A") && (e.ctrlKey || e.metaKey)) { e.preventDefault(); visible().forEach((f) => ui.sel.add(f.id)); ui.select = true; render(); }
    else if (e.key === "Delete" && !deleteDenied()) { const ids = ui.sel.size ? targetIds() : (i >= 0 ? [idOf(items[i])] : []); if (ids.length) { e.preventDefault(); deleteFiles(ids); } }
    else if (e.key === "Escape" && (ui.sel.size || ui.previewId)) { e.preventDefault(); if (ui.sel.size) ui.sel.clear(); else ui.previewId = null; render(); }
  }

  function render() {
    UI.keepFocus(screen, () => {
      clear(screen);
      const all = allFiles(), vis = visible();
      pruneState(all);
      screen.appendChild(el("header", { class: "screen-header" }, [
        el("button", { type: "button", class: "btn btn-icon ghost", id: "btn-vault-back", "aria-label": "Back", title: "Back", onclick: () => Shell.back() }, [icon("back")]),
        el("div", { class: "screen-heading" }, [el("p", { class: "eyebrow", text: "Sample files" }), el("h1", { class: "screen-title", text: "Vault" })]),
      ]));
      notices().forEach((n) => screen.appendChild(n));
      screen.appendChild(toolbar(M.filterFiles(all, { space: ui.space }).length, vis.length));
      const bar = selectionBar(vis); if (bar) screen.appendChild(bar);

      let list;
      if (!all.length) list = UI.emptyState("files", "No files", "Everything was deleted. Restore files from Trail while this page stays open.", el("button", { type: "button", class: "btn outline", id: "btn-vault-to-trail", onclick: () => Shell.openApp("trail") }, [icon("trail"), el("span", { text: "Open Trail" })]));
      else if (!vis.length) list = el("div", {}, [UI.emptyState("search", "No files match", "Try another search or Space."), el("div", { class: "action-bar" }, [el("button", { type: "button", class: "btn outline", id: "btn-vault-clear-filters", "data-fk": "clearf", onclick: () => { ui.query = ""; ui.space = "all"; render(); } }, [el("span", { text: "Clear filters" })])])]);
      else {
        const tab = vis.some((f) => f.id === ui.focusId) ? ui.focusId : vis[0].id;
        const ul = el("ul", { class: "vault-" + ui.view, role: "list", "aria-label": "Files" }, vis.map((f, i) => fileItem(f, i, f.id === tab)));
        ul.addEventListener("keydown", onListKey);
        list = ul;
      }
      const prev = previewCard();
      const main = el("div", { class: "vault-main" }, [wide.matches ? null : prev, list]);
      screen.appendChild(el("div", { class: "trail-layout vault-layout" + (wide.matches ? " is-wide" : "") }, wide.matches ? [main, prev] : [main]));
    }, "#vault-search");
    if (ui.resultFresh) { ui.resultFresh = false; const r = document.getElementById("vault-result"); if (r && r.scrollIntoView) r.scrollIntoView({ block: "center" }); }   // a result must not stay off-screen after a move or delete
    const si = document.activeElement;
    if (si && si.id === "vault-search") { const n = si.value.length; si.setSelectionRange(n, n); }
  }

  /** Select and preview a file (used by Index). Returns false for an unknown id and changes nothing. */
  function preview(id) {
    if (typeof id !== "string" || !byId(id)) return false;
    ui.query = ""; ui.space = "all"; ui.previewId = id; ui.focusId = id; ui.anchor = id;
    render();
    const b = screen.querySelector('[data-fk="file:' + CSS.escape(id) + '"]');
    // preventScroll: a plain focus() could scroll the page so far that the toolbar slides under the Glass Corner.
    // Narrow layouts show the preview above the list, so bring that card into view; wide ones keep it in a side panel.
    if (b) { b.focus({ preventScroll: true }); const target = wide.matches ? b : screen.querySelector("#vault-preview"); if (target && target.scrollIntoView) target.scrollIntoView({ block: "nearest" }); }
    return true;
  }

  const live = () => screen.classList.contains("active");
  ["files", "receipts", "permission"].forEach((evt) => System.on(evt, () => { if (live() && !ui.busy) render(); }));
  if (wide.addEventListener) wide.addEventListener("change", () => { if (live()) render(); });
  Shell.onScreenShow((id) => { if (id === "screen-vault") render(); });
  render();

  window.AuroraVault = { preview, selection: () => Array.from(ui.sel), previewed: () => ui.previewId, state: () => ({ view: ui.view, sort: ui.sort, dir: ui.dir, space: ui.space, query: ui.query, select: ui.select }) };
})();
