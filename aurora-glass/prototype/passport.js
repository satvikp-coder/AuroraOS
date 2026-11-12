/* aurora-glass/prototype/passport.js — Passport: see and change what Aura and apps may do (window.AuroraPassport).
 *
 * SIMULATION ONLY. This screen changes the REAL prototype permission state, and only through
 * AuroraSystem.permissions.set() and .resetAll(): the same policy that backs the review dialog (S3 permissions can never
 * be Allow, unknown ids are refused, every change is logged). It cannot approve or run anything.
 *
 * RULES
 *  - Tightening (to Ask or Deny) applies at once with an Undo toast. Moving TO Allow needs a confirmation that says what
 *    will then run without a review. Controls always show the STORED decision after the call, never an optimistic one.
 *  - The broker decides per capability, not per app. The Apps tab says so and "Revoke all" says which other apps it affects.
 *  - All text is inserted with textContent (AuroraUI.el).
 */
(function () {
  "use strict";
  const UI = window.AuroraUI, System = window.AuroraSystem, Shell = window.AuroraShell, M = window.AuroraPassportModel;
  if (!UI || !System || !Shell || !M) return;
  const { el, icon, clear } = UI;

  const screen = Shell.addScreen("screen-passport", "Passport");
  const ui = { tab: "overview" };
  const TABS = [["overview", "Overview"], ["apps", "Apps"], ["activity", "Activity"]];
  const W = M.DECISION_WORD;
  const DEC_CHIP = { allow: ["Allowed", "ok", "check"], ask: ["Asks you", "info", "eye"], deny: ["Blocked", "error", "lock"] };
  const REASON = { "always-ask": "This permission always asks you. It cannot be pre-approved.", "unknown-permission": "That permission does not exist.", "invalid-decision": "That is not a valid choice.", refused: "That change was refused." };
  const RECEIPT_CHIP = { denied: ["Blocked", "error", "lock"], "needs-reapproval": ["Needs review again", "warn", "eye"] };

  const nameOf = (appId) => { const a = M.appById(appId); return a ? a.name : appId; };
  const catalog = () => System.permissions.catalog();

  // ---------------- decisions ----------------
  function loosenBody(c) {
    const users = M.appsUsing(c.id).map(nameOf);
    return el("div", { class: "review-body" }, [
      // Only S0 actions skip the review when allowed. Changes (S1), file edits (S2) and destructive ones (S3) are always reviewed.
      el("p", { id: "passport-loosen-text", text: c.risk === "S0" ? "If you allow this, Aura and apps can " + M.consequence(c.id) + " as soon as they ask, without showing you a review first." : "You will still review every change of this kind before it runs, whether this is Allow or Ask. Allow records that you trust it; it does not skip the review." }),
      users.length ? el("p", { class: "perm-affects", text: "This affects every app that uses it: " + users.join(", ") + "." }) : null,
      el("p", { class: "review-note", text: "You can change it back at any time. Every change is recorded in Activity." }),
    ]);
  }

  async function change(c, to) {
    const from = System.permissions.get(c.id);
    const kind = M.classify(from, to);
    if (kind === "same") { render(); return; }
    if (M.needsConfirm(from, to)) {
      const ok = await UI.confirm({ id: "passport-confirm", icon: "alert", title: c.risk === "S0" ? "Allow “" + c.label + "” without asking?" : "Allow “" + c.label + "”?", body: loosenBody(c), confirmLabel: c.risk === "S0" ? "Allow without asking" : "Allow", confirmId: "btn-passport-confirm", cancelLabel: "Keep asking", cancelId: "btn-passport-cancel" });
      if (!ok) { render(); UI.announce("Nothing was changed."); return; }
    }
    const r = System.permissions.set(c.id, to);
    if (!r.ok) { UI.toast(REASON[r.reason] || "That change was not allowed.", { tone: "error" }); render(); return; }
    const hadFocus = !!(document.activeElement && document.activeElement.closest && document.activeElement.closest('[id="perm-' + c.id + '"]'));
    render();
    if (hadFocus) { const b = document.querySelector('[data-fk="seg:' + c.id + ":" + to + '"]'); if (b) b.focus(); }
    const text = c.label + ": " + W[from] + " → " + W[to];
    if (kind === "tighten") {
      UI.toast(text, { action: { label: "Undo", run: () => { const u = System.permissions.set(c.id, from); if (!u.ok) UI.toast(REASON[u.reason] || "Could not undo.", { tone: "error" }); render(); } } });
    } else UI.toast(text);
    UI.announce(text);
  }

  async function revokeAll(appId) {
    const app = M.appById(appId), cat = catalog(), targets = M.revokeTargets(appId, cat);
    if (!app || !targets.length) return;
    const shared = M.sharedWith(appId, targets);
    const otherApps = Array.from(new Set([].concat(...Object.values(shared)))).map(nameOf);
    const label = (id) => cat.find((c) => c.id === id).label;
    const ok = await UI.confirm({ id: "passport-revoke-confirm", icon: "lock", title: "Revoke all for " + app.name + "?",
      body: el("div", { class: "review-body" }, [
        el("p", { text: "These will be set to Deny, so they stop working until you allow them again:" }),
        el("ul", { class: "confirm-list" }, targets.map((id) => el("li", { text: label(id) }))),
        el("p", { class: "perm-affects", text: otherApps.length ? "Permissions belong to a capability, not to one app. This also blocks them for: " + otherApps.join(", ") + "." : "No other app uses these capabilities." }),
      ]), confirmLabel: "Revoke", confirmId: "btn-passport-revoke-ok", cancelLabel: "Cancel", cancelId: "btn-passport-revoke-cancel" });
    if (!ok) return;
    const before = {}; targets.forEach((id) => { before[id] = System.permissions.get(id); });
    let done = 0;
    targets.forEach((id) => { if (System.permissions.set(id, "deny").ok) done++; });
    render();
    UI.toast("Revoked " + done + (done === 1 ? " permission" : " permissions") + " for " + app.name, { action: { label: "Undo", run: () => { Object.keys(before).forEach((id) => System.permissions.set(id, before[id])); render(); } } });
  }

  async function resetDefaults() {
    const ok = await UI.confirm({ id: "passport-reset-confirm", icon: "undo", title: "Reset permissions to defaults?", body: "Every permission returns to its default choice. Each change is recorded in Activity.", confirmLabel: "Reset", confirmId: "btn-passport-reset-ok", cancelLabel: "Cancel", cancelId: "btn-passport-reset-cancel" });
    if (!ok) return;
    System.permissions.resetAll();
    render();
    UI.toast("Permissions reset to defaults");
  }

  // ---------------- tabs ----------------
  function setTab(id) { ui.tab = id; render(); }
  function tablist() {
    const list = el("div", { class: "tabs", role: "tablist", "aria-label": "Passport sections" }, TABS.map(([id, label]) =>
      el("button", { type: "button", role: "tab", class: "tab", id: "ptab-" + id, "aria-selected": String(ui.tab === id), "aria-controls": "ppanel-" + id, tabindex: ui.tab === id ? 0 : -1, "data-fk": "ptab:" + id, text: label, onclick: () => setTab(id) })));
    list.addEventListener("keydown", (e) => {
      const i = TABS.findIndex((t) => t[0] === ui.tab);
      let j = -1;
      if (e.key === "ArrowRight") j = (i + 1) % TABS.length; else if (e.key === "ArrowLeft") j = (i + TABS.length - 1) % TABS.length; else if (e.key === "Home") j = 0; else if (e.key === "End") j = TABS.length - 1;
      if (j >= 0) { e.preventDefault(); ui.tab = TABS[j][0]; render(); const t = document.getElementById("ptab-" + ui.tab); if (t) t.focus(); }
    });
    return list;
  }

  // ---------------- Overview ----------------
  function permissionRow(c) {
    const noteId = "perm-note-" + c.id;
    const seg = UI.segmented(c.label + " permission", [
      { value: "allow", label: "Allow", disabled: !c.canAllow, describedBy: c.canAllow ? undefined : noteId },
      { value: "ask", label: "Ask" }, { value: "deny", label: "Deny" },
    ], c.decision, (v) => { change(c, v); return false; });   // false: the control keeps showing the STORED decision until the call has happened
    seg.querySelectorAll("button").forEach((b) => b.setAttribute("data-fk", "seg:" + c.id + ":" + b.dataset.value));
    return el("div", { class: "list-row perm-row", role: "listitem", id: "perm-" + c.id, dataset: { permission: c.id, decision: c.decision } }, [
      el("div", { class: "row-main" }, [
        el("span", { class: "row-title", text: c.label }),
        el("span", { class: "row-meta", text: c.explain }),
        el("div", { class: "perm-badges" }, [UI.riskBadge(c.risk), c.supported ? null : UI.chip("Not available in this prototype", "warn", "info")]),
      ]),
      el("div", { class: "perm-control" }, [seg, c.canAllow ? null : el("p", { class: "perm-note", id: noteId, text: "Always asks: cannot be pre-approved" })]),
    ]);
  }

  function overview() {
    const cat = catalog(), sum = M.summarize(cat);
    return [
      el("section", { class: "card opaque perm-summary", "aria-label": "Summary" }, [
        el("p", { class: "eyebrow", text: "Right now" }),
        el("p", { class: "perm-sentence", id: "passport-summary", text: sum.sentence }),
        el("p", { class: "screen-intro", text: "Every request from Aura or an app is checked against these choices. Choices that change files or leave this device always ask you again; that cannot be turned off." }),
      ]),
      el("div", { class: "list perm-list", role: "list", "aria-label": "Permissions" }, cat.map(permissionRow)),
    ];
  }

  // ---------------- Apps ----------------
  function appsTab() {
    const cat = catalog(), byId = Object.fromEntries(cat.map((c) => [c.id, c]));
    const cards = M.APPS.map((a) => {
      const targets = M.revokeTargets(a.id, cat);
      const rows = a.permissions.map((id) => { const c = byId[id]; return c ? el("li", { class: "perm-app-row", dataset: { permission: id } }, [el("span", { class: "perm-app-label", text: c.label }), UI.riskBadge(c.risk, true), UI.chip(...DEC_CHIP[c.decision])]) : null; });
      return el("article", { class: "card opaque perm-app", role: "listitem", dataset: { app: a.id } }, [
        el("div", { class: "perm-app-head" }, [UI.appTile(a.icon, a.hue), el("h3", { class: "section-title", text: a.name })]),
        rows.length ? el("ul", { class: "perm-app-list", "aria-label": a.name + " permissions" }, rows) : el("p", { class: "perm-none", text: "Needs no permissions." }),
        el("div", { class: "action-bar perm-app-actions" }, [
          a.permissions.length ? el("button", { type: "button", class: "btn outline btn-small", "data-fk": "change:" + a.id, "aria-label": "Change " + a.name + " permissions", onclick: () => { ui.tab = "overview"; render(); const first = document.getElementById("perm-" + a.permissions[0]); if (first) { first.scrollIntoView({ block: "center" }); const b = first.querySelector('[aria-checked="true"]'); if (b) b.focus(); } } }, [icon("edit"), el("span", { text: "Change" })]) : null,
          el("button", { type: "button", class: "btn outline btn-small", "data-fk": "revoke:" + a.id, "aria-label": "Revoke all for " + a.name, disabled: targets.length ? undefined : true, onclick: () => revokeAll(a.id) }, [icon("lock"), el("span", { text: "Revoke all" })]),
        ]),
      ]);
    });
    const hardware = el("section", { class: "card opaque", "aria-labelledby": "passport-hw" }, [
      el("h3", { class: "section-title", id: "passport-hw", text: "Hardware" }),
      el("ul", { class: "perm-app-list" }, M.HARDWARE.map((h) => el("li", { class: "perm-app-row", dataset: { hardware: h.id } }, [el("span", { class: "perm-hw-icon", "aria-hidden": "true" }, [icon(h.icon)]), el("span", { class: "perm-app-label", text: h.label }), UI.chip("Hardware-dependent · not available", "muted", "info")]))),
      el("p", { class: "review-note", text: "This prototype has no camera, microphone or location. A real device would ask here and show an indicator while one is in use." }),
    ]);
    return [
      el("p", { class: "callout callout-warn", id: "passport-capability-note" }, [icon("info"), el("span", { text: "In this prototype a permission belongs to a capability, not to one app. Changing it changes it for every app that uses it." })]),
      el("div", { class: "perm-app-grid", role: "list", "aria-label": "Apps" }, cards),
      hardware,
    ];
  }

  // ---------------- Activity ----------------
  function activityTab() {
    const log = System.permissions.log().slice().reverse();
    const blocked = System.receipts().filter((r) => r.status === "denied" || r.status === "needs-reapproval").slice(0, 20);
    const changes = log.length
      ? el("ul", { class: "gc-list perm-log", "aria-label": "Permission changes" }, log.map((e) => el("li", { class: "gc-item", dataset: { logId: e.id } }, [
        el("span", { class: "gc-item-title", text: e.label + ": " + W[e.from] + " → " + W[e.to] }), UI.chip(...DEC_CHIP[e.to]), el("span", { class: "gc-item-time", text: UI.timeAgo(e.at) })])))
      : UI.emptyState("passport", "No changes yet", "When you change a permission it is recorded here, newest first.");
    const stale = blocked.length
      ? el("ul", { class: "gc-list perm-blocked", "aria-label": "Blocked requests" }, blocked.map((r) => el("li", { class: "gc-item", dataset: { receipt: r.id } }, [
        el("span", { class: "gc-item-title", text: r.describe.title }), UI.chip(...(RECEIPT_CHIP[r.status])), el("span", { class: "gc-item-time", text: (r.errorText ? r.errorText + " · " : "") + UI.timeAgo(r.at) })])))
      : UI.emptyState("check", "Nothing was blocked", "Requests that Passport blocked or that needed a fresh review show up here.");
    return [
      el("section", { class: "card opaque", "aria-labelledby": "passport-act-1" }, [el("h3", { class: "section-title", id: "passport-act-1", text: "Permission changes" }), changes]),
      el("section", { class: "card opaque", "aria-labelledby": "passport-act-2" }, [el("h3", { class: "section-title", id: "passport-act-2", text: "Blocked or stale requests" }), stale]),
      el("div", { class: "action-bar" }, [el("button", { type: "button", class: "btn outline", id: "btn-passport-reset", "data-fk": "reset", onclick: resetDefaults }, [icon("undo"), el("span", { text: "Reset to defaults" })])]),
    ];
  }

  // ---------------- screen ----------------
  function render() {
    UI.keepFocus(screen, () => {
      clear(screen);
      screen.appendChild(el("header", { class: "screen-header" }, [
        el("button", { type: "button", class: "btn btn-icon ghost", id: "btn-passport-back", "aria-label": "Back", title: "Back", onclick: () => Shell.back() }, [icon("back")]),
        el("div", { class: "screen-heading" }, [el("p", { class: "eyebrow", text: "Permissions" }), el("h1", { class: "screen-title", text: "Passport" })]),
      ]));
      screen.appendChild(tablist());
      const body = ui.tab === "apps" ? appsTab() : ui.tab === "activity" ? activityTab() : overview();
      screen.appendChild(el("div", { class: "passport-panel", role: "tabpanel", id: "ppanel-" + ui.tab, "aria-labelledby": "ptab-" + ui.tab, tabindex: 0 }, body));
    }, "#btn-passport-back");
  }

  const live = () => screen.classList.contains("active");
  System.on("permission", () => { if (live()) render(); });
  System.on("receipts", () => { if (live()) render(); });
  Shell.onScreenShow((id) => { if (id === "screen-passport") render(); });
  render();

  window.AuroraPassport = { tab: () => ui.tab, setTab };
})();
