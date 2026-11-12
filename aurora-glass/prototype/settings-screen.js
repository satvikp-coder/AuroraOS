/* aurora-glass/prototype/settings-screen.js — the Settings screen (DOM) for the Aurora Glass prototype.
 * SIMULATION ONLY: nothing is persisted and no real device setting is changed.
 * All dynamic text uses DOM APIs (textContent); no innerHTML.
 */
(function () {
  "use strict";
  const Shell = window.AuroraShell, Models = window.AuroraModels, SettingsLib = window.AuroraSettings;
  if (!Shell || !Models || !SettingsLib || !window.AuroraUI) return;
  const el = window.AuroraUI.el;

  const settings = SettingsLib.createSettings();
  window.AuroraSettingsModel = settings;       // exposed for tests only
  const qs = Models.qs;

  const CHOICE_LABELS = {
    "appearance.theme": { light: "Light", dark: "Dark" },
    "appearance.glass": { soft: "Soft Glass", clear: "Clear (opaque)" },
    "accessibility.textSize": { "100": "100% (default)", "125": "125%", "150": "150%", "200": "200%" },
    "aura.mode": { off: "Off", ask: "Ask before acting", quick: "Quick Commands only" },
  };
  const EFFECT_NOTE = { stored: "Stored only (no effect yet)", locked: "Locked" };

  let filter = "";
  let lastFocusKey = null;

  function showAdvanced() { return settings.get("developer.showAdvanced") === true; }
  function sectionLabel(id) { return settings.sections.find((s) => s.id === id).label; }
  function valueText(e) {
    if (e.type === "toggle") return e.value ? "On" : "Off";
    if (e.type === "choice") return (CHOICE_LABELS[e.id] && CHOICE_LABELS[e.id][e.value]) || e.value;
    if (e.type === "range") return e.value + " days";
    return String(e.value);
  }
  /** The glass style is forced to Clear while battery saver is on; show what is really in effect. */
  function effectiveValue(e) { return e.id === "appearance.glass" && settings.get("battery.saver") ? "clear" : e.value; }

  // ---------------- rendering ----------------
  function visibleEntries(sectionId) {
    if (filter.trim()) {
      const ids = new Set(settings.search(filter).map((e) => e.id));            // search covers advanced entries too
      return settings.list(sectionId, true).filter((e) => ids.has(e.id));
    }
    return settings.list(sectionId, showAdvanced());
  }

  function renderRow(e) {
    const labelId = "st-label-" + e.id.replace(/\./g, "-");
    const row = el("div", { class: "setting-row", "data-setting": e.id, role: "group", "aria-labelledby": labelId });
    const note = EFFECT_NOTE[e.effect];
    const label = el("span", { class: "setting-label", id: labelId }, [document.createTextNode(e.label)]);
    if (note) label.appendChild(el("span", { class: "setting-note", text: note }));
    row.appendChild(label);

    const cur = effectiveValue(e);
    let control;
    if (e.type === "toggle") {
      control = el("button", { type: "button", class: "switch", role: "switch", "aria-checked": String(!!e.value), "aria-labelledby": labelId, "data-fk": e.id,
        onclick: () => { settings.set(e.id, !settings.get(e.id)); Shell.announce(e.label + (settings.get(e.id) ? " on" : " off")); } },
        [el("span", { text: e.value ? "On" : "Off" })]);
    } else if (e.type === "choice") {
      const sel = el("select", { "aria-labelledby": labelId, "data-fk": e.id, onchange: (ev) => { settings.set(e.id, ev.target.value); Shell.announce(e.label + " set to " + valueText(settings.entry(e.id))); } });
      e.options.forEach((o) => { const opt = el("option", { value: o, text: (CHOICE_LABELS[e.id] && CHOICE_LABELS[e.id][o]) || o }); if (o === cur) opt.selected = true; sel.appendChild(opt); });
      if (e.id === "appearance.glass" && settings.get("battery.saver")) { sel.disabled = true; label.appendChild(el("span", { class: "setting-note", text: "Battery saver keeps Clear mode" })); }
      control = sel;
    } else if (e.type === "range") {
      const out = el("output", { id: "st-out-" + e.id.replace(/\./g, "-"), text: valueText(e) });
      const input = el("input", { type: "range", min: String(e.min), max: String(e.max), step: "1", value: String(e.value), "aria-labelledby": labelId, "data-fk": e.id,
        oninput: (ev) => { settings.set(e.id, Number(ev.target.value)); } });
      control = el("span", { class: "setting-control-range" }, [input, document.createTextNode(" "), out]);
    } else {
      control = el("span", { class: "setting-value", text: valueText(e) });
    }
    row.appendChild(el("span", { class: "setting-control" }, [control]));
    return row;
  }

  function render() {
    const root = document.getElementById("settings-sections");
    const a = document.activeElement;
    const key = a && root.contains(a) ? a.getAttribute("data-fk") : lastFocusKey;
    root.replaceChildren();
    let count = 0;
    settings.sections.forEach((sec) => {
      const entries = visibleEntries(sec.id);
      if (!entries.length) return;
      count += entries.length;
      const card = el("section", { class: "card opaque settings-group", "aria-labelledby": "st-sec-" + sec.id }, [el("h2", { class: "section-title", id: "st-sec-" + sec.id, text: sec.label })]);
      entries.forEach((e) => card.appendChild(renderRow(e)));
      root.appendChild(card);
    });
    document.getElementById("settings-empty").classList.toggle("panel-hidden", count > 0);
    if (key) { const next = root.querySelector('[data-fk="' + CSS.escape(key) + '"]'); if (next && !next.disabled) next.focus(); }
    lastFocusKey = null;
    const jsonEl = document.getElementById("settings-json");
    if (!jsonEl.classList.contains("panel-hidden")) jsonEl.textContent = JSON.stringify(settings.snapshot(), null, 2);
  }

  // ---------------- effects (what each setting really does in the prototype) ----------------
  function apply(id, v) {
    const root = document.documentElement;
    switch (id) {
      case "appearance.theme": qs.set("appearance", v); break;
      case "appearance.glass": if (!settings.get("battery.saver")) Shell.setTier(v); break;
      case "accessibility.textSize": if (v === "100") root.removeAttribute("data-text-scale"); else root.setAttribute("data-text-scale", v); break;
      case "accessibility.highContrast": Shell.setContrast(v ? "high" : "normal"); break;
      case "accessibility.reduceMotion": Shell.setMotion(v ? "reduce" : "normal"); break;
      case "notifications.focus": qs.set("focus", v); break;
      case "notifications.glassCorner": { const gc = document.querySelector(".glass-corner"); if (gc) gc.hidden = !v; break; }
      case "privacy.indicators": { const c = document.getElementById("qc-privacy-card"); if (c) c.classList.toggle("panel-hidden", !v); break; }
      case "battery.saver": qs.set("batterySaver", v); break;
      case "aura.mode": qs.set("auraMode", v); break;
      case "developer.failAuraStep": if (window.AuroraSystem) window.AuroraSystem.dev.failNextType = v ? "app.open" : null; break;
      default: break;                                   // "stored" entries and the advanced-view switch have no side effect
    }
  }
  settings.onChange((id, v) => { apply(id, v); render(); Shell.refreshIndex && document.getElementById("screen-index").classList.contains("active") && Shell.refreshIndex(); });

  // The simulated failure fires once: when the broker has consumed it, switch the setting back off.
  if (window.AuroraSystem) window.AuroraSystem.on("receipts", () => { if (settings.get("developer.failAuraStep") && window.AuroraSystem.dev.failNextType === null) settings.set("developer.failAuraStep", false); });

  // Quick Center and the Settings registry describe the same state: keep them in step both ways.
  const QS_TO_SETTING = { appearance: "appearance.theme", focus: "notifications.focus", batterySaver: "battery.saver", auraMode: "aura.mode" };
  qs.onChange((key, value) => { const id = QS_TO_SETTING[key]; if (id && settings.get(id) !== value) settings.set(id, value); else render(); });

  // ---------------- Index integration ----------------
  function openSetting(id) {
    const e = settings.entry(id);
    if (!e) return;
    filter = ""; document.getElementById("settings-filter").value = "";
    if (e.advanced && !showAdvanced()) settings.set("developer.showAdvanced", true);
    lastFocusKey = null;
    Shell.showScreen("screen-settings");
    render();
    const row = document.querySelector('#settings-sections [data-setting="' + CSS.escape(id) + '"]');
    if (row) {
      row.scrollIntoView({ block: "center" });
      row.classList.add("settings-highlight");
      setTimeout(() => row.classList.remove("settings-highlight"), 1500);
      const ctl = row.querySelector("[data-fk]") || row;
      if (ctl.focus) { if (!ctl.hasAttribute("tabindex") && !ctl.getAttribute("data-fk")) ctl.setAttribute("tabindex", "-1"); ctl.focus(); }
    }
    Shell.announce("Settings: " + e.label);
  }
  window.AuroraOpenSetting = openSetting;
  Shell.registerIndexSource((q) => settings.search(q).map((e) => ({
    kind: "Setting", label: e.label, meta: sectionLabel(e.section) + " · " + valueText(e), action: () => openSetting(e.id),
  })));

  // ---------------- wiring ----------------
  function init() {
    document.getElementById("btn-quick-settings").addEventListener("click", () => Shell.showScreen("screen-settings"));
    document.getElementById("settings-filter").addEventListener("input", (e) => { filter = e.target.value; render(); Shell.announce(document.querySelectorAll("#settings-sections .setting-row").length + " settings shown"); });

    const jsonBtn = document.getElementById("btn-settings-json"), jsonEl = document.getElementById("settings-json");
    jsonBtn.addEventListener("click", () => {
      const open = jsonEl.classList.toggle("panel-hidden") === false;
      jsonBtn.setAttribute("aria-expanded", String(open));
      jsonBtn.textContent = open ? "Hide settings JSON" : "Show settings as JSON";
      if (open) jsonEl.textContent = JSON.stringify(settings.snapshot(), null, 2);
    });

    // Reset confirmation: the shared modal dialog (focus trap, Escape, focus returns to this button).
    const resetBtn = document.getElementById("btn-settings-reset");
    resetBtn.addEventListener("click", async () => {
      const ok = await window.AuroraUI.confirm({ id: "settings-reset-confirm", icon: "undo", title: "Reset all settings?", body: "Every setting returns to its default. This only affects the prototype.",
        confirmLabel: "Reset", confirmId: "btn-settings-reset-ok", cancelLabel: "Cancel", cancelId: "btn-settings-reset-cancel" });
      if (ok) { settings.reset(); Shell.announce("All settings reset to defaults"); } else Shell.announce("Reset cancelled");
    });

    Shell.onScreenShow((id) => {
      if (id !== "screen-settings") return;
      if (settings.get("appearance.theme") !== Shell.state.theme) settings.set("appearance.theme", Shell.state.theme);
      render();
    });
    render();
  }

  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", init);
  else init();
})();
