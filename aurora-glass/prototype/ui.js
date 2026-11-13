/* aurora-glass/prototype/ui.js — shared Aurora Glass UI primitives (window.AuroraUI).
 *
 * One place for the things every screen needs: safe element building, the original icon set, dialogs with a
 * focus trap, the hold-to-confirm control, toasts with an action, empty states, risk badges, segmented controls.
 *
 * RULES (see docs/agents/AURORA_DESIGN_SYSTEM.md):
 *  - Never innerHTML. All text goes through textContent; names, queries and file metadata are untrusted.
 *  - Every control is a real <button>/<input>/<select>, has an accessible name and a 48px target.
 *  - Styling lives in components.css; this file only assigns the documented class names.
 */
(function () {
  "use strict";
  const SVG = "http://www.w3.org/2000/svg";

  /** el("div", {class, text, onclick, "aria-label", dataset: {k: v}, hidden: bool}, [children | strings]) */
  function el(tag, props, children) {
    const n = document.createElement(tag);
    const p = props || {};
    Object.keys(p).forEach((k) => {
      const v = p[k];
      if (v === undefined || v === null || v === false) return;
      if (k === "text") n.textContent = String(v);
      else if (k === "class") n.className = v;
      else if (k === "dataset") Object.keys(v).forEach((d) => { n.dataset[d] = String(v[d]); });
      else if (k === "style") Object.keys(v).forEach((s) => n.style.setProperty(s, String(v[s])));
      else if (k.startsWith("on") && typeof v === "function") n.addEventListener(k.slice(2), v);
      else if (v === true) n.setAttribute(k, "");
      else n.setAttribute(k, String(v));
    });
    (children || []).forEach((c) => { if (c !== null && c !== undefined && c !== false) n.appendChild(typeof c === "string" ? document.createTextNode(c) : c); });
    return n;
  }
  function clear(node) { node.replaceChildren(); return node; }

  // ------------------------------------------------------------------
  // Icons: original 24x24 stroke glyphs drawn for AuroraOS (no third-party icon library).
  // ------------------------------------------------------------------
  const DOT = "h.01";
  const ICONS = {
    home: "M4 11l8-7 8 7M6 10v9h12v-9M10 19v-5h4v5",
    back: "M19 12H5M11 6l-6 6 6 6",
    spaces: "M4 4h7v7H4zM13 4h7v7h-7zM4 13h7v7H4zM13 13h7v7h-7z",
    pages: "M8 4h11v13H8zM5 8v12h10",
    search: "M11 4a7 7 0 1 0 0 14a7 7 0 0 0 0-14zM16.2 16.2L20 20",
    aura: "M3 17h18M6 17a6 6 0 0 1 12 0M12 5v3M5.6 8.6l1.8 1.8M18.4 8.6l-1.8 1.8M8 21h8",
    settings: "M4 7h9M17 7h3M4 12h3M11 12h9M4 17h11M19 17h1M15 5v4M9 10v4M17 15v4",
    notes: "M6 3h9l4 4v14H6zM14 3v5h5M9 12h7M9 16h5",
    files: "M3 6h6l2 2h10v11H3z",
    calculator: "M6 3h12v18H6zM9 7h6M9 12" + DOT + "M12 12" + DOT + "M15 12" + DOT + "M9 16" + DOT + "M12 16" + DOT + "M15 16" + DOT,
    clock: "M12 3a9 9 0 1 0 0 18a9 9 0 0 0 0-18zM12 7v5l3 2",
    calendar: "M4 6h16v14H4zM4 10h16M8 3v4M16 3v4",
    passport: "M12 3l7 3v5c0 4.5-3 8-7 10c-4-2-7-5.5-7-10V6zM9 12l2 2 4-4",
    trail: "M4 12a8 8 0 1 0 2.5-5.8M4 4v4h4M12 8v4l3 2",
    vault: "M4 5h16v4H4zM5 9v10h14V9M10 13h4",
    quick: "M7 5h10a3 3 0 0 1 0 6H7a3 3 0 0 1 0-6zM7 13h10a3 3 0 0 1 0 6H7a3 3 0 0 1 0-6zM8 8" + DOT + "M16 16" + DOT,
    bell: "M6 16v-5a6 6 0 0 1 12 0v5l2 2H4zM10 20a2 2 0 0 0 4 0",
    close: "M6 6l12 12M18 6L6 18",
    check: "M5 12l5 5 9-10",
    alert: "M12 4l9 16H3zM12 10v4M12 17" + DOT,
    info: "M12 3a9 9 0 1 0 0 18a9 9 0 0 0 0-18zM12 11v6M12 7.5" + DOT,
    lock: "M6 11h12v9H6zM8 11V8a4 4 0 0 1 8 0v3",
    undo: "M8 6L4 10l4 4M4 10h10a5 5 0 0 1 0 10h-3",
    trash: "M4 7h16M9 7V4h6v3M6 7l1 13h10l1-13M10 11v5M14 11v5",
    move: "M3 6h6l2 2h10v11H3zM10 13.5h6M13.5 11l2.5 2.5-2.5 2.5",
    pin: "M9 3h6l-1 6 3 3H7l3-3zM12 12v9",
    plus: "M12 5v14M5 12h14",
    minus: "M5 12h14",
    more: "M5 12" + DOT + "M12 12" + DOT + "M19 12" + DOT,
    chevron: "M9 6l6 6-6 6",
    "chevron-down": "M6 9l6 6 6-6",
    sun: "M12 8a4 4 0 1 0 0 8a4 4 0 0 0 0-8zM12 2v2M12 20v2M2 12h2M20 12h2M5 5l1.5 1.5M17.5 17.5L19 19M5 19l1.5-1.5M17.5 6.5L19 5",
    moon: "M20 14A8 8 0 1 1 10 4a6.5 6.5 0 0 0 10 10z",
    wifi: "M3 9a13 13 0 0 1 18 0M6 12.5a9 9 0 0 1 12 0M9 16a5 5 0 0 1 6 0M12 19.5" + DOT,
    bluetooth: "M7 8l10 8-5 4V4l5 4-10 8",
    battery: "M3 8h15v8H3zM21 11v2M6 11v2M9 11v2",
    focus: "M12 3a9 9 0 1 0 0 18a9 9 0 0 0 0-18zM12 8a4 4 0 1 0 0 8a4 4 0 0 0 0-8z",
    rotate: "M7 3h10v18H7zM11 18h2",
    mic: "M12 3a3 3 0 0 0-3 3v5a3 3 0 0 0 6 0V6a3 3 0 0 0-3-3zM6 11a6 6 0 0 0 12 0M12 17v4",
    camera: "M4 8h3l2-3h6l2 3h3v11H4zM12 10a3.5 3.5 0 1 0 0 7a3.5 3.5 0 0 0 0-7z",
    location: "M12 21c-4-5-6-8-6-11a6 6 0 0 1 12 0c0 3-2 6-6 11zM12 8a2 2 0 1 0 0 4a2 2 0 0 0 0-4z",
    play: "M8 5l11 7-11 7z",
    pause: "M8 5v14M16 5v14",
    stop: "M6 6h12v12H6z",
    list: "M8 6h12M8 12h12M8 18h12M4 6" + DOT + "M4 12" + DOT + "M4 18" + DOT,
    edit: "M4 20h4l11-11-4-4L4 16zM13 7l4 4",
    volume: "M4 10h3l5-4v12l-5-4H4zM16 9a4 4 0 0 1 0 6",
    file: "M6 3h9l4 4v14H6zM14 3v5h5",
    image: "M4 5h16v14H4zM4 16l5-5 4 4 3-3 4 4M9 9" + DOT,
    sort: "M7 5v14M4 16l3 3 3-3M14 7h6M14 12h4M14 17h2",
    filter: "M4 6h16l-6 7v6l-4-2v-4z",
    share: "M12 15V4M8 8l4-4 4 4M5 13v6h14v-6",
    skip: "M5 5l9 7-9 7zM18 5v14",
    timer: "M12 7a7 7 0 1 0 0 14a7 7 0 0 0 0-14zM12 11v3M10 3h4M12 3v4",
    flag: "M6 21V4h11l-2 4 2 4H6",
    eye: "M2 12c3-5 6-7 10-7s7 2 10 7c-3 5-6 7-10 7s-7-2-10-7zM12 9a3 3 0 1 0 0 6a3 3 0 0 0 0-6z",
    keyboard: "M3 7h18v10H3zM7 11" + DOT + "M11 11" + DOT + "M15 11" + DOT + "M8 14h8",
  };

  /** icon("home") -> decorative <svg class="icon">. Unknown names fall back to "info" so nothing renders blank. */
  function icon(name, extraClass) {
    const svg = document.createElementNS(SVG, "svg");
    svg.setAttribute("viewBox", "0 0 24 24");
    svg.setAttribute("class", "icon" + (extraClass ? " " + extraClass : ""));
    svg.setAttribute("aria-hidden", "true");
    svg.setAttribute("focusable", "false");
    const path = document.createElementNS(SVG, "path");
    path.setAttribute("d", Object.prototype.hasOwnProperty.call(ICONS, name) ? ICONS[name] : ICONS.info);
    svg.appendChild(path);
    svg.dataset.icon = name;
    return svg;
  }

  /** The AuroraOS app tile: rounded tinted square with a glyph. `hue` is 0–360. */
  function appTile(iconName, hue, size) {
    return el("span", { class: "app-tile" + (size ? " app-tile-" + size : ""), style: { "--h": hue === undefined ? 215 : hue }, "aria-hidden": "true" }, [icon(iconName)]);
  }

  // ------------------------------------------------------------------
  // Live region + toast
  // ------------------------------------------------------------------
  function announce(text) {
    const live = document.getElementById("a11y-live");
    if (!live) return;
    live.textContent = "";
    live.textContent = String(text);
  }

  let toastTimer = null;
  /** toast("Moved to Work", {action: {label: "Undo", run: fn}, tone: "ok" | "warn" | "error", ms}) */
  function toast(text, opts) {
    const o = opts || {};
    const box = document.getElementById("toast");
    if (!box) return;
    clearTimeout(toastTimer);
    clear(box);
    box.className = "toast show" + (o.tone ? " toast-" + o.tone : "");
    box.appendChild(el("span", { class: "toast-text", text }));
    if (o.action && typeof o.action.run === "function") {
      box.appendChild(el("button", { type: "button", class: "toast-action", text: o.action.label, onclick: () => { hide(); o.action.run(); } }));
    }
    function hide() { box.classList.remove("show"); }
    toastTimer = setTimeout(hide, o.ms || (o.action ? 6000 : 2800));
    announce(text);
  }

  // ------------------------------------------------------------------
  // Small building blocks
  // ------------------------------------------------------------------
  function button(label, opts) {
    const o = opts || {};
    const kids = [];
    if (o.icon) kids.push(icon(o.icon));
    if (!o.iconOnly) kids.push(el("span", { class: "btn-label", text: label }));
    const b = el("button", {
      type: "button", class: "btn " + (o.kind || "outline") + (o.iconOnly ? " btn-icon" : "") + (o.small ? " btn-small" : ""),
      id: o.id, "aria-label": o.iconOnly ? label : o.ariaLabel, title: o.iconOnly ? label : undefined, "data-fk": o.fk, disabled: o.disabled ? true : undefined,
      onclick: o.onclick,
    }, kids);
    return b;
  }

  const RISK_TEXT = { S0: "Safe", S1: "Reversible setting", S2: "Changes your files", S3: "Destructive or leaves this device" };
  const RISK_ICON = { S0: "check", S1: "undo", S2: "edit", S3: "alert" };
  /** Risk badge: always icon + code + words, never colour alone. */
  function riskBadge(risk, short) {
    const r = Object.prototype.hasOwnProperty.call(RISK_TEXT, risk) ? risk : "S3";
    return el("span", { class: "risk risk-" + r.toLowerCase(), "data-risk": r }, [icon(RISK_ICON[r]), el("span", { text: short ? r : r + " · " + RISK_TEXT[r] })]);
  }

  /** Status chip: tone = ok | warn | error | info | muted. Icon + text. */
  function chip(text, tone, iconName) {
    return el("span", { class: "chip chip-" + (tone || "muted") }, [iconName ? icon(iconName) : null, el("span", { text })]);
  }

  function emptyState(iconName, title, body, action) {
    return el("div", { class: "empty-state", role: "status" }, [
      el("span", { class: "empty-icon", "aria-hidden": "true" }, [icon(iconName)]),
      el("p", { class: "empty-title", text: title }),
      body ? el("p", { class: "empty-body", text: body }) : null,
      action || null,
    ]);
  }

  /** Segmented control (radio group). options: [{value, label, icon?}] */
  function segmented(name, options, value, onChange) {
    // options: [{value, label, icon?, disabled?, describedBy?}]. A disabled option cannot be chosen (and is skipped by the arrow keys);
    // describedBy points at visible text that says why. onChange(v) may return false to refuse: the control then shows the previous value.
    const group = el("div", { class: "segmented", role: "radiogroup", "aria-label": name });
    let current = value;
    function select(v, focus) {
      current = v;
      group.querySelectorAll("button").forEach((b) => {
        const on = b.dataset.value === v;
        b.setAttribute("aria-checked", String(on)); b.tabIndex = on ? 0 : -1;
        if (on && focus) b.focus();
      });
    }
    function choose(v, focus) {
      const o = options.find((x) => x.value === v);
      if (!o || o.disabled) return;
      const prev = current;
      select(v, focus);
      if (onChange(v) === false) select(prev, focus);
    }
    options.forEach((o) => {
      group.appendChild(el("button", { type: "button", role: "radio", class: "segment", dataset: { value: o.value }, "aria-label": o.icon ? o.label : undefined, disabled: o.disabled ? true : undefined,
        "aria-describedby": o.describedBy, onclick: () => choose(o.value, false) }, [o.icon ? icon(o.icon) : null, o.icon ? null : el("span", { text: o.label })]));
    });
    group.addEventListener("keydown", (e) => {
      const dir = e.key === "ArrowRight" || e.key === "ArrowDown" ? 1 : e.key === "ArrowLeft" || e.key === "ArrowUp" ? -1 : 0;
      if (!dir) return;
      let k = options.findIndex((o) => o.value === current);
      for (let step = 0; step < options.length; step++) {
        k = (k + dir + options.length) % options.length;
        if (!options[k].disabled) { e.preventDefault(); choose(options[k].value, true); return; }
      }
    });
    group.setValue = (v) => select(v);          // reflect a stored value without calling onChange
    select(value);
    return group;
  }

  /** Re-render while keeping keyboard focus on the control with the same data-fk. */
  function keepFocus(container, render, fallbackSel) {
    const a = document.activeElement;
    const key = a && container.contains(a) ? a.getAttribute("data-fk") : null;
    render();
    if (!key) return;
    const next = container.querySelector('[data-fk="' + CSS.escape(key) + '"]');
    if (next && !next.disabled) { next.focus(); return; }
    const sibling = container.querySelector('[data-fk="' + CSS.escape(key.split(":")[0]) + ':primary"]');
    const fb = sibling || (fallbackSel && container.querySelector(fallbackSel));
    if (fb) fb.focus();
  }

  // ------------------------------------------------------------------
  // Hold-to-confirm (S3). Pointer OR keyboard hold; a visible two-step alternative for people who cannot hold.
  // ------------------------------------------------------------------
  function holdButton(label, onConfirm, opts) {
    const o = opts || {};
    const ms = o.ms || 1200;
    let timer = null, done = false;
    const fill = el("span", { class: "hold-fill", "aria-hidden": "true" });
    const text = el("span", { class: "btn-label", text: "Hold to " + label });
    const btn = el("button", { type: "button", class: "btn danger hold-btn", id: o.id, "aria-describedby": o.describedBy }, [fill, icon("alert"), text]);
    btn.style.setProperty("--hold-ms", ms + "ms");
    function start() {
      if (done || timer) return;
      btn.classList.add("holding");
      timer = setTimeout(() => { done = true; timer = null; btn.classList.remove("holding"); btn.classList.add("held"); onConfirm(); }, ms);
    }
    function cancel() { if (timer) { clearTimeout(timer); timer = null; } btn.classList.remove("holding"); }
    btn.addEventListener("pointerdown", (e) => { if (e.button === 0) start(); });
    ["pointerup", "pointerleave", "pointercancel", "blur"].forEach((t) => btn.addEventListener(t, cancel));
    btn.addEventListener("keydown", (e) => { if ((e.key === " " || e.key === "Enter") && !e.repeat) { e.preventDefault(); start(); } });
    btn.addEventListener("keyup", (e) => { if (e.key === " " || e.key === "Enter") cancel(); });
    btn.addEventListener("click", (e) => e.preventDefault());

    // Two-step alternative: an explicit, separate, deliberate second press. Never a single click.
    let armed = false;
    const alt = el("button", { type: "button", class: "btn-link hold-alt", text: "Can't hold? Use two steps" });
    alt.addEventListener("click", () => {
      if (done) return;
      if (!armed) { armed = true; alt.textContent = "Step 2: press again to " + label; alt.classList.add("armed"); announce("Press again to " + label + ". Press Escape to cancel."); }
      else { done = true; onConfirm(); }
    });
    return el("div", { class: "hold-wrap" }, [btn, alt]);
  }

  // ------------------------------------------------------------------
  // Dialog: modal, opaque, focus-trapped, Escape = cancel, focus returns to the opener.
  // ------------------------------------------------------------------
  let openDialogs = 0;
  /**
   * dialog({ title, body: Node|string, tone: "danger", icon, actions: [{label, kind, value, hold}], dismissValue })
   * Resolves with the chosen action's `value` (or dismissValue / null on Escape or scrim click).
   * An action with hold:true renders as hold-to-confirm.
   */
  function dialog(opts) {
    const o = opts || {};
    return new Promise((resolve) => {
      const opener = document.activeElement;
      const titleId = "dlg-title-" + Date.now() + "-" + (++openDialogs);
      const bodyId = titleId + "-body";
      const actions = el("div", { class: "dialog-actions" });
      const body = typeof o.body === "string" ? el("p", { text: o.body }) : (o.body || el("p", {}));
      const panel = el("div", { class: "dialog" + (o.tone ? " dialog-" + o.tone : ""), role: o.role || (o.tone === "danger" ? "alertdialog" : "dialog"), "aria-modal": "true", "aria-labelledby": titleId, "aria-describedby": bodyId, id: o.id }, [
        el("div", { class: "dialog-head" }, [o.icon ? el("span", { class: "dialog-icon" }, [icon(o.icon)]) : null, el("h2", { class: "dialog-title", id: titleId, text: o.title || "" })]),
        el("div", { class: "dialog-body", id: bodyId }, [body]),
        actions,
      ]);
      const scrim = el("div", { class: "scrim" }, [panel]);
      function close(value) {
        document.removeEventListener("keydown", onKey, true);
        scrim.remove();
        if (opener && document.contains(opener) && opener.focus) opener.focus();
        resolve(value);
      }
      (o.actions || [{ label: "Close", kind: "outline", value: null }]).forEach((a) => {
        if (a.hold) actions.appendChild(holdButton(a.label, () => close(a.value), { id: a.id, describedBy: bodyId }));
        else actions.appendChild(el("button", { type: "button", class: "btn " + (a.kind || "outline"), id: a.id, text: a.label, onclick: () => close(a.value) }));
      });
      function focusables() { return [...panel.querySelectorAll("button, input, select, textarea, [tabindex='0']")].filter((n) => !n.disabled && n.offsetParent !== null); }
      function onKey(e) {
        const all = document.querySelectorAll(".scrim");
        if (all.length && all[all.length - 1] !== scrim) return;          // not the top-most dialog
        if (e.key === "Escape") { e.preventDefault(); e.stopPropagation(); close(o.dismissValue === undefined ? null : o.dismissValue); return; }
        if (e.key !== "Tab") return;
        const f = focusables(); if (!f.length) return;
        const i = f.indexOf(document.activeElement);
        e.preventDefault();
        if (i < 0) f[0].focus(); else f[(i + (e.shiftKey ? f.length - 1 : 1)) % f.length].focus();
      }
      document.addEventListener("keydown", onKey, true);
      scrim.addEventListener("pointerdown", (e) => { if (e.target === scrim && !o.sticky) close(o.dismissValue === undefined ? null : o.dismissValue); });
      (document.getElementById("app") || document.body).appendChild(scrim);
      // Focus the SAFE choice first (first non-danger, non-hold button), never the destructive one.
      const f = focusables();
      const safe = o.initialFocus ? panel.querySelector(o.initialFocus) : f.find((n) => !n.classList.contains("danger") && !n.classList.contains("accent") && !n.classList.contains("hold-alt")) || f[0];
      if (safe) safe.focus();
      announce((o.title || "Dialog") + ". " + (typeof o.body === "string" ? o.body : ""));
    });
  }

  /** confirm({title, body, confirmLabel, danger, hold}) -> Promise<boolean> */
  function confirm(o) {
    return dialog({
      title: o.title, body: o.body, tone: o.danger ? "danger" : undefined, icon: o.danger ? "alert" : o.icon, id: o.id, dismissValue: false,
      actions: [{ label: o.cancelLabel || "Cancel", kind: "outline", value: false, id: o.cancelId }, { label: o.confirmLabel || "Confirm", kind: o.danger ? "danger" : "accent", value: true, hold: !!o.hold, id: o.confirmId }],
    });
  }

  /** prompt({title, label, value, validate(text) -> errorString|null, confirmLabel}) -> Promise<string|null> */
  function prompt(o) {
    const input = el("input", { type: "text", class: "field", value: o.value || "", maxlength: o.maxLength || 40, autocomplete: "off", id: "dlg-input" });
    const err = el("p", { class: "field-error", role: "alert" });
    const body = el("div", { class: "field-group" }, [el("label", { for: "dlg-input", text: o.label }), input, err]);
    return new Promise((resolve) => {
      let settled = false;
      const p = dialog({ title: o.title, body, id: o.id, initialFocus: "#dlg-input", sticky: true, dismissValue: "__cancel",
        actions: [{ label: "Cancel", kind: "outline", value: "__cancel" }, { label: o.confirmLabel || "Save", kind: "accent", value: "__ok" }] });
      input.addEventListener("keydown", (e) => { if (e.key === "Enter") { e.preventDefault(); input.closest(".dialog").querySelector(".btn.accent").click(); } });
      p.then((v) => {
        if (settled) return; settled = true;
        if (v !== "__ok") { resolve(null); return; }
        resolve(input.value.trim());
      });
      // The confirm button is disabled while the text is invalid, so "OK" can only ever return valid text.
      const okBtn = input.closest(".dialog").querySelector(".btn.accent");
      const check = (quiet) => { const problem = o.validate ? o.validate(input.value.trim()) : null; err.textContent = quiet ? "" : problem || ""; input.setAttribute("aria-invalid", problem && !quiet ? "true" : "false"); okBtn.disabled = !!problem; };
      input.addEventListener("input", () => check(false));
      check(true);
      input.select();
    });
  }

  /** Human-friendly relative time for receipts and files ("just now", "5 min ago", "Tue 14:02"). */
  function timeAgo(ts, now) {
    const d = Math.max(0, (now === undefined ? Date.now() : now) - ts);
    if (d < 45e3) return "just now";
    if (d < 3600e3) return Math.round(d / 60e3) + " min ago";
    if (d < 86400e3) return Math.round(d / 3600e3) + " h ago";
    return new Date(ts).toLocaleDateString(undefined, { weekday: "short", day: "numeric", month: "short" });
  }

  window.AuroraUI = { el, clear, icon, ICONS: Object.keys(ICONS), appTile, announce, toast, button, riskBadge, RISK_TEXT, chip, emptyState, segmented, keepFocus, holdButton, dialog, confirm, prompt, timeAgo };
})();
