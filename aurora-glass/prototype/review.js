/* aurora-glass/prototype/review.js — the ONE trusted review surface (window.AuroraReview).
 *
 * It holds the broker's trusted-UI facet (taken once from system.js) and is the only code that can approve,
 * edit, skip or run a proposal. It renders:
 *   1. the compact review dialog used by System.request() for single actions (Vault delete, Passport, ...);
 *   2. the Aura Plan screen (#screen-aura): numbered editable steps -> confirmation -> progress -> receipt.
 *
 * SECURITY RULES (do not weaken):
 *  - Approval always carries the digest of exactly what was rendered. Any edit/skip makes a NEW proposal.
 *  - S3 steps need the hold-to-confirm control; there is no one-click or voice path.
 *  - A step whose permission is "deny", or that has no adapter, cannot be approved; the plan must skip it.
 *  - Receipts shown here are the broker's receipts. Never render a success the broker did not report.
 *  - Review panels are opaque (secure surface), never glass. All text via textContent.
 * SIMULATION BOUNDARY: one JS context; this is not an OS trust boundary (see broker.js header).
 */
(function () {
  "use strict";
  const UI = window.AuroraUI, System = window.AuroraSystem, Shell = window.AuroraShell;
  if (!UI || !System || !Shell) return;
  const { el, icon, clear } = UI;

  const trusted = System.takeTrusted(request);
  const T = trusted.ui;
  const DECISION = { allow: ["Allowed", "ok", "check"], ask: ["Asks you", "info", "eye"], deny: ["Blocked in Passport", "error", "lock"] };
  const STATUS = {
    done: ["Done", "ok", "check"], failed: ["Failed", "error", "alert"], denied: ["Blocked", "error", "lock"], unsupported: ["Not available", "warn", "info"],
    "needs-reapproval": ["Needs review again", "warn", "eye"], cancelled: ["Not run", "muted", "minus"], skipped: ["Skipped", "muted", "skip"], pending: ["Waiting", "muted", "more"], running: ["Running", "info", "play"],
  };
  const statusChip = (s) => { const d = STATUS[s] || STATUS.failed; return UI.chip(d[0], d[1], d[2]); };
  const liveSteps = (p) => p.preview.steps.filter((s) => !s.skipped);
  const blockers = (p) => liveSteps(p).filter((s) => s.decision === "deny" || !s.supported);
  const ROLLBACK = { full: "Every step can be undone afterwards.", limited: "Some steps cannot be undone afterwards.", none: "These steps cannot be undone afterwards." };

  function status(text, running, done, total) { System.emit("status", { text, running: !!running, done: done || 0, total: total || 0 }); }

  /** One step row. Shared by the dialog and the plan screen. */
  function stepRow(p, s, opts) {
    const o = opts || {};
    const d = System.describe(p.steps[s.index]);
    const dec = DECISION[s.decision];
    const meta = el("div", { class: "step-meta" }, [UI.riskBadge(s.risk), s.supported ? UI.chip(dec[0], dec[1], dec[2]) : UI.chip("Not available in this prototype", "warn", "info")]);
    const main = el("div", { class: "step-main" }, [el("span", { class: "step-kind", text: d.kind }), el("span", { class: "step-title", text: d.title }), el("span", { class: "step-detail", text: d.detail }), meta]);
    if (o.editor) main.appendChild(o.editor);
    return el("li", { class: "plan-step" + (s.skipped ? " is-skipped" : ""), dataset: { step: s.index + 1, risk: s.risk, type: s.type } }, [
      el("span", { class: "step-number", "aria-hidden": "true", text: String(s.index + 1) }), main, o.trailing || null,
    ]);
  }

  async function run(p, meta, onProgress) {
    const total = liveSteps(p).length; let n = 0;
    status("Running " + total + (total === 1 ? " step" : " steps"), true, 0, total);
    const result = await trusted.execute(p.id, { onProgress: (e) => { if (e.status === "done") n++; status(e.status === "running" ? System.describe(p.steps[e.index]).title : "Running", true, n, total); if (onProgress) onProgress(e); } });
    trusted.noteRun(p, result, meta);
    const ok = result.ok === true;
    status(ok ? (total === 1 ? System.describe(p.steps[liveSteps(p)[0].index]).title + " · done" : "Plan complete · " + total + " steps") : result.state === "CANCELLED" ? "Stopped" : "Did not finish", false, n, total);
    return Object.assign({ proposalId: p.id }, result);
  }

  // ------------------------------------------------------------------
  // 1. System.request(): single actions and small plans from app screens.
  // ------------------------------------------------------------------
  async function request(payload, meta) {
    const r = System.proposer.propose(payload);
    if (!r.ok) return { ok: false, reason: r.error || "invalid" };
    let p = r.proposal;
    if (!p.preview.needsReview) {                               // one S0 step that Passport allows
      const out = await run(p, meta);
      if (!out.ok && !(meta && meta.quiet)) UI.toast(System.explain(out.steps && out.steps[0] && out.steps[0].error || out.reason), { tone: "error" });
      return out;
    }
    const blocked = blockers(p);
    const list = el("ol", { class: "plan-steps" }, p.preview.steps.map((s) => stepRow(p, s)));
    const body = el("div", { class: "review-body" }, [list]);
    if (blocked.length) {
      body.appendChild(el("p", { class: "callout callout-error", role: "alert" }, [icon("lock"), el("span", { text: blocked[0].supported ? "Passport blocks this. Change the permission in Passport if you want to allow it." : "This prototype cannot do this, so it will not run." })]));
      const v = await UI.dialog({ id: "review-dialog", title: "This can't run", icon: "lock", body, dismissValue: null,
        actions: [{ label: "Close", kind: "outline", value: null }].concat(blocked[0].supported ? [{ label: "Open Passport", kind: "accent", value: "passport", id: "btn-review-passport" }] : []) });
      T.reject(p.id);
      if (v === "passport") Shell.openApp("passport");
      return { ok: false, reason: blocked[0].supported ? "permission-denied" : "unsupported", blocked: true };
    }
    const s3 = p.preview.s3Indices.length > 0;
    body.appendChild(el("p", { class: "review-note", text: (ROLLBACK[p.preview.expectedRollback] || "") + " Simulated: only this prototype changes." }));
    const ok = await UI.dialog({ id: "review-dialog", title: (meta && meta.title) || (s3 ? "Confirm this action" : "Review this change"), icon: s3 ? "alert" : "eye", tone: s3 ? "danger" : undefined, body, dismissValue: false,
      actions: [{ label: "Cancel", kind: "outline", value: false, id: "btn-review-cancel" }, { label: s3 ? "confirm" : "Approve", kind: s3 ? "danger" : "accent", value: true, hold: s3, id: "btn-review-approve" }] });
    if (!ok) { T.reject(p.id); return { ok: false, cancelled: true }; }
    const a = T.approve(p.id, { digest: p.digest, confirmS3: p.preview.s3Indices });
    if (!a.ok) { UI.toast(System.explain(a.reason), { tone: "error" }); return { ok: false, reason: a.reason }; }
    const out = await run(p, meta);
    if (!out.ok && !(meta && meta.quiet)) { const bad = (out.steps || []).find((s) => s.error); UI.toast(System.explain(bad ? bad.error : out.reason), { tone: "error" }); }
    return out;
  }

  // ------------------------------------------------------------------
  // 2. Aura Plan screen
  // ------------------------------------------------------------------
  const screen = document.getElementById("screen-aura");
  const plan = { p: null, text: "", editing: false, phase: "idle", result: null, progress: {}, meta: null };

  function header() {
    return el("header", { class: "screen-header" }, [
      el("button", { type: "button", class: "btn btn-icon ghost", id: "btn-aura-back", "aria-label": "Back", onclick: () => Shell.back() }, [icon("back")]),
      el("div", { class: "screen-heading" }, [el("p", { class: "eyebrow eyebrow-aura", text: "Aura" }), el("h1", { class: "screen-title", text: "Plan" })]),
    ]);
  }

  function editorFor(p, s) {
    const a = p.steps[s.index];
    const apply = (action) => { const r = T.editStep(plan.p.id, s.index, action); if (r.ok) { plan.p = r.proposal; render(); UI.announce("Step " + (s.index + 1) + " changed. The plan was previewed again."); } else UI.toast("That change is not valid.", { tone: "error" }); };
    const id = "step-edit-" + s.index;
    const select = (label, values, cur, names, make) => el("label", { class: "step-edit", for: id }, [el("span", { text: label }),
      el("select", { id, class: "field step-text", "data-fk": "edit:" + s.index, onchange: (e) => apply(make(e.target.value)) }, values.map((v) => { const o = el("option", { value: v, text: names(v) }); if (v === cur) o.selected = true; return o; }))]);
    const capz = (v) => v.charAt(0).toUpperCase() + v.slice(1);
    if (a.type === "space.switch") return select("Space", System.SPACES, a.arguments.space, (v) => System.SPACE_NAME[v] || v, (v) => ({ type: a.type, arguments: { space: v } }));
    if (a.type === "app.open") return select("App", System.APPS, a.arguments.app, capz, (v) => ({ type: a.type, arguments: { app: v } }));
    if (a.type === "focus.set") return select("Focus mode", ["on", "off"], a.arguments.enabled ? "on" : "off", capz, (v) => ({ type: a.type, arguments: { enabled: v === "on" } }));
    if (a.type === "timer.start") {
      return el("label", { class: "step-edit", for: id }, [el("span", { text: "Minutes" }),
        el("input", { id, class: "field step-text", type: "number", min: 1, max: 1440, step: 1, inputmode: "numeric", value: Math.max(1, Math.round(a.arguments.seconds / 60)), "data-fk": "edit:" + s.index,
          onchange: (e) => { const m = Number(e.target.value); if (Number.isInteger(m) && m >= 1 && m <= 1440) apply({ type: a.type, arguments: { seconds: m * 60 } }); else { UI.toast("Enter 1 to 1440 minutes.", { tone: "error" }); render(); } } })]);
    }
    return el("p", { class: "step-edit step-edit-none", text: "This step has nothing to edit. You can skip it." });
  }

  function renderReview(root) {
    const p = plan.p;
    const live = liveSteps(p), blocked = blockers(p);
    root.appendChild(el("section", { class: "card request-card", "aria-label": "Recognized request" }, [
      el("p", { class: "eyebrow", text: "Recognized request" }), el("p", { class: "request-text", id: "plan-request-text", text: "“" + plan.text + "”" }),
      el("p", { class: "request-note", text: "Nothing has run yet. Check each step, edit or skip what you don't want, then confirm." }),
    ]));
    const list = el("ol", { class: "plan-steps", "aria-label": "Plan steps" });
    p.preview.steps.forEach((s) => {
      const skip = el("button", { type: "button", class: "btn outline btn-small step-skip", "aria-pressed": String(s.skipped), "data-fk": "skip:" + s.index,
        "aria-label": (s.skipped ? "Include step " : "Skip step ") + (s.index + 1) + ": " + System.describe(p.steps[s.index]).title,
        onclick: () => { const r = T.skipStep(plan.p.id, s.index, !s.skipped); if (r.ok) { plan.p = r.proposal; render(); UI.announce("Step " + (s.index + 1) + (s.skipped ? " included" : " skipped")); } } }, [icon(s.skipped ? "plus" : "skip"), el("span", { text: s.skipped ? "Include" : "Skip" })]);
      list.appendChild(stepRow(p, s, { trailing: skip, editor: plan.editing && !s.skipped ? editorFor(p, s) : null }));
    });
    root.appendChild(list);
    const summary = el("section", { class: "card plan-summary", "aria-label": "Plan summary" }, [
      el("div", { class: "summary-row" }, [el("span", { class: "summary-label", text: "Highest risk" }), live.length ? UI.riskBadge(p.preview.risk) : UI.chip("Nothing to run", "muted")]),
      el("div", { class: "summary-row" }, [el("span", { class: "summary-label", text: "Undo" }), el("span", { class: "summary-value", text: live.length ? ROLLBACK[p.preview.expectedRollback] : "Every step is skipped." })]),
    ]);
    if (blocked.length) summary.appendChild(el("p", { class: "callout callout-error", role: "alert", id: "plan-blocked" }, [icon("lock"), el("span", { text: "Step " + (blocked[0].index + 1) + (blocked[0].supported ? " is blocked by Passport." : " is not available in this prototype.") + " Skip it to run the rest." })]));
    root.appendChild(summary);
    root.appendChild(el("div", { class: "action-bar" }, [
      el("button", { type: "button", class: "btn outline", id: "btn-plan-edit", "aria-pressed": String(plan.editing), "data-fk": "plan-edit", onclick: () => { plan.editing = !plan.editing; render(); UI.announce(plan.editing ? "Editing steps. Change a value to update the plan." : "Editing finished."); } }, [icon("edit"), el("span", { text: plan.editing ? "Done editing" : "Edit steps" })]),
      el("button", { type: "button", class: "btn accent", id: "btn-plan-confirm", "data-fk": "plan-confirm", disabled: live.length === 0 || blocked.length > 0 ? true : undefined, onclick: confirmPlan }, [el("span", { text: "Review and run" }), icon("chevron")]),
    ]));
  }

  async function confirmPlan() {
    const p = plan.p, live = liveSteps(p), s3 = p.preview.s3Indices.length > 0;
    const body = el("div", { class: "review-body" }, [
      el("p", { text: live.length + (live.length === 1 ? " step" : " steps") + " will run in this prototype, in order." }),
      el("ol", { class: "confirm-list" }, live.map((s) => el("li", {}, [el("span", { text: System.describe(p.steps[s.index]).title }), UI.riskBadge(s.risk, true)]))),
      el("p", { class: "review-note", text: ROLLBACK[p.preview.expectedRollback] + " You can stop between steps." }),
    ]);
    const ok = await UI.dialog({ id: "plan-confirmation", title: "Run this plan?", icon: s3 ? "alert" : "aura", tone: s3 ? "danger" : undefined, body, dismissValue: false,
      actions: [{ label: "Cancel", kind: "outline", value: false, id: "btn-plan-cancel" }, { label: s3 ? "run plan" : "Run plan", kind: s3 ? "danger" : "accent", value: true, hold: s3, id: "btn-plan-run" }] });
    if (!ok) { UI.announce("Nothing was run."); return; }
    const a = T.approve(p.id, { digest: p.digest, confirmS3: p.preview.s3Indices });
    if (!a.ok) {
      if (a.reason === "permission-changed" || a.reason === "preview-expired") { const r = T.refreshPreview(p.id); if (r.ok) plan.p = r.proposal; }
      UI.toast(System.explain(a.reason), { tone: "error" }); render(); return;
    }
    plan.phase = "running"; plan.progress = {}; render();
    const result = await run(p, plan.meta, (e) => { plan.progress[e.index] = e.status; if (plan.phase === "running") render(); });
    plan.result = result; plan.phase = result.ok ? "receipt" : "failure";
    render();
    if (result.ok) document.dispatchEvent(new CustomEvent("aurora:plan-complete", { detail: { proposalId: p.id } }));
    const h = screen.querySelector(".result-panel h2"); if (h) h.focus();
  }

  function renderRunning(root) {
    const p = plan.p, live = liveSteps(p);
    const doneCount = live.filter((s) => plan.progress[s.index] === "done").length;
    root.appendChild(el("section", { class: "card result-panel", id: "plan-running", role: "status", "aria-live": "polite" }, [
      el("p", { class: "eyebrow eyebrow-aura", text: "Running" }),
      el("h2", { class: "panel-title" }, [el("span", { id: "running-current", text: String(Math.min(live.length, doneCount + 1)) }), " of ", el("span", { class: "running-total", text: String(live.length) })]),
      el("div", { class: "meter", role: "progressbar", "aria-label": "Plan progress", "aria-valuemin": 0, "aria-valuemax": live.length, "aria-valuenow": doneCount }, [el("div", { class: "meter-fill meter-aura", style: { width: Math.round(doneCount / live.length * 100) + "%" } })]),
      el("ol", { class: "result-list" }, live.map((s) => el("li", { class: "result-row" }, [el("span", { class: "result-title", text: System.describe(p.steps[s.index]).title }), statusChip(plan.progress[s.index] || "pending")]))),
      el("div", { class: "action-bar" }, [el("button", { type: "button", class: "btn outline", id: "btn-plan-stop", "data-fk": "plan-stop", onclick: () => { trusted.cancel(p.id); UI.announce("Stopping after the current step."); } }, [icon("stop"), el("span", { text: "Stop after this step" })])]),
    ]));
  }

  function renderResult(root, failed) {
    const p = plan.p, res = plan.result, steps = res.steps || [];
    const rec = System.receipts();
    const done = steps.filter((s) => s.status === "done");
    const bad = steps.find((s) => s.error);
    const cancelled = res.state === "CANCELLED";
    const title = !failed ? "Plan complete" : cancelled ? "Plan stopped" : "Plan did not finish";
    const summary = !failed ? (done.length === 1 ? "1 step completed." : "All " + done.length + " steps completed.")
      : cancelled ? done.length + " of " + liveSteps(p).length + " steps ran before you stopped it."
      : done.length + " of " + liveSteps(p).length + " steps completed. " + (bad ? "Step " + (bad.index + 1) + ": " + System.explain(bad.error) : System.explain(res.reason));
    const list = el("ol", { class: "result-list receipt-list" });
    steps.forEach((s) => {
      const r = s.receiptId ? rec.find((x) => x.id === s.receiptId) : null;
      const undoState = r ? r.undoState : "unavailable";
      const undoBtn = undoState === "available" ? el("button", { type: "button", class: "btn outline btn-small", "data-fk": "undo:" + s.receiptId, "aria-label": "Undo: " + System.describe(p.steps[s.index]).title,
        onclick: async () => { const u = await System.undo(s.receiptId); UI.toast(u.ok ? "Undone: " + System.describe(p.steps[s.index]).title : u.text, { tone: u.ok ? "ok" : "error" }); render(); } }, [icon("undo"), el("span", { text: "Undo" })])
        : undoState === "used" ? UI.chip("Undone", "muted", "undo") : undoState === "failed" ? UI.chip("Undo failed", "error", "alert") : s.status === "done" ? UI.chip("Can't be undone", "muted", "lock") : null;
      list.appendChild(el("li", { class: "result-row", dataset: { status: s.status } }, [
        el("span", { class: "result-title" }, [el("span", { text: System.describe(p.steps[s.index]).title }), s.error ? el("span", { class: "result-error", text: System.explain(s.error) }) : null]),
        el("span", { class: "result-side" }, [statusChip(s.status), undoBtn]),
      ]));
    });
    const anyUndo = steps.some((s) => { const r = s.receiptId && rec.find((x) => x.id === s.receiptId); return r && r.undoState === "available"; });
    root.appendChild(el("section", { class: "card result-panel " + (failed ? "is-failed" : "is-ok"), id: failed ? "plan-failure" : "plan-receipt", role: "status" }, [
      el("p", { class: "eyebrow" + (failed ? " eyebrow-danger" : " eyebrow-aura"), text: failed ? "Receipt · incomplete" : "Receipt" }),
      el("h2", { class: "panel-title", tabindex: -1, text: title }),
      el("p", { class: "receipt-summary", id: failed ? "failure-text" : undefined, text: summary }),
      list,
      el("div", { class: "action-bar" }, [
        anyUndo ? el("button", { type: "button", class: "btn outline", id: "btn-plan-undo-all", "data-fk": "undo-all", onclick: async () => { const u = await System.undoProposal(p.id); UI.toast(u.ok ? "Completed steps were undone." : "Some steps could not be undone.", { tone: u.ok ? "ok" : "error" }); render(); } }, [icon("undo"), el("span", { text: failed ? "Undo completed steps" : "Undo all" })]) : null,
        el("button", { type: "button", class: "btn outline", id: "btn-plan-trail", onclick: () => Shell.openApp("trail") }, [icon("trail"), el("span", { text: "Open Trail" })]),
        el("button", { type: "button", class: "btn accent", id: "btn-receipt-close", "data-fk": "receipt-close", onclick: () => { plan.phase = "idle"; plan.p = null; Shell.showScreen("screen-home"); } }, [el("span", { text: "Done" })]),
      ]),
      el("p", { class: "receipt-foot", text: "Simulated receipt: only this browser prototype changed. No real device or file was touched." }),
    ]));
  }

  function render() {
    if (!screen) return;
    UI.keepFocus(screen, () => {
      clear(screen);
      screen.appendChild(header());
      const root = el("div", { class: "plan-body" });
      screen.appendChild(root);
      if (!plan.p) root.appendChild(UI.emptyState("aura", "No plan to review", "Ask Aura from Search, for example “prepare my phone for studying”.", UI.button("Open Search", { kind: "accent", icon: "search", onclick: () => Shell.openIndex("") })));
      else if (plan.phase === "running") renderRunning(root);
      else if (plan.phase === "receipt") renderResult(root, false);
      else if (plan.phase === "failure") renderResult(root, true);
      else renderReview(root);
    }, "#btn-aura-back");
  }

  /** Open the plan screen for an already-parsed plan payload ({steps:[...]}). Proposes only; nothing runs. */
  function openPlan(payload, text, meta) {
    const r = System.proposer.propose(payload);
    if (!r.ok) { UI.toast("Aura could not build that plan.", { tone: "error" }); return { ok: false, reason: r.error }; }
    if (plan.p && plan.phase === "review") T.reject(plan.p.id);
    plan.p = r.proposal; plan.text = text; plan.editing = false; plan.phase = "review"; plan.result = null; plan.progress = {}; plan.meta = Object.assign({ source: "aura", requestText: text }, meta || {});
    Shell.showScreen("screen-aura");
    return { ok: true };
  }

  /** Text -> Quick Command. A plan opens the review screen; a single action goes through System.request. */
  function ask(text) {
    const parsed = System.parseCommand(text);
    if (!parsed.ok) return Promise.resolve({ ok: false, reason: "unrecognized" });
    const mode = window.AuroraModels && window.AuroraModels.qs.get("auraMode");
    if (mode === "off") { UI.toast("Aura is off. Turn it on in Quick Center to run commands.", { tone: "warn" }); return Promise.resolve({ ok: false, reason: "aura-off" }); }
    if (parsed.kind === "plan") return Promise.resolve(openPlan(parsed.plan, text.trim()));
    return System.request(parsed.action, { source: "aura", requestText: text.trim() });
  }

  Shell.onScreenShow((id) => { if (id === "screen-aura") render(); });
  window.AuroraReview = { ask, openPlan, describeCommand(text) { const r = System.parseCommand(text); if (!r.ok) return null; const steps = r.kind === "plan" ? r.plan.steps : [r.action]; return { kind: r.kind, count: steps.length, title: r.kind === "plan" ? steps.length + "-step plan" : System.describe(steps[0]).title, preview: System.proposer.preview(r.kind === "plan" ? r.plan : r.action) }; } };
})();
