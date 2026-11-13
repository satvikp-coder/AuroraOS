/* aurora-glass/prototype/welcome.js - the first-run welcome (window.AuroraWelcome).
 *
 * SIMULATION ONLY. Three short, skippable steps that introduce the browser prototype. Session-only: "seen" lives in sessionStorage (this tab), nothing is
 * stored permanently. It never blocks the interface for long (Skip and Escape end it), never opens over another dialog, and every sentence describes the
 * prototype truthfully: nothing here uses real cellular, audio, camera, location or any hardware.
 * Automatic start: real visitors get it once per tab session. Browser automation (navigator.webdriver) does not, so tests stay deterministic;
 * ?welcome=1 forces it, ?welcome=auto applies the real-visitor rules even under automation, ?welcome=off disables it.
 */
(function () {
  "use strict";
  const UI = window.AuroraUI, Shell = window.AuroraShell;
  if (!UI || !Shell) return;
  const { el } = UI;

  const SEEN_KEY = "aurora-glass.welcome.seen";
  const STEPS = [
    { id: "spaces", title: "Spaces keep your day organised", body: "Aurora Glass groups what you do into Spaces: Study, Work, Projects and Personal, plus Spaces you create.",
      points: ["Each Space has its own apps and open pages.", "Switch Space from Home or with the Spaces button in the Dock."] },
    { id: "apps", title: "Apps, Pages and getting around", body: "Open an app from Home, the Dock or Index. Press Ctrl+K (Cmd+K on a Mac) to search.",
      points: ["Every open app is a Page. Pages shows what is running and lets you pause or close it.", "Back, or the Escape key, always goes one step back."] },
    { id: "aura", title: "Aura asks before it acts", body: "Aura can propose actions, such as starting a timer or moving a file. You review each one before anything happens.",
      points: ["Passport shows what each app is allowed to do.", "Trail records every action and lets you undo it.", "Deleting needs an extra press-and-hold to confirm."] },
  ];
  const NOTE = "This is a browser prototype. Everything is simulated here: no cellular, audio, camera, location or other phone hardware is used, and Notes are saved in this browser only.";

  let isOpen = false, step = 0;

  function mode() { try { return new URLSearchParams(location.search).get("welcome"); } catch (e) { return null; } }
  function seen() { try { return sessionStorage.getItem(SEEN_KEY) === "1"; } catch (e) { return false; } }
  function markSeen() { try { sessionStorage.setItem(SEEN_KEY, "1"); } catch (e) { /* private mode: it may show again after a reload; acceptable */ } }
  const otherDialogOpen = () => !!document.querySelector(".scrim");

  /** Show the tour now. Returns false when one is already open or another dialog is up (it never stacks over a review or permission dialog). */
  function show() {
    if (isOpen || otherDialogOpen()) return false;
    isOpen = true; step = 0; markSeen();
    const dots = el("span", { class: "welcome-dots", "aria-hidden": "true" });
    const label = el("p", { class: "welcome-step-label", id: "welcome-step-label" });
    const heading = el("h3", { class: "welcome-heading", id: "welcome-heading" });
    const text = el("p", { class: "welcome-body", id: "welcome-text" });
    const points = el("ul", { class: "welcome-points", id: "welcome-points" });
    const back = el("button", { type: "button", class: "btn outline", id: "btn-welcome-back", text: "Back" });
    const skip = el("button", { type: "button", class: "btn ghost", id: "btn-welcome-skip", text: "Skip tour" });
    const next = el("button", { type: "button", class: "btn accent", id: "btn-welcome-next", text: "Next" });
    const body = el("div", { class: "welcome", id: "welcome" }, [
      el("div", { class: "welcome-progress" }, [label, dots]), heading, text, points, el("p", { class: "welcome-note", id: "welcome-note", text: NOTE }),
      el("div", { class: "welcome-actions" }, [skip, el("div", { class: "welcome-nav" }, [back, next])]),
    ]);
    let api = null;
    function render(announce) {
      const s = STEPS[step], last = step === STEPS.length - 1;
      label.textContent = "Step " + (step + 1) + " of " + STEPS.length;
      heading.textContent = s.title; text.textContent = s.body;
      UI.clear(points); s.points.forEach((p) => points.appendChild(el("li", { text: p })));
      UI.clear(dots); STEPS.forEach((_, i) => dots.appendChild(el("span", { class: "welcome-dot" + (i === step ? " is-current" : i < step ? " is-done" : "") })));
      back.hidden = step === 0; next.textContent = last ? "Finish" : "Next"; skip.hidden = last;
      if (announce) UI.announce("Step " + (step + 1) + " of " + STEPS.length + ": " + s.title);
    }
    next.addEventListener("click", () => { if (step < STEPS.length - 1) { step++; render(true); } else api.close("finish"); });
    back.addEventListener("click", () => { if (step > 0) { step--; render(true); next.focus(); } });
    skip.addEventListener("click", () => api.close("skip"));
    render(false);
    UI.dialog({ id: "welcome-dialog", title: "Welcome to Aurora Glass", icon: "home", body, actions: [], sticky: true, dismissValue: "skip", initialFocus: "#btn-welcome-next",
      onOpen: (a) => { api = a; } }).then(() => { isOpen = false; });
    return true;
  }

  /** The automatic path: wait (briefly) while another dialog is open, then show once. Resolves true if shown. */
  function request() {
    return new Promise((resolve) => {
      let tries = 0;
      (function attempt() {
        if (isOpen) return resolve(false);
        if (!otherDialogOpen()) return resolve(show());
        if (++tries > 25) return resolve(false);                             // about 10 s of another dialog: give up rather than nag
        setTimeout(attempt, 400);
      })();
    });
  }

  function init() {
    const m = mode();
    if (m === "off") return;
    if (m === "1") { setTimeout(request, 250); return; }
    const automated = !!navigator.webdriver && m !== "auto";
    if (automated || seen()) return;
    setTimeout(request, 600);
  }

  window.AuroraWelcome = { show, request, isOpen: () => isOpen, steps: () => STEPS.map((s) => s.id) };
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", init); else init();
})();
