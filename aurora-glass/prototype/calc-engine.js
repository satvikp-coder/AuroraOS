/* aurora-glass/prototype/calc-engine.js - DOM-free pocket-calculator state machine (window.AuroraCalcEngine).
 *
 * SIMULATION ONLY. A browser-prototype app: nothing is native, nothing is saved. Loadable in the browser and in Node for unit tests.
 *
 * - No eval, no Function, no parsing of arbitrary text: the only inputs are the named key presses below.
 * - Immediate-execution semantics like a pocket calculator (2 + 3 x 4 = 20, there is no operator precedence); repeated "=" repeats the last operation.
 * - Exact decimal arithmetic on BigInt coefficients (value = n x 10^e), so 0.1 + 0.2 = 0.3. Results are rounded half away from zero to
 *   12 significant digits; values that do not fit 12 plain digits are shown in exponent form (1.5e+15); beyond 1e+99 the state is the "Overflow" error.
 * - Division by zero is the error state "Can't divide by zero"; every input except Clear is ignored until Clear.
 * - A typed number is capped at 12 digits (the sign and the decimal point do not count).
 *
 * State is a plain object; input(state, name, arg) returns a NEW state and never mutates its argument.
 */
(function (root, factory) {
  if (typeof module === "object" && module.exports) module.exports = factory();
  else root.AuroraCalcEngine = factory();
})(typeof self !== "undefined" ? self : this, function () {
  "use strict";

  const MAX_DIGITS = 12, MAX_EXP = 99;
  const SYMBOL = Object.freeze({ "+": "+", "-": "−", "*": "×", "/": "÷" });
  const ALIAS = Object.freeze({ "−": "-", "–": "-", "x": "*", "X": "*", "×": "*", "÷": "/", ":": "/" });
  const ERRORS = Object.freeze({ div0: "Can't divide by zero", overflow: "Number too large" });
  const ZERO = Object.freeze({ n: 0n, e: 0 });

  // ---------------- decimals: value = n * 10^e, at most 12 significant digits, no trailing zeros ----------------
  /** Normalise and round to 12 significant digits (half away from zero). null = overflow (beyond 1e+99); underflow below 1e-99 becomes 0. */
  function make(n, e) {
    if (n === 0n) return ZERO;
    const neg = n < 0n; let a = neg ? -n : n;
    const d = a.toString().length;
    if (d > MAX_DIGITS) {
      const drop = d - MAX_DIGITS, p = 10n ** BigInt(drop);
      let q = a / p; if ((a % p) * 2n >= p) q += 1n;
      a = q; e += drop;
    }
    while (a % 10n === 0n) { a /= 10n; e += 1; }
    const x = a.toString().length + e - 1;
    if (x > MAX_EXP) return null;
    if (x < -MAX_EXP) return ZERO;
    return { n: neg ? -a : a, e };
  }
  /** Parse a typed or computed entry: optional "-", digits with one optional ".", optional "e+XX" (computed entries only). */
  function parse(str) {
    const m = /^(-?)(\d*)(?:\.(\d*))?(?:e([+-]\d+))?$/.exec(str);
    if (!m || (m[2] === "" && (m[3] === undefined || m[3] === ""))) return ZERO;
    const frac = m[3] || "";
    let n = BigInt((m[2] || "0") + frac); if (m[1]) n = -n;
    return make(n, (m[4] ? parseInt(m[4], 10) : 0) - frac.length) || ZERO;
  }
  function format(v) {
    if (!v || v.n === 0n) return "0";
    const neg = v.n < 0n, a = (neg ? -v.n : v.n).toString(), d = a.length, x = d + v.e - 1;
    let out;
    if (x >= MAX_DIGITS || x < -6) out = a[0] + (d > 1 ? "." + a.slice(1) : "") + "e" + (x < 0 ? "-" : "+") + String(Math.abs(x)).padStart(2, "0");
    else if (v.e >= 0) out = a + "0".repeat(v.e);
    else { const f = -v.e; out = f >= d ? "0." + "0".repeat(f - d) + a : a.slice(0, d - f) + "." + a.slice(d - f); }
    return (neg ? "-" : "") + out;
  }
  function add(a, b) { const e = Math.min(a.e, b.e); return make(a.n * 10n ** BigInt(a.e - e) + b.n * 10n ** BigInt(b.e - e), e); }
  const neg = (a) => ({ n: -a.n, e: a.e });
  function apply(a, op, b) {
    let v;
    if (op === "+") v = add(a, b);
    else if (op === "-") v = add(a, neg(b));
    else if (op === "*") v = make(a.n * b.n, a.e + b.e);
    else if (op === "/") {
      if (b.n === 0n) return { err: "div0" };
      const P = 30n;
      v = make((a.n * 10n ** P) / b.n, a.e - b.e - Number(P));        // >= 18 digits survive the integer division, then rounded once to 12
    } else return { err: "div0" };
    return v ? { v } : { err: "overflow" };
  }

  // ---------------- state ----------------
  function create() { return { acc: null, operator: null, entry: null, computed: false, last: null, just: false, error: null, expr: "", eq: 0 }; }
  const copy = (s) => Object.assign({}, s);
  const digitCount = (e) => e.replace(/[-.]/g, "").length;
  const fresh = (s) => (s.just && s.operator === null ? Object.assign(create(), { eq: s.eq }) : copy(s));   // a digit after "=" starts a new calculation
  const numberOf = (s) => (s.entry !== null ? parse(s.entry) : s.acc !== null ? s.acc : ZERO);

  function digit(st, d) {
    if (st.error || !/^[0-9]$/.test(String(d))) return st;
    const s = fresh(st); let e = s.computed || s.entry === null ? "" : s.entry;
    if (e === "0" || e === "-0") e = e.slice(0, -1);
    else if (digitCount(e) >= MAX_DIGITS) return st;
    s.entry = e + d; s.computed = false; s.just = false;
    return s;
  }
  function dot(st) {
    if (st.error) return st;
    const s = fresh(st); let e = s.computed || s.entry === null ? "0" : s.entry;
    if (e.indexOf(".") >= 0 || digitCount(e) >= MAX_DIGITS) return st;
    s.entry = e === "-" ? "-0." : e + "."; s.computed = false; s.just = false;
    return s;
  }
  function setError(st, code) { const s = copy(st); s.error = code; s.operator = null; s.entry = null; s.computed = false; return s; }
  function op(st, key) {
    key = Object.prototype.hasOwnProperty.call(ALIAS, key) ? ALIAS[key] : key;
    if (st.error || !Object.prototype.hasOwnProperty.call(SYMBOL, key)) return st;
    const s = copy(st);
    if (s.entry !== null) {
      const v = parse(s.entry);
      if (s.operator !== null && s.acc !== null) {
        const r = apply(s.acc, s.operator, v);
        if (r.err) return setError(st, r.err);
        s.acc = r.v;
      } else s.acc = v;
    } else if (s.acc === null) s.acc = ZERO;
    s.entry = null; s.computed = false; s.operator = key; s.just = false; s.last = null;
    s.expr = format(s.acc) + " " + SYMBOL[key];
    return s;
  }
  function equals(st) {
    if (st.error) return st;
    const s = copy(st);
    if (s.operator !== null) {
      const left = s.acc !== null ? s.acc : ZERO, right = s.entry !== null ? parse(s.entry) : left;      // 5 x = 25
      const r = apply(left, s.operator, right);
      if (r.err) return setError(st, r.err);
      s.expr = format(left) + " " + SYMBOL[s.operator] + " " + format(right) + " =";
      s.last = { op: s.operator, operand: right }; s.acc = r.v; s.operator = null;
    } else if (s.last && s.just) {                                                                         // repeated "="
      const r = apply(s.acc, s.last.op, s.last.operand);
      if (r.err) return setError(st, r.err);
      s.expr = format(s.acc) + " " + SYMBOL[s.last.op] + " " + format(s.last.operand) + " =";
      s.acc = r.v;
    } else if (s.entry !== null) { s.acc = parse(s.entry); s.expr = format(s.acc) + " ="; }
    else if (s.acc === null) return st;
    else s.expr = format(s.acc) + " =";
    s.entry = null; s.computed = false; s.just = true; s.eq = st.eq + 1;
    return s;
  }
  function percent(st) {
    if (st.error) return st;
    const s = copy(st);
    const base = s.entry !== null ? parse(s.entry) : s.acc;
    if (base === null) return st;
    const hundred = { n: 1n, e: 2 };
    let r;
    if ((s.operator === "+" || s.operator === "-") && s.acc !== null && s.entry !== null) {            // 200 + 10% = 200 + 20
      const m = apply(s.acc, "*", base);
      if (m.err) return setError(st, m.err);
      r = apply(m.v, "/", hundred);
    } else r = apply(base, "/", hundred);                                                              // 50% = 0.5; 200 x 10% = 200 x 0.1
    if (r.err) return setError(st, r.err);
    if (s.entry !== null || s.operator !== null) { s.entry = format(r.v); s.computed = true; }
    else { s.acc = r.v; }
    s.just = st.just && s.entry === null;
    return s;
  }
  function sign(st) {
    if (st.error) return st;
    const s = copy(st);
    if (s.entry !== null) { if (s.entry === "0") return st; s.entry = s.entry[0] === "-" ? s.entry.slice(1) : "-" + s.entry; }
    else if (s.operator !== null) { s.entry = "-0"; s.computed = false; }
    else if (s.acc !== null) s.acc = neg(s.acc);
    else return st;
    return s;
  }
  function backspace(st) {
    if (st.error || st.entry === null) return st;
    const s = copy(st);
    if (s.computed) { s.entry = "0"; s.computed = false; return s; }
    let e = s.entry.slice(0, -1);
    if (e === "" || e === "-" || e === "-0") e = "0";
    s.entry = e;
    return s;
  }
  function clearEntry(st) {
    if (st.error) return st;                                  // only Clear leaves the error state
    if (st.just) return create();
    const s = copy(st); s.entry = "0"; s.computed = false;    // the display shows 0; the pending operation and accumulator stay
    return s;
  }

  function input(st, name, arg) {
    switch (name) {
      case "digit": return digit(st, arg);
      case "dot": return dot(st);
      case "op": return op(st, arg);
      case "equals": return equals(st);
      case "percent": return percent(st);
      case "sign": return sign(st);
      case "clear": return create();
      case "clearEntry": return clearEntry(st);
      case "backspace": return backspace(st);
    }
    return st;
  }
  /** A state that holds a previous result as its current value (history reuse). Anything that is not a plain number gives the cleared state. */
  function load(text) {
    const s = create();
    if (typeof text !== "string" || !/^-?\d+(\.\d+)?(e[+-]\d+)?$/.test(text)) return s;
    s.acc = parse(text); s.just = true;
    return s;
  }
  function display(st) { return st.error ? ERRORS[st.error] : st.entry !== null ? st.entry : format(st.acc); }
  /** The line above the result: the pending operation or the last completed one. */
  function expression(st) { return st.error ? "" : st.expr; }

  return { create, input, load, display, expression, format, parse, numberOf, SYMBOL, ERRORS, MAX_DIGITS, MAX_EXP };
});
