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
  function renderPagesInner() {
    const list = document.getElementById("pages-list");
    list.replaceChildren();
    const items = pages.list();
    document.getElementById("pages-empty").classList.toggle("panel-hidden", items.length > 0);

    items.forEach((p) => {
      const actions = [];
      if (p.state === "running") {
        actions.push(el("button", { class: "btn outline", type: "button", "data-fk": p.id + ":primary", text: "Pause", "aria-label": "Pause " + p.name,
          onclick: () => { pages.pause(p.id); Shell.announce(p.name + " paused"); } }));
      } else {
        actions.push(el("button", { class: "btn accent", type: "button", "data-fk": p.id + ":primary", text: "Resume", "aria-label": "Resume " + p.name,
          onclick: () => {
            const r = pages.resume(p.id);
            if (r.ok) Shell.announce(p.name + " resumed");
            else { Shell.showToast("Not enough simulated memory to resume " + p.name); Shell.announce("Low memory. Cannot resume " + p.name + ". Save paused pages first."); }
          } }));
      }
      if (p.state === "paused") {
        actions.push(el("button", { class: "btn outline", type: "button", "data-fk": p.id + ":save", text: "Save", "aria-label": "Save " + p.name + " and free memory",
          onclick: () => { pages.save(p.id); Shell.announce(p.name + " saved"); } }));
      }
      actions.push(el("button", { class: "btn outline", type: "button", "data-fk": p.id + ":close", text: "Close", "aria-label": "Close " + p.name,
        onclick: () => { pages.close(p.id); Shell.announce(p.name + " closed"); } }));

      list.appendChild(el("article", { class: "card page-card", role: "listitem", "data-page": p.id, "data-state": p.state }, [
        el("div", { class: "page-head" }, [
          el("h3", { class: "section-title", text: p.name }),
          el("span", { class: "state-badge state-" + p.state, text: STATE_LABEL[p.state] }),
        ]),
        el("p", { class: "page-meta", text: spaceName(p.space) + " Space · " + (p.state === "saved" ? "0 MB in RAM (" + p.memMB + " MB when resumed)" : p.memMB + " MB") }),
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
  const TILES = [
    { key: "wifi", label: "Wi-Fi" },
    { key: "bluetooth", label: "Bluetooth" },
    { key: "batterySaver", label: "Battery saver" },
    { key: "focus", label: "Focus mode" },
    { key: "rotationLock", label: "Rotation lock" },
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
        el("span", { class: "tile-label", text: t.label }),
        el("span", { class: "tile-state", text: on ? "On" : "Off" }),
      ]));
    });

    const priv = qs.privacy(sessions);
    const names = { microphone: "Microphone", camera: "Camera", location: "Location" };
    const list = document.getElementById("qc-privacy");
    list.replaceChildren();
    const sim = document.getElementById("qc-privacy-sim");
    sim.replaceChildren();
    Object.keys(names).forEach((k) => {
      list.appendChild(el("li", { class: "privacy-item" + (priv[k] ? " active" : ""), "data-sensor": k },
        [el("span", { text: names[k] + ": " + (priv[k] ? "in use (simulated)" : "not in use") })]));
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
