/* aurora-glass/prototype/trail.js — Trail: what Aura and apps did, and how to undo it (window.AuroraTrail).
 *
 * SIMULATION ONLY. Trail has no history of its own. Every row is a receipt the prototype Action Broker recorded
 * (AuroraSystem.receipts()), grouped by the proposal that caused it (AuroraSystem.proposer.getProposal()).
 * Undo and Restore call AuroraSystem.undo / undoProposal and show the broker's real answer: nothing is claimed before
 * the call returns, and a failed undo stays failed. Trail never touches AuroraSystem.world.
 * History is for this page load only; reloading clears it.
 * All text is inserted with textContent (AuroraUI.el).
 */
(function () {
  "use strict";
  const UI = window.AuroraUI, System = window.AuroraSystem, Shell = window.AuroraShell, M = window.AuroraTrailModel;
  if (!UI || !System || !Shell || !M) return;
  const { el, icon, clear } = UI;

  const screen = Shell.addScreen("screen-trail", "Trail");
  const ui = { query: "", status: "all", risk: "all", selected: null, busy: null, notice: null, error: null, undoNote: Object.create(null) };
  const wide = window.matchMedia ? window.matchMedia("(min-width: 1024px)") : { matches: false, addEventListener() {} };

  const proposalOf = (id) => (System.proposer && System.proposer.getProposal ? System.proposer.getProposal(id) : null);
  function load() {
    try { ui.error = null; return System.receipts(); }
    catch (e) { ui.error = "Trail could not read the history."; return []; }
  }

  // ---------------- undo / restore: always the broker's real answer ----------------
  const isBusy = () => ui.busy !== null;
  function say(text, tone) { ui.notice = { text, tone: tone || "info" }; UI.announce(text); }

  async function undoReceipt(r) {
    if (isBusy()) return;
    ui.busy = { kind: "receipt", id: r.id }; ui.notice = null; render();
    let res;
    try { res = await System.undo(r.id); } catch (e) { res = { ok: false, reason: "undo-failed", text: System.explain("undo-failed") }; }
    ui.busy = null;
    if (res.ok) say("Undone: " + r.describe.title, "ok");
    else { ui.undoNote[r.id] = res.text || System.explain(res.reason); say("Not undone: " + ui.undoNote[r.id], "error"); }
    render(); focusRow(r.id);
  }

  async function undoAll(g) {
    if (isBusy() || !g.proposalId) return;
    ui.busy = { kind: "group", id: g.proposalId }; ui.notice = null; render();
    let res;
    try { res = await System.undoProposal(g.proposalId); } catch (e) { res = { ok: false, steps: [] }; }
    ui.busy = null;
    const steps = res.steps || [];
    const okN = steps.filter((s) => s.ok).length, bad = steps.filter((s) => !s.ok);
    steps.forEach((s) => { if (!s.ok && s.receiptId) ui.undoNote[s.receiptId] = System.explain(s.reason); });
    if (!steps.length) say("Nothing could be undone: " + System.explain(res.reason || "nothing-to-undo"), "error");
    else if (!bad.length) say("Undid " + okN + (okN === 1 ? " step" : " steps"), "ok");
    else say("Undid " + okN + " of " + steps.length + " steps. " + bad.length + (bad.length === 1 ? " step" : " steps") + " could not be undone.", "error");
    render();
  }

  async function restore(file, receipt) {
    if (isBusy()) return;
    ui.busy = { kind: "restore", id: file.id }; ui.notice = null; render();
    let res;
    try { res = await System.undo(receipt.id); } catch (e) { res = { ok: false, reason: "undo-failed" }; }
    ui.busy = null;
    if (res.ok) say("Restored “" + file.name + "”", "ok");
    else say("Could not restore “" + file.name + "”: " + (res.text || System.explain(res.reason)), "error");
    render();
  }

  function focusRow(id) { const b = screen.querySelector('[data-fk="row:' + CSS.escape(id) + '"]'); if (b) b.focus(); }

  // ---------------- rendering ----------------
  function statusChip(r) { const [t, tone, ic] = M.statusInfo(r.status); return UI.chip(t, tone, ic); }

  function undoButton(r, withLabel) {
    const running = ui.busy && ui.busy.kind === "receipt" && ui.busy.id === r.id;
    return el("button", { type: "button", class: "btn outline btn-small trail-undo", "data-fk": "undo:" + r.id, disabled: isBusy() ? true : undefined, "aria-busy": running ? "true" : undefined,
      "aria-label": (running ? "Undoing " : "Undo ") + r.describe.title, onclick: () => undoReceipt(r) }, [icon("undo"), el("span", { text: running ? "Undoing…" : withLabel || "Undo" })]);
  }

  function detailBody(r, g) {
    const u = M.undoInfo(r);
    const when = new Date(r.at);
    const why = u.state === "failed" && ui.undoNote[r.id] ? "Undo failed: " + ui.undoNote[r.id] : u.why;
    const rows = [
      ["What", r.describe.title], ["Source", r.source], ["When", when.toLocaleString() + " (" + UI.timeAgo(r.at) + ")"],
      ["Risk", UI.RISK_TEXT[r.risk] ? r.risk + " · " + UI.RISK_TEXT[r.risk] : "Unknown"], ["Result", M.statusInfo(r.status)[0]],
    ];
    if (g.requestText) rows.splice(1, 0, ["Asked", g.requestText]);
    return el("div", { class: "trail-detail-body" }, [
      el("dl", { class: "trail-facts" }, [].concat(...rows.map(([k, v]) => [el("dt", { text: k }), el("dd", { text: v })]))),
      r.errorText ? el("p", { class: "callout callout-error trail-why" }, [icon("alert"), el("span", { text: r.errorText })]) : null,
      el("p", { class: "trail-why" + (u.state === "failed" ? " is-failed" : ""), "data-undo-state": u.state }, [icon(u.state === "used" ? "check" : u.state === "available" ? "undo" : "info"), el("span", { text: why })]),
      u.state === "available" ? el("div", { class: "action-bar" }, [undoButton(r)]) : null,
    ]);
  }

  function receiptRow(r, g) {
    const selected = ui.selected === r.id, u = M.undoInfo(r);
    const head = el("button", { type: "button", class: "trail-row-main", "data-fk": "row:" + r.id, "aria-expanded": String(selected), "aria-controls": wide.matches ? "trail-detail" : "trail-d-" + r.id,
      onclick: () => { ui.selected = selected ? null : r.id; render(); focusRow(r.id); } }, [
      el("span", { class: "trail-row-icon", "aria-hidden": "true" }, [icon(r.describe.icon && UI.ICONS.includes(r.describe.icon) ? r.describe.icon : "info")]),
      el("span", { class: "row-main" }, [
        el("span", { class: "row-title", text: r.describe.title }),
        el("span", { class: "row-meta", text: r.source + " · " + UI.timeAgo(r.at) }),
        el("span", { class: "trail-row-badges" }, [statusChip(r), UI.riskBadge(r.risk, true), r.status === "done" ? UI.chip(u.label, u.state === "available" ? "info" : "muted", u.state === "used" ? "undo" : u.state === "available" ? "undo" : "info") : null]),
      ]),
    ]);
    return el("li", { class: "trail-row" + (selected ? " is-selected" : ""), dataset: { receipt: r.id, status: r.status, undo: r.undoState } }, [
      el("div", { class: "trail-row-line" }, [head, u.state === "available" ? undoButton(r) : null]),
      selected && !wide.matches ? el("div", { class: "trail-inline", id: "trail-d-" + r.id }, [detailBody(r, g)]) : null,
    ]);
  }

  function groupCard(g) {
    const [label, tone, ic] = g.outcome;
    const busyGroup = ui.busy && ui.busy.kind === "group" && ui.busy.id === g.proposalId;
    const title = g.requestText || (g.kind === "plan" ? "Plan with " + g.totalSteps + " steps" : g.receipts[0].describe.title);
    const partial = g.matched !== undefined && g.matched < g.total;
    return el("article", { class: "card opaque trail-group", dataset: { group: g.id, kind: g.kind, outcome: label }, "aria-label": title }, [
      g.kind === "plan" || g.requestText ? el("header", { class: "trail-group-head" }, [
        el("div", { class: "trail-group-text" }, [
          el("p", { class: "eyebrow", text: g.kind === "plan" ? "Plan · " + g.source : g.source }),
          el("h3", { class: "section-title", text: title }),
          g.kind === "plan" ? el("p", { class: "row-meta", text: g.receipts.length + " of " + g.totalSteps + (g.totalSteps === 1 ? " step" : " steps") + " ran" + (g.notRun ? " · " + g.notRun + " did not run" : "") + (partial ? " · showing " + g.matched + " matching" : "") }) : null,
        ]),
        UI.chip(label, tone, ic),
      ]) : null,
      el("ul", { class: "trail-rows", "aria-label": title + " steps" }, g.receipts.map((r) => receiptRow(r, g))),
      g.kind === "plan" && g.undoable > 0 && g.proposalId ? el("div", { class: "action-bar" }, [
        el("button", { type: "button", class: "btn outline btn-small", "data-fk": "undoall:" + g.id, disabled: isBusy() ? true : undefined, "aria-busy": busyGroup ? "true" : undefined, onclick: () => undoAll(g) }, [icon("undo"), el("span", { text: busyGroup ? "Undoing…" : "Undo all (" + g.undoable + ")" })]),
      ]) : null,
    ]);
  }

  function filters(all) {
    const search = el("div", { class: "search-field" }, [icon("search"),
      el("input", { type: "search", class: "field", id: "trail-search", "data-fk": "search", placeholder: "Search history", "aria-label": "Search history", autocomplete: "off", value: ui.query,
        oninput: (e) => { ui.query = e.target.value; render(); } }),
    ]);
    const chips = el("div", { class: "trail-chips", role: "group", "aria-label": "Filter by status" }, M.STATUS_FILTERS.map(([v, label]) =>
      el("button", { type: "button", class: "trail-filter", "data-fk": "status:" + v, "aria-pressed": String(ui.status === v), text: label, onclick: () => { ui.status = v; render(); } })));
    const risk = el("div", { class: "control-group trail-risk" }, [el("label", { for: "trail-risk", text: "Risk" }),
      el("select", { id: "trail-risk", "data-fk": "risk", onchange: (e) => { ui.risk = e.target.value; render(); } }, M.RISK_FILTERS.map(([v, label]) => el("option", { value: v, text: label, selected: ui.risk === v ? true : undefined })))]);
    return el("div", { class: "trail-filters" }, [search, chips, risk]);
  }

  function deletedSection(receipts) {
    const files = System.files.deleted();
    const rows = files.map((f) => {
      const rr = M.restoreReceipt(f.id, receipts), running = ui.busy && ui.busy.kind === "restore" && ui.busy.id === f.id;
      return el("li", { class: "list-row trail-deleted", dataset: { deletedFile: f.id } }, [
        el("div", { class: "row-main" }, [el("span", { class: "row-title", text: f.name }), el("span", { class: "row-meta", text: "Deleted " + UI.timeAgo(f.deletedAt) + (rr ? "" : " · " + M.restoreWhy(f.id, receipts)) })]),
        rr ? el("button", { type: "button", class: "btn outline btn-small", "data-fk": "restore:" + f.id, disabled: isBusy() ? true : undefined, "aria-busy": running ? "true" : undefined, "aria-label": (running ? "Restoring " : "Restore ") + f.name, onclick: () => restore(f, rr) }, [icon("undo"), el("span", { text: running ? "Restoring…" : "Restore" })]) : null,
      ]);
    });
    return el("section", { class: "card opaque", "aria-labelledby": "trail-deleted-h" }, [
      el("h3", { class: "section-title", id: "trail-deleted-h", text: "Deleted files" }),
      rows.length ? el("ul", { class: "trail-deleted-list", "aria-label": "Deleted files" }, rows) : el("p", { class: "row-meta trail-none", text: "No deleted files. Deleting a file keeps it here so it can be restored during this session." }),
    ]);
  }

  function detailPanel(all) {
    const r = ui.selected && all.find((x) => x.id === ui.selected);
    const g = r && M.group(all, proposalOf).find((x) => x.receipts.some((y) => y.id === r.id));
    return el("aside", { class: "card opaque trail-panel", id: "trail-detail", "aria-label": "Action details" }, r && g
      ? [el("h3", { class: "section-title", text: r.describe.title }), detailBody(r, g)]
      : [UI.emptyState("trail", "Select an action", "Details and Undo appear here.")]);
  }

  function render() {
    UI.keepFocus(screen, () => {
      clear(screen);
      const all = load();
      const groups = M.group(all, proposalOf);
      const shown = M.filter(groups, ui);
      // A detail that the current filters hide is not kept selected: the panel must never show a row that is not listed.
      if (ui.selected && !shown.some((g) => g.receipts.some((r) => r.id === ui.selected))) ui.selected = null;

      screen.appendChild(el("header", { class: "screen-header" }, [
        el("button", { type: "button", class: "btn btn-icon ghost", id: "btn-trail-back", "aria-label": "Back", title: "Back", onclick: () => Shell.back() }, [icon("back")]),
        el("div", { class: "screen-heading" }, [el("p", { class: "eyebrow", text: "History and recovery" }), el("h1", { class: "screen-title", text: "Trail" })]),
      ]));
      if (ui.error) screen.appendChild(el("p", { class: "callout callout-error", id: "trail-error", role: "alert" }, [icon("alert"), el("span", { text: ui.error })]));
      if (ui.notice) screen.appendChild(el("p", { class: "callout trail-notice" + (ui.notice.tone === "error" ? " callout-error" : ""), id: "trail-notice", role: "status" }, [icon(ui.notice.tone === "error" ? "alert" : "check"), el("span", { text: ui.notice.text })]));
      screen.appendChild(filters(all));

      let list;
      if (!all.length) list = UI.emptyState("trail", "Nothing has happened yet", "When Aura or an app does something, it shows up here with a way to undo it.");
      else if (!shown.length) list = el("div", {}, [UI.emptyState("search", "Nothing matches", "No action matches these filters."),
        el("div", { class: "action-bar" }, [el("button", { type: "button", class: "btn outline", id: "btn-trail-clear", "data-fk": "clear", onclick: () => { ui.query = ""; ui.status = "all"; ui.risk = "all"; render(); } }, [el("span", { text: "Clear filters" })])])]);
      else list = el("div", { class: "trail-days" }, M.byDay(shown).map((d) => el("section", { class: "trail-day", "aria-label": d.label }, [el("h2", { class: "trail-day-h", text: d.label })].concat(d.groups.map(groupCard)))));

      const main = el("div", { class: "trail-main" }, [list, deletedSection(all)]);
      screen.appendChild(el("div", { class: "trail-layout" + (wide.matches ? " is-wide" : "") }, wide.matches ? [main, detailPanel(all)] : [main]));
      screen.appendChild(el("p", { class: "callout trail-footer", id: "trail-footer" }, [icon("info"), el("span", { text: "This history lasts only until you reload or close this page. The “Keep Trail history” setting is saved but does not keep anything yet." })]));
    }, "#btn-trail-back");
    const si = document.activeElement;
    if (si && si.id === "trail-search") { const n = si.value.length; si.setSelectionRange(n, n); }   // re-rendering must not move the caret
  }

  const live = () => screen.classList.contains("active");
  System.on("receipts", () => { if (live() && !isBusy()) render(); });
  System.on("files", () => { if (live() && !isBusy()) render(); });
  if (wide.addEventListener) wide.addEventListener("change", () => { if (live()) render(); });
  Shell.onScreenShow((id) => { if (id === "screen-trail") render(); });
  render();

  window.AuroraTrail = { filters: () => ({ query: ui.query, status: ui.status, risk: ui.risk }), selected: () => ui.selected };
})();
