/* aurora-glass/prototype/calculator.js - the Calculator screen (window.AuroraCalculator).
 *
 * SIMULATION ONLY. A browser-prototype app over the DOM-free engine (calc-engine.js): nothing is saved, nothing is native, and it never
 * touches AuroraSystem.world, the broker or storage. Keys act only while this screen is active and focus is not in another text control.
 * All text is inserted with textContent (AuroraUI.el).
 */
(function () {
  "use strict";
  const UI = window.AuroraUI, Shell = window.AuroraShell, E = window.AuroraCalcEngine;
  if (!UI || !Shell || !E) return;
  const { el, icon } = UI;

  const screen = Shell.addScreen("screen-calculator", "Calculator");
  const HISTORY_MAX = 10;
  let state = E.create(), history = [], lastEq = 0;
  let resultEl, exprEl, listEl, histCard, countEl;

  // Keypad, row by row. kind: d digit, o operator, f function, e equals. id: stable element id suffix.
  const PAD = [
    [{ k: "clear", t: "C", n: "clear", kind: "f", label: "Clear" }, { k: "backspace", t: "⌫", n: "backspace", kind: "f", label: "Backspace" }, { k: "percent", t: "%", n: "percent", kind: "f", label: "Percent" }, { k: "op", a: "/", t: "÷", n: "divide", kind: "o", label: "Divide" }],
    [{ k: "digit", a: "7", t: "7", n: "7", kind: "d" }, { k: "digit", a: "8", t: "8", n: "8", kind: "d" }, { k: "digit", a: "9", t: "9", n: "9", kind: "d" }, { k: "op", a: "*", t: "×", n: "multiply", kind: "o", label: "Multiply" }],
    [{ k: "digit", a: "4", t: "4", n: "4", kind: "d" }, { k: "digit", a: "5", t: "5", n: "5", kind: "d" }, { k: "digit", a: "6", t: "6", n: "6", kind: "d" }, { k: "op", a: "-", t: "−", n: "subtract", kind: "o", label: "Subtract" }],
    [{ k: "digit", a: "1", t: "1", n: "1", kind: "d" }, { k: "digit", a: "2", t: "2", n: "2", kind: "d" }, { k: "digit", a: "3", t: "3", n: "3", kind: "d" }, { k: "op", a: "+", t: "+", n: "add", kind: "o", label: "Add" }],
    [{ k: "sign", t: "±", n: "sign", kind: "f", label: "Change sign" }, { k: "digit", a: "0", t: "0", n: "0", kind: "d" }, { k: "dot", t: ".", n: "dot", kind: "d", label: "Decimal point" }, { k: "equals", t: "=", n: "equals", kind: "e", label: "Equals" }],
  ];

  function press(name, arg) {
    state = E.input(state, name, arg);
    if (state.eq !== lastEq) {                                  // a calculation just completed: record it (errors never reach here)
      lastEq = state.eq;
      if (state.last && !state.error) { history.unshift({ expr: E.expression(state), result: E.display(state) }); if (history.length > HISTORY_MAX) history.pop(); }
    }
    update();
  }

  const FIT = ["", "len-m", "len-s", "len-xs", "len-xxs"];
  /** Shrink the result until the WHOLE text fits (a clipped exponent would hide digits); at the smallest size it scrolls as a last resort. */
  function fit() {
    if (!resultEl || !resultEl.clientWidth) return;                          // not laid out (screen hidden): the next show fits it
    const base = state.error ? " is-error" : "";
    for (const c of FIT) {
      resultEl.className = "calc-result " + c + base;
      if (state.error || resultEl.scrollWidth <= resultEl.clientWidth) break;
    }
  }

  function update() {
    if (!resultEl) return;
    const text = E.display(state);
    resultEl.textContent = text;
    fit();
    exprEl.textContent = E.expression(state);
    exprEl.title = exprEl.textContent;
    renderHistory();
  }

  function renderHistory() {
    UI.clear(listEl);
    if (!history.length) listEl.appendChild(el("li", { class: "calc-hist-empty row-meta", text: "Completed calculations appear here. Tap one to use its result." }));
    history.forEach((h, i) => {
      listEl.appendChild(el("li", {}, [el("button", { type: "button", class: "calc-hist-item", id: "btn-calc-hist-" + i, "data-hist": String(i), "aria-label": h.expr + " " + h.result + ". Use this result.", onclick: () => reuse(i) }, [
        el("span", { class: "calc-hist-expr", text: h.expr }), el("span", { class: "calc-hist-result", text: h.result })]) ]));
    });
    countEl.textContent = history.length + (history.length === 1 ? " calculation" : " calculations");
  }

  function reuse(i) {
    const h = history[i]; if (!h) return;
    state = Object.assign(E.load(h.result), { eq: lastEq });    // the counter keeps counting so reusing a result is not recorded again
    update();
    const r = screen.querySelector("#calc-result"); if (r && r.focus) r.focus({ preventScroll: true });
  }

  function key(spec) {
    return el("button", {
      type: "button", class: "calc-key calc-" + spec.kind, id: "btn-calc-" + spec.n, "data-calc": spec.n,
      "aria-label": spec.label || undefined, onclick: () => press(spec.k, spec.a),
    }, [el("span", { text: spec.t, "aria-hidden": spec.label ? "true" : undefined })]);
  }

  function build() {
    UI.clear(screen);
    screen.appendChild(el("header", { class: "screen-header" }, [
      el("button", { type: "button", class: "btn btn-icon ghost", id: "btn-calc-back", "aria-label": "Back", title: "Back", onclick: () => Shell.back() }, [icon("back")]),
      el("div", { class: "screen-heading" }, [el("p", { class: "eyebrow", text: "Pocket calculator" }), el("h1", { class: "screen-title", text: "Calculator" })]),
    ]));
    screen.appendChild(el("p", { class: "callout", id: "calc-banner" }, [icon("info"), el("span", { text: "Browser prototype. Nothing is saved and nothing leaves this page." })]));
    exprEl = el("p", { class: "calc-expr", id: "calc-expr", "aria-hidden": "true" });
    resultEl = el("output", { class: "calc-result", id: "calc-result", "aria-live": "polite", "aria-atomic": "true", tabindex: "-1", "aria-label": "Result" });
    const display = el("div", { class: "card opaque calc-display", id: "calc-display" }, [exprEl, resultEl]);
    const pad = el("div", { class: "calc-pad", id: "calc-pad", role: "group", "aria-label": "Keypad" }, PAD.map((row) => el("div", { class: "calc-row" }, row.map(key))));
    listEl = el("ul", { class: "calc-history-list", id: "calc-history", "aria-label": "History" });
    countEl = el("span", { class: "row-meta", id: "calc-history-count" });
    histCard = el("section", { class: "card opaque calc-history", id: "calc-history-card", "aria-label": "History" }, [
      el("div", { class: "calc-history-head" }, [el("h2", { class: "calc-history-title", text: "History" }), countEl,
        el("button", { type: "button", class: "btn ghost btn-small", id: "btn-calc-hist-clear", onclick: () => { history = []; update(); } }, [el("span", { text: "Clear history" })])]),
      listEl]);
    screen.appendChild(el("div", { class: "calc-layout" }, [el("div", { class: "calc-main" }, [display, pad]), histCard]));
    update();
  }

  // ---------------- keyboard: only while this screen is active and focus is on the page, the display or the keypad ----------------
  const live = () => screen.classList.contains("active");
  function mapKey(e) {
    const k = e.key;
    if (/^[0-9]$/.test(k)) return ["digit", k];
    if (k === "." || k === ",") return ["dot"];
    if (k === "+" || k === "-" || k === "*" || k === "/") return ["op", k];
    if (k === "Enter" || k === "=") return ["equals"];
    if (k === "Backspace") return ["backspace"];
    if (k === "Escape") return ["clear"];
    if (k === "Delete") return ["clearEntry"];
    if (k === "%") return ["percent"];
    if (k === "F9" || k === "_") return ["sign"];
    return null;
  }
  function onKey(e) {
    if (!live() || e.defaultPrevented || e.ctrlKey || e.metaKey || e.altKey) return;
    const t = e.target;
    if (t && t.matches && t.matches("input, textarea, select, [contenteditable='true'], dialog *, .dialog *")) return;
    const inside = t && t.closest ? t.closest("#calc-pad, #calc-display, #calc-result") : null;
    const control = t && t.closest ? t.closest("button, a[href], input, select, textarea, summary, [role='button'], [role='tab'], [role='menuitem'], [contenteditable='true']") : null;
    if (control && !inside) return;                                                          // focus is on Back, History, the Dock...: those keep their own behaviour (a focused heading or the page itself does not)
    const m = mapKey(e);
    if (!m) return;
    e.preventDefault();                                                                       // also stops Enter from clicking the focused key a second time
    press(m[0], m[1]);
  }
  document.addEventListener("keydown", onKey);

  window.addEventListener("resize", fit);
  Shell.onScreenShow((id) => { if (id === "screen-calculator") update(); });
  build();

  window.AuroraCalculator = {
    state: () => ({ display: E.display(state), expression: E.expression(state), error: state.error, history: history.map((h) => ({ expr: h.expr, result: h.result })) }),
    press, reset() { state = E.create(); history = []; lastEq = 0; update(); },
  };
})();
