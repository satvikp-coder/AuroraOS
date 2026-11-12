/* aurora-glass/prototype/screens.js — Pages and Quick Center screens.
 * SIMULATION ONLY: nothing here manages real processes, memory, radios or device settings.
 * All dynamic text is inserted with textContent / DOM APIs (no innerHTML).
 */
(function () {
  "use strict";
  const Core = window.AuroraCore;
  const Shell = window.AuroraShell;
  const UI = window.AuroraUI;
  if (!Core || !Shell || !UI) return;
  const el = UI.el;
  const keepFocus = UI.keepFocus;

  // ---------------- Shared simulated models ----------------
  const pages = Core.createPages({
    budgetMB: 2048,
    pages: [
      { id: "page-notes", appId: "notes", name: "Notes", space: "study", state: "running", memMB: 180 },
      { id: "page-clock", appId: "clock", name: "Clock", space: "study", state: "paused", memMB: 90 },
      { id: "page-files", appId: "files", name: "Files", space: "work", state: "paused", memMB: 240 },
      { id: "page-calc", appId: "calculator", name: "Calculator", space: "personal", state: "saved", memMB: 60 },
    ],
  });
  const qs = Core.createQuickSettings();
  const sessions = { microphone: false, camera: false, location: false };
  window.AuroraModels = { pages, qs, sessions }; // exposed for tests / later modules

  const STATE_LABEL = { running: "Running", paused: "Paused", saved: "Saved (frozen)" };
  const spaceName = (id) => { const hit = Shell.spaces().find((x) => x.id === id); return hit ? hit.name : id; };

  // ---------------- Pages ----------------
  function renderPages() { keepFocus(document.getElementById("screen-pages"), renderPagesInner, "#btn-pages-back"); }
  function btn(iconName, label, aria, fk, kind, onclick) {
    return el("button", { class: "btn " + kind, type: "button", "data-fk": fk, "aria-label": aria, onclick }, [UI.icon(iconName), el("span", { text: label })]);
  }
  function renderPagesInner() {
    const list = document.getElementById("pages-list");
    list.replaceChildren();
    const items = pages.list();
    document.getElementById("pages-empty").classList.toggle("panel-hidden", items.length > 0);

    items.forEach((p) => {
      const actions = [];
      if (p.state === "running") {
        actions.push(btn("pause", "Pause", "Pause " + p.name, p.id + ":primary", "outline", () => { pages.pause(p.id); Shell.announce(p.name + " paused"); }));
      } else {
        actions.push(btn("play", "Resume", "Resume " + p.name, p.id + ":primary", "outline", () => {
          const r = pages.resume(p.id);
          if (r.ok) Shell.announce(p.name + " resumed");
          else { Shell.showToast("Not enough simulated memory to resume " + p.name, { tone: "warn" }); Shell.announce("Low memory. Cannot resume " + p.name + ". Save paused pages first."); }
        }));
      }
      if (p.state === "paused") {
        actions.push(btn("vault", "Save", "Save " + p.name + " and free memory", p.id + ":save", "outline", () => { pages.save(p.id); Shell.announce(p.name + " saved"); }));
      }
      actions.push(el("button", { class: "btn ghost btn-icon", type: "button", "data-fk": p.id + ":close", "aria-label": "Close " + p.name, title: "Close", onclick: () => { pages.close(p.id); Shell.announce(p.name + " closed"); } }, [UI.icon("close")]));

      const app = Shell.app(p.appId);
      const chip = UI.chip(STATE_LABEL[p.state], p.state === "running" ? "ok" : p.state === "paused" ? "warn" : "muted", p.state === "running" ? "play" : p.state === "paused" ? "pause" : "vault");
      chip.classList.add("state-badge", "state-" + p.state);
      const open = () => { if (app && app.screen) Shell.openApp(p.appId); else Shell.showToast(p.name + " has no screen in this prototype yet.", { tone: "info" }); };
      list.appendChild(el("article", { class: "card opaque page-card", role: "listitem", "data-page": p.id, "data-state": p.state }, [
        el("div", { class: "page-head" }, [
          UI.appTile(app ? app.icon : "info", app ? app.hue : 215),
          el("h3", { class: "page-title" }, [el("button", { type: "button", class: "page-open", "data-fk": p.id + ":open", "aria-label": "Open " + p.name, onclick: open, text: p.name })]),
          chip,
        ]),
        el("p", { class: "page-meta", text: spaceName(p.space) + " Space · " + (p.state === "saved" ? "0 MB in RAM (" + p.memMB + " MB when resumed)" : p.memMB + " MB") }),
        el("p", { class: "page-hint", text: p.state === "running" ? "Using simulated memory. Pause it to keep your place." : p.state === "paused" ? "Still holding its memory. Save it to free the memory." : "Frozen to storage. Resuming needs " + p.memMB + " MB." }),
        el("div", { class: "page-actions" }, actions),
      ]));
    });

    const u = pages.usage();
    const pct = Math.round(u.ratio * 100);
    document.getElementById("pages-memory-text").textContent = u.usedMB + " / " + u.budgetMB + " MB";
    const meter = document.getElementById("pages-meter");
    meter.setAttribute("aria-valuenow", String(pct));
    meter.setAttribute("aria-valuetext", pct + " percent used" + (u.low ? ", low memory" : ""));
    const fill = document.getElementById("pages-meter-fill");
    fill.style.width = Math.min(100, pct) + "%";
    fill.classList.toggle("low", u.low);
    document.getElementById("pages-lowmem").classList.toggle("panel-hidden", !u.low);
    document.getElementById("btn-pages-free").disabled = !items.some((p) => p.state === "paused");
  }

  // ---------------- Quick Center ----------------
  // note: what the toggle really does in this prototype
  const TILES = [
    { key: "wifi", label: "Wi-Fi", icon: "wifi", note: "Simulated" },
    { key: "bluetooth", label: "Bluetooth", icon: "bluetooth", note: "Simulated" },
    { key: "batterySaver", label: "Battery saver", icon: "battery", note: "Uses Clear glass" },
    { key: "focus", label: "Focus mode", icon: "focus", note: "Shown on Home" },
    { key: "rotationLock", label: "Rotation lock", icon: "rotate", note: "Simulated" },
  ];
  let tierBeforeSaver = null;

  function renderQuick() { keepFocus(document.getElementById("screen-quick"), renderQuickInner); }
  function renderQuickInner() {
    const v = qs.all();
    document.getElementById("qc-brightness").value = v.brightness;
    document.getElementById("qc-brightness-out").textContent = v.brightness + "%";
    document.getElementById("qc-volume").value = v.volume;
    document.getElementById("qc-volume-out").textContent = v.volume === 0 ? "Muted" : v.volume + "%";
    document.getElementById("qc-appearance").value = v.appearance;
    document.getElementById("qc-aura-mode").value = v.auraMode;
    document.documentElement.style.setProperty("--proto-dim", String((100 - v.brightness) / 100 * 0.5));

    const tiles = document.getElementById("qc-tiles");
    tiles.replaceChildren();
    TILES.forEach((t) => {
      const on = !!v[t.key];
      tiles.appendChild(el("button", {
        class: "tile" + (on ? " on" : ""), type: "button", "data-setting": t.key, "data-fk": "tile:" + t.key,
        "aria-pressed": String(on),
        onclick: () => { qs.toggle(t.key); Shell.announce(t.label + (qs.get(t.key) ? " on" : " off")); },
      }, [
        UI.icon(t.icon),
        el("span", { class: "tile-label", text: t.label }),
        el("span", { class: "tile-state", text: on ? "On" : "Off" }),
        el("span", { class: "tile-note", text: t.note }),
      ]));
    });

    const priv = qs.privacy(sessions);
    const names = { microphone: "Microphone", camera: "Camera", location: "Location" };
    const glyph = { microphone: "mic", camera: "camera", location: "location" };
    const list = document.getElementById("qc-privacy");
    list.replaceChildren();
    const sim = document.getElementById("qc-privacy-sim");
    sim.replaceChildren();
    Object.keys(names).forEach((k) => {
      list.appendChild(el("li", { class: "privacy-item" + (priv[k] ? " active" : ""), "data-sensor": k },
        [UI.icon(glyph[k]), el("span", { text: names[k] + ": " + (priv[k] ? "in use (simulated)" : "not in use") })]));
      sim.appendChild(el("button", { class: "btn outline", type: "button", "data-fk": "sim:" + k, "aria-pressed": String(priv[k]),
        text: (priv[k] ? "Stop " : "Simulate ") + names[k].toLowerCase(),
        onclick: () => { sessions[k] = !sessions[k]; renderQuick(); Shell.announce(names[k] + (sessions[k] ? " in use, simulated" : " not in use")); } }));
    });
  }

  qs.onChange((key, value) => {
    if (key === "appearance") Shell.setAppearance(value);
    if (key === "batterySaver") {
      if (value) { tierBeforeSaver = Shell.state.tier; Shell.setTier("clear"); }
      else if (tierBeforeSaver) { Shell.setTier(tierBeforeSaver); tierBeforeSaver = null; }
    }
    renderQuick();
  });

  function wireQuick() {
    document.getElementById("qc-brightness").addEventListener("input", (e) => qs.set("brightness", Number(e.target.value)));
    document.getElementById("qc-volume").addEventListener("input", (e) => qs.set("volume", Number(e.target.value)));
    document.getElementById("qc-appearance").addEventListener("change", (e) => qs.set("appearance", e.target.value));
    document.getElementById("qc-aura-mode").addEventListener("change", (e) => qs.set("auraMode", e.target.value));
  }

  // ---------------- Wiring ----------------
  function init() {
    pages.onChange(renderPages);
    wireQuick();
    document.getElementById("btn-pages-free").addEventListener("click", () => {
      const r = pages.freeMemory();
      Shell.announce("Saved paused pages. " + r.freedMB + " megabytes freed (simulated).");
    });
    Shell.onScreenShow((id) => {
      if (id === "screen-pages") renderPages();
      if (id === "screen-quick") {
        if (qs.get("appearance") !== Shell.state.theme) qs.set("appearance", Shell.state.theme);
        renderQuick();
      }
    });
    renderPages();
    renderQuick();
  }

  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", init);
  else init();
})();
