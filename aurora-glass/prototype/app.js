/* aurora-glass/app.js — Aurora Glass prototype interaction logic
 * Simulated host-browser demo only. This prototype never controls a real OS
 * or device. State is mock; all Aura operations are simulated.
 */

(function () {
  const root = document.documentElement;
  const app = document.getElementById("app");
  const content = document.getElementById("content");
  const live = document.getElementById("a11y-live");
  const toastEl = document.getElementById("toast");

  // ---------- Mock state ----------
  const state = {
    theme: "light",
    tier: "soft",
    contrast: "normal",
    width: "auto",
    motion: "normal",
    activeScreen: "screen-home",
    activeSpace: "study",
    auraFlow: "success", // success | failure
    planStep: 0,
    planRunning: false,
    planComplete: false,
    planFailed: false,
    indexQuery: "",
  };

  const spaces = [
    { id: "study", name: "Study", accent: "var(--color-accent)" },
    { id: "work", name: "Work", accent: "var(--color-accent)" },
    { id: "projects", name: "Projects", accent: "var(--color-aura)" },
    { id: "personal", name: "Personal", accent: "var(--color-aura)" },
  ];

  const mockIndex = [
    { kind: "App", label: "Notes", meta: "Open notes" },
    { kind: "App", label: "Files", meta: "Browse files" },
    { kind: "App", label: "Calculator", meta: "Calculate" },
    { kind: "App", label: "Clock", meta: "Timer & alarm" },
    { kind: "Setting", label: "Display", meta: "Brightness, dark mode" },
    { kind: "Setting", label: "Passport", meta: "Permissions & access" },
    { kind: "Action", label: "Quick Center", meta: "Tiles & sliders" },
    { kind: "Action", label: "Vault export", meta: "Export user files" },
  ];

  // ---------- Helpers ----------
  function announce(text) {
    live.textContent = "";
    live.textContent = text;
  }

  function showToast(text, ms = 2500) {
    toastEl.textContent = text;
    toastEl.classList.add("show");
    setTimeout(() => toastEl.classList.remove("show"), ms);
  }

  function hideAllScreens() {
    document.querySelectorAll("#app .screen").forEach((s) => {
      s.classList.remove("active");
    });
  }

  function showScreen(id) {
    hideAllScreens();
    const el = document.getElementById(id);
    if (!el) return;
    el.classList.add("active");
    state.activeScreen = id;
    announce(`Screen changed: ${el.getAttribute("aria-label") || id}`);
    // Move focus to first focusable element
    const first = el.querySelector("[tabindex], button, a, input");
    if (first) first.focus();
    window.scrollTo(0, 0);
  }

  function applyTheme() {
    root.setAttribute("data-theme", state.theme);
    root.setAttribute("data-glass-tier", state.tier);
    root.setAttribute("data-contrast", state.contrast);
    // Reduced motion via class on root (CSS media handles prefers-reduced-motion;
    // we also force it via inline style when "reduce" selected)
    if (state.motion === "reduce") {
      root.style.setProperty("--motion-fast", "0ms");
      root.style.setProperty("--motion-base", "0ms");
      root.style.setProperty("--motion-slow", "0ms");
    } else {
      root.style.removeProperty("--motion-fast");
      root.style.removeProperty("--motion-base");
      root.style.removeProperty("--motion-slow");
    }
    // Dock active state
    document.querySelectorAll(".dock-btn").forEach((b) => {
      const target = b.getAttribute("data-target");
      b.classList.toggle("active", target === state.activeScreen);
    });
    showToast(`${state.theme} · ${state.tier} · contrast ${state.contrast}`);
  }

  function setPreviewWidth() {
    const shell = document.getElementById("app");
    shell.style.maxWidth = state.width === "auto" ? "" : state.width + "px";
    shell.style.margin = state.width === "auto" ? "0" : "0 auto";
    announce(`Preview width set to ${state.width === "auto" ? "auto" : state.width + "px"}`);
  }

  // ---------- Spaces ----------
  function renderSpaces() {
    const grid = document.querySelector(".space-grid");
    if (!grid) return;
    grid.innerHTML = "";
    spaces.forEach((sp) => {
      const art = document.createElement("article");
      art.className = "space-card-item" + (sp.id === state.activeSpace ? " selected" : "");
      art.setAttribute("role", "listitem");
      art.setAttribute("tabindex", "0");
      art.setAttribute("data-space", sp.id);
      art.setAttribute(
        "aria-label",
        sp.name + " Space — " + (sp.id === state.activeSpace ? "active" : "inactive")
      );
      art.innerHTML = `
        <div class="space-badge">${sp.name.charAt(0)}</div>
        <h3>${sp.name}</h3>
        <p>${sp.id === "study" ? "Notes, focus timer, reading list" : sp.id === "work" ? "Projects, files, meetings" : sp.id === "projects" ? "Active builds, drafts, reviews" : "Journal, photos, reminders"}</p>
        <span class="space-state">${sp.id === state.activeSpace ? "Active" : "Inactive"}</span>
      `;
      art.addEventListener("click", () => switchSpace(sp.id));
      art.addEventListener("keydown", (e) => {
        if (e.key === "Enter" || e.key === " ") { e.preventDefault(); switchSpace(sp.id); }
      });
      grid.appendChild(art);
    });
    // Update space chip on home
    const chip = document.querySelector(".space-chip");
    if (chip) {
      chip.dataset.space = state.activeSpace;
      chip.classList.toggle("selected", true);
      const label = chip.querySelector("span:last-child");
      if (label) label.textContent = spaces.find((s) => s.id === state.activeSpace).name;
    }
  }

  function switchSpace(id) {
    state.activeSpace = id;
    renderSpaces();
    announce("Switched to " + spaces.find((s) => s.id === id).name + " Space");
    showToast("Space switched: " + id);
  }

  // ---------- Index ----------
  function renderIndex() {
    const box = document.getElementById("index-results");
    const input = document.getElementById("index-query");
    if (!box) return;
    const q = state.indexQuery.toLowerCase();
    box.innerHTML = "";
    const results = mockIndex.filter((i) => i.label.toLowerCase().includes(q) || i.meta.toLowerCase().includes(q));
    box.setAttribute("aria-expanded", results.length > 0 ? "true" : "false");
    results.forEach((item) => {
      const div = document.createElement("div");
      div.className = "index-result";
      div.setAttribute("role", "option");
      div.setAttribute("tabindex", "0");
      div.setAttribute("aria-label", item.kind + ": " + item.label + ", " + item.meta);
      div.innerHTML = `<strong>${item.label}</strong><span class="meta">${item.kind} &middot; ${item.meta}</span>`;
      div.addEventListener("click", () => selectIndex(item));
      div.addEventListener("keydown", (e) => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); selectIndex(item); } });
      box.appendChild(div);
    });
    input.setAttribute("aria-controls", results.length ? box.id : "");
    announce(results.length + " results");
  }

  function selectIndex(item) {
    announce("Selected " + item.label);
    showToast("Opened " + item.label + " (simulated)");
    showScreen("screen-home");
  }

  // ---------- Aura plan ----------
  function renderPlan() {
    const requestText = document.getElementById("plan-request-text");
    if (requestText) requestText.textContent = state.auraFlow === "failure"
      ? "\"Prepare my phone for studying.\" (step 3 simulated failure)"
      : "\"Prepare my phone for studying.\"";
  }

  let planTimer = null;

  function startPlan() {
    if (planTimer) clearInterval(planTimer);
    state.planRunning = true;
    state.planComplete = false;
    state.planFailed = false;
    state.planStep = 1;
    hideAllScreens();
    const sheet = document.getElementById("screen-aura");
    sheet.classList.add("active");
    // Hide all panels
    ["plan-confirmation", "plan-running", "plan-receipt", "plan-failure"].forEach((id) => {
      document.getElementById(id).classList.add("panel-hidden");
    });
    document.getElementById("plan-running").classList.remove("panel-hidden");
    updateRunningUI();
    announce("Plan running. Step 1 of 4: switching to Study Space.");

    // Auto-advance through plan steps (~600ms each)
    planTimer = setInterval(() => {
      advancePlan();
    }, 600);
  }

  function updateRunningUI() {
    const cur = document.getElementById("running-current");
    const tot = document.getElementById("running-total");
    const lbl = document.getElementById("running-label");
    if (cur) cur.textContent = state.planStep;
    if (tot) tot.textContent = "4";
    const labels = [
      "Switching to Study Space…",
      "Enabling focus mode…",
      "Opening Notes…",
      "Starting 25-minute timer…",
    ];
    if (lbl) lbl.textContent = labels[state.planStep - 1] || "";
    announce("Step " + state.planStep + " of 4: " + (labels[state.planStep - 1] || ""));
  }

  function advancePlan() {
    if (!state.planRunning) return;
    state.planStep++;
    if (state.planStep > 4) {
      finishPlan();
      return;
    }
    updateRunningUI();
  }

  function finishPlan() {
    if (planTimer) clearInterval(planTimer);
    planTimer = null;
    state.planRunning = false;
    state.planComplete = true;
    document.getElementById("plan-running").classList.add("panel-hidden");
    if (state.auraFlow === "failure") {
      state.planFailed = true;
      document.getElementById("plan-failure").classList.remove("panel-hidden");
      document.getElementById("failure-text").textContent = "Step 3 (Open Notes) could not complete — simulated failure.";
      announce("Plan failed at step 3. Repair, skip, or stop.");
    } else {
      document.getElementById("plan-receipt").classList.remove("panel-hidden");
      announce("Plan complete. All 4 steps succeeded.");
    }
  }

  // ---------- Navigation wiring ----------
  function wireNav() {
    const go = (btnId, target) => {
      const b = document.getElementById(btnId);
      if (!b) return;
      b.addEventListener("click", () => showScreen(target));
    };
    go("dock-home", "screen-home");
    go("dock-back", "screen-home");
    go("dock-spaces", "screen-spaces");
    go("dock-pages", "screen-home");
    go("dock-ask", "screen-index");
    go("btn-index-back", "screen-home");
    go("btn-spaces-back", "screen-home");
    go("btn-aura-back", "screen-home");

    // Spaces chip / button
    const spaceBtn = document.getElementById("btn-switch-space");
    if (spaceBtn) spaceBtn.addEventListener("click", () => showScreen("spaces"));

    // Space cards keyboard
    document.querySelectorAll(".space-card-item").forEach((c) => {
      c.addEventListener("click", () => switchSpace(c.dataset.space));
      c.addEventListener("keydown", (e) => {
        if (e.key === "Enter" || e.key === " ") { e.preventDefault(); switchSpace(c.dataset.space); }
      });
    });

    // Index search
    const input = document.getElementById("index-query");
    if (input) {
      input.addEventListener("input", (e) => {
        state.indexQuery = e.target.value;
        renderIndex();
      });
      input.addEventListener("keydown", (e) => {
        if (e.key === "Enter") { e.preventDefault(); const first = document.querySelector("#index-results .index-result"); if (first) first.click(); }
        if (e.key === "Escape") showScreen("screen-home");
      });
    }
    const clear = document.getElementById("btn-index-clear");
    if (clear) clear.addEventListener("click", () => { input.value = ""; state.indexQuery = ""; renderIndex(); });

    // Aura study button on home
    const btnAura = document.getElementById("btn-aura-study");
    if (btnAura) btnAura.addEventListener("click", startPlan);

    // Plan: confirm -> confirmation panel
    const btnConfirm = document.getElementById("btn-plan-confirm");
    if (btnConfirm) btnConfirm.addEventListener("click", () => {
      document.getElementById("plan-confirmation").classList.remove("panel-hidden");
      announce("Confirm plan execution? 4 steps will run.");
    });
    const btnPlanCancel = document.getElementById("btn-plan-cancel");
    if (btnPlanCancel) btnPlanCancel.addEventListener("click", () => {
      document.getElementById("plan-confirmation").classList.add("panel-hidden");
      announce("Plan confirmation cancelled.");
    });
    const btnPlanRun = document.getElementById("btn-plan-run");
    if (btnPlanRun) btnPlanRun.addEventListener("click", startPlan);
    const btnPlanEdit = document.getElementById("btn-plan-edit");
    if (btnPlanEdit) btnPlanEdit.addEventListener("click", () => {
      document.querySelectorAll(".step-text").forEach((t) => { t.removeAttribute("readonly"); t.focus(); });
      announce("Plan steps are now editable. Select a field and type to change it.");
    });

    // Running panel
    const btnStop = document.getElementById("btn-plan-stop");
    if (btnStop) btnStop.addEventListener("click", () => {
      if (planTimer) clearInterval(planTimer);
      planTimer = null;
      state.planRunning = false;
      document.getElementById("plan-running").classList.add("panel-hidden");
      document.getElementById("plan-receipt").classList.remove("panel-hidden");
      document.getElementById("plan-receipt").querySelector("h3").textContent = "Plan stopped";
      document.getElementById("plan-receipt").querySelector("p.receipt-summary").textContent = "Plan was stopped by the user.";
      announce("Plan stopped by user.");
    });

    // Failure panel actions
    const btnRepair = document.getElementById("btn-failure-repair");
    if (btnRepair) btnRepair.addEventListener("click", () => { advancePlan(); document.getElementById("plan-failure").classList.add("panel-hidden"); document.getElementById("plan-running").classList.remove("panel-hidden"); announce("Repairing. Retrying step 3."); });
    const btnSkip = document.getElementById("btn-failure-skip");
    if (btnSkip) btnSkip.addEventListener("click", () => { advancePlan(); document.getElementById("plan-failure").classList.add("panel-hidden"); document.getElementById("plan-running").classList.remove("panel-hidden"); announce("Skipped failed step. Continuing."); });
    const btnStopFail = document.getElementById("btn-failure-stop");
    if (btnStopFail) btnStopFail.addEventListener("click", () => {
      if (planTimer) clearInterval(planTimer);
      planTimer = null;
      state.planRunning = false;
      document.getElementById("plan-failure").classList.add("panel-hidden");
      document.getElementById("plan-receipt").classList.remove("panel-hidden");
      document.getElementById("plan-receipt").querySelector("h3").textContent = "Plan stopped";
      document.getElementById("plan-receipt").querySelector("p.receipt-summary").textContent = "Plan stopped after a failure.";
      announce("Plan stopped after failure.");
    });
    const btnReceiptClose = document.getElementById("btn-receipt-close");
    if (btnReceiptClose) btnReceiptClose.addEventListener("click", () => {
      if (planTimer) clearInterval(planTimer);
      planTimer = null;
      state.planComplete = false;
      state.planFailed = false;
      state.planRunning = false;
      showScreen("screen-home");
      announce("Receipt closed. Back to Home.");
    });

    // Glass Corner
    const gc = document.querySelector(".glass-corner");
    if (gc) {
      gc.addEventListener("click", () => {
        const expanded = gc.getAttribute("aria-expanded") === "true";
        gc.setAttribute("aria-expanded", !expanded);
        announce(expanded ? "Glass Corner closed" : "Glass Corner opened — notifications");
      });
      gc.addEventListener("keydown", (e) => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); gc.click(); } });
    }
  }

  // ---------- Controls panel ----------
  function wireControls() {
    const panel = document.getElementById("controls-panel");
    // Open from dock? We'll add a settings entry. For now wire selects.
    const selTheme = document.getElementById("ctrl-theme");
    const selTier = document.getElementById("ctrl-tier");
    const selContrast = document.getElementById("ctrl-contrast");
    const selWidth = document.getElementById("ctrl-width");
    const selMotion = document.getElementById("ctrl-motion");
    const selFlow = document.getElementById("ctrl-aura-flow");
    const btnApply = document.getElementById("ctrl-apply");
    const btnReset = document.getElementById("ctrl-reset");
    const btnClose = document.getElementById("btn-controls-close");

    if (selTheme) selTheme.value = state.theme;
    if (selTier) selTier.value = state.tier;
    if (selContrast) selContrast.value = state.contrast;
    if (selWidth) selWidth.value = state.width;
    if (selMotion) selMotion.value = state.motion;
    if (selFlow) selFlow.value = state.auraFlow;

    if (btnApply) {
      btnApply.addEventListener("click", () => {
        state.theme = selTheme.value;
        state.tier = selTier.value;
        state.contrast = selContrast.value;
        state.width = selWidth.value;
        state.motion = selMotion.value;
        state.auraFlow = selFlow.value;
        applyTheme();
        setPreviewWidth();
        renderSpaces();
        renderPlan();
        panel.classList.add("panel-hidden");
        showScreen(state.activeScreen);
        announce("Controls applied.");
      });
    }
    if (btnReset) {
      btnReset.addEventListener("click", () => {
        state.theme = "light"; state.tier = "soft"; state.contrast = "normal";
        state.width = "auto"; state.motion = "normal"; state.auraFlow = "success";
        selTheme.value = "light"; selTier.value = "soft"; selContrast.value = "normal";
        selWidth.value = "auto"; selMotion.value = "normal"; selFlow.value = "success";
        applyTheme(); setPreviewWidth(); renderSpaces(); renderPlan();
        panel.classList.add("panel-hidden");
        showScreen(state.activeScreen);
        announce("Controls reset to defaults.");
      });
    }
    if (btnClose) btnClose.addEventListener("click", () => { panel.classList.add("panel-hidden"); });
  }

  // ---------- Keyboard global ----------
  function wireKeyboard() {
    document.addEventListener("keydown", (e) => {
      // Escape: close panels / modals / return home
      if (e.key === "Escape") {
        const confirmPanel = document.getElementById("plan-confirmation");
        if (confirmPanel && !confirmPanel.classList.contains("panel-hidden")) {
          confirmPanel.classList.add("panel-hidden");
          announce("Confirmation closed.");
          return;
        }
        if (planTimer) clearInterval(planTimer);
        planTimer = null;
        if (state.activeScreen !== "screen-home") { showScreen("screen-home"); return; }
      }
      // Ctrl+K / Cmd+K: open Index
      if ((e.ctrlKey || e.metaKey) && e.key === "k") {
        e.preventDefault();
        showScreen("screen-index");
        const input = document.getElementById("index-query");
        if (input) input.focus();
      }
    });
  }

  // ---------- Init ----------
  function init() {
    wireNav();
    wireControls();
    wireKeyboard();
    renderSpaces();
    renderPlan();
    applyTheme();
    setPreviewWidth();
    showScreen("screen-home");
    announce("Aurora Glass prototype ready. Home screen.");
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
  } else {
    init();
  }
})();
