/** tests/unit/calc.test.js - the DOM-free pocket-calculator engine (aurora-glass/prototype/calc-engine.js). */
const assert = require("assert");
const fs = require("fs");
const path = require("path");
const FILE = path.resolve(__dirname, "..", "..", "aurora-glass", "prototype", "calc-engine.js");
const E = require(FILE);

let passed = 0, failed = 0;
const ser = (o) => JSON.stringify(o, (k, v) => (typeof v === "bigint" ? v.toString() + "n" : v));
function test(name, fn) { try { fn(); passed++; console.log("  PASS:", name); } catch (e) { failed++; console.log("  FAIL:", name, "\n   ", e.message); } }

/** Key-sequence runner: digits, '.', + - * / (operators), '=', '%', n (sign), C (clear), E (clear entry), B (backspace). */
function run(seq, state) {
  let s = state || E.create();
  for (const ch of seq) {
    if (ch >= "0" && ch <= "9") s = E.input(s, "digit", ch);
    else if (ch === ".") s = E.input(s, "dot");
    else if ("+-*/".includes(ch)) s = E.input(s, "op", ch);
    else if (ch === "=") s = E.input(s, "equals");
    else if (ch === "%") s = E.input(s, "percent");
    else if (ch === "n") s = E.input(s, "sign");
    else if (ch === "C") s = E.input(s, "clear");
    else if (ch === "E") s = E.input(s, "clearEntry");
    else if (ch === "B") s = E.input(s, "backspace");
    else throw new Error("bad key " + ch);
  }
  return s;
}
const show = (seq) => E.display(run(seq));

console.log("== arithmetic: immediate execution, no precedence ==");
const TABLE = [
  ["", "0"], ["7", "7"], ["2+3=", "5"], ["2+3+", "5"], ["2+3+4=", "9"], ["2+3*4=", "20"], ["2*3+4=", "10"], ["10/4=", "2.5"], ["5-8=", "-3"],
  ["8-3-2=", "3"], ["9/3/3=", "1"], ["0/5=", "0"], ["5-5=", "0"], ["2*3=", "6"], ["100/8=", "12.5"], ["1/8=", "0.125"], ["2.5*4=", "10"], ["3.0*2=", "6"],
  ["-5=", "-5"], ["7n/2=", "-3.5"],
];
TABLE.forEach(([seq, want]) => test(`"${seq}" -> ${want}`, () => assert.strictEqual(show(seq), want)));

console.log("== decimal arithmetic is exact (no binary floating point) ==");
[["0.1+0.2=", "0.3"], ["0.1*3=", "0.3"], ["1.1+2.2=", "3.3"], ["10-9.99=", "0.01"], ["0.5+0.5=", "1"], ["1.5*2=", "3"], ["0.07*100=", "7"], ["4.35*100=", "435"],
 ["1/3=", "0.333333333333"], ["2/3=", "0.666666666667"], ["1/7=", "0.142857142857"], ["10/3=", "3.33333333333"], ["22/7=", "3.14285714286"], ["5/3=", "1.66666666667"]]
  .forEach(([seq, want]) => test(`"${seq}" -> ${want}`, () => assert.strictEqual(show(seq), want)));

console.log("== repeated equals repeats the last operation ==");
[["5*=", "25"], ["2+=", "4"], ["2+3==", "8"], ["2+3===", "11"], ["2*3==", "18"], ["9-2==", "5"], ["2*2====", "32"], ["8/2==", "2"]]
  .forEach(([seq, want]) => test(`"${seq}" -> ${want}`, () => assert.strictEqual(show(seq), want)));

console.log("== entering numbers ==");
[["00", "0"], ["007", "7"], ["0.0", "0.0"], ["1.2.3", "1.23"], [".5", "0.5"], [".5+.5=", "1"], ["123456789012", "123456789012"], ["1234567890123", "123456789012"],
 ["12345678901.2", "12345678901.2"], ["123456789012.", "123456789012"], ["12B", "1"], ["1B", "0"], ["B", "0"], ["12BBB", "0"], ["1.5B", "1."], ["1.5BB", "1"], ["9n", "-9"], ["0n", "0"],
 ["9nn", "9"], ["9nB", "0"], ["9nB5", "5"], ["12n3", "-123"], ["0.5n", "-0.5"]]
  .forEach(([seq, want]) => test(`"${seq}" -> ${want}`, () => assert.strictEqual(show(seq), want)));

console.log("== negative numbers, operators, clear, clear entry ==");
[["5n+3=", "-2"], ["5+3n=", "2"], ["5+n3=", "2"], ["2+*3=", "6"], ["2+-3=", "-1"], ["2+3E", "0"], ["2+3E4=", "6"], ["2+3=E", "0"], ["2+3C", "0"], ["2+3=4", "4"], ["2+3=4=", "4"],
 ["2+3=*2=", "10"], ["2+3=n", "-5"], ["2+3=n+1=", "-4"], ["2+3=B", "5"]]
  .forEach(([seq, want]) => test(`"${seq}" -> ${want}`, () => assert.strictEqual(show(seq), want)));

console.log("== percent ==");
[["200+10%=", "220"], ["200-10%=", "180"], ["200*10%=", "20"], ["200/10%=", "2000"], ["50%", "0.5"], ["50+50=%", "1"], ["200+10%+5=", "225"], ["50%B", "0"], ["0.01%", "0.0001"]]
  .forEach(([seq, want]) => test(`"${seq}" -> ${want}`, () => assert.strictEqual(show(seq), want)));

console.log("== large and small results ==");
[["999999999999+1=", "1e+12"], ["123456789012+1=", "123456789013"], ["1000000*1000000=", "1e+12"], ["999999999999*999999999999=", "9.99999999998e+23"],
 ["1/1000000=", "0.000001"], ["1/10000000=", "1e-07"], ["999999999999*2=", "2e+12"], ["1/3*3=", "0.999999999999"]]
  .forEach(([seq, want]) => test(`"${seq}" -> ${want}`, () => assert.strictEqual(show(seq), want)));
test("repeated multiplication reaches the overflow error exactly when the value passes 1e+99", () => {
  let s = run("999999999999*999999999999=");
  let steps = 1;
  while (!s.error && steps < 30) { s = E.input(s, "equals"); steps++; }
  assert.strictEqual(steps, 8);
  assert.strictEqual(s.error, "overflow");
  assert.strictEqual(E.display(s), E.ERRORS.overflow);
  assert.ok(E.display(run("999999999999*999999999999" + "=".repeat(7))).includes("e+"));        // 7 presses: still a number
});
test("repeated multiplication of tiny values underflows to 0, never to an error", () => {
  const at13 = run("0.0000001*0.0000001" + "=".repeat(13));                              // 13 presses: 1e-98
  assert.strictEqual(E.display(at13), "1e-98");
  assert.strictEqual(E.display(run("0.0000001*0.0000001" + "=".repeat(14))), "0");          // 14 presses: below 1e-99
});

console.log("== division by zero: an error state until Clear ==");
test("7/0= shows the error, every other input is ignored, Clear recovers", () => {
  const e = run("7/0=");
  assert.strictEqual(e.error, "div0");
  assert.strictEqual(E.display(e), "Can't divide by zero");
  ["5", ".", "+", "=", "%", "n", "B", "E"].forEach((k) => assert.strictEqual(E.display(run(k, e)), "Can't divide by zero", "ignored: " + k));
  assert.strictEqual(E.display(run("C5", e)), "5");
});
test("dividing by zero in a chain (1/0+) and by 0.0 is the same error; 0/0 too", () => {
  assert.strictEqual(run("1/0+").error, "div0");
  assert.strictEqual(run("5/0.0=").error, "div0");
  assert.strictEqual(run("0/0=").error, "div0");
  assert.strictEqual(run("5/0%").error, null);                       // percent divides by 100, never by the entry
});
test("after the error, Clear returns to a working calculator", () => {
  assert.strictEqual(E.display(run("5/0=C")), "0");
  assert.strictEqual(E.display(run("5/0=C5/2=")), "2.5");
  assert.strictEqual(E.display(run("5/1=")), "5");
});

console.log("== expression line ==");
[["2+", "2 +"], ["2+3", "2 +"], ["12-", "12 −"], ["2+3=", "2 + 3 ="], ["2*3==", "6 × 3 ="], ["5=", "5 ="], ["6/3=", "6 ÷ 3 ="], ["2+3+", "5 +"], ["C", ""], ["5*=", "5 × 5 ="]]
  .forEach(([seq, want]) => test(`"${seq}" shows "${want}"`, () => assert.strictEqual(E.expression(run(seq)), want)));
test("the error state has no expression", () => assert.strictEqual(E.expression(run("1/0=")), ""));

console.log("== load(): reusing a result from the history ==");
test("load() makes a result the current value; operators continue from it and a digit starts afresh", () => {
  assert.strictEqual(E.display(E.load("12.5")), "12.5");
  assert.strictEqual(E.display(run("+1=", E.load("12.5"))), "13.5");
  assert.strictEqual(E.display(run("7", E.load("12.5"))), "7");
  assert.strictEqual(E.display(E.load("1e-07")), "1e-07");
  assert.strictEqual(E.display(run("*2=", E.load("-4"))), "-8");
});
test("load() of anything that is not a plain number is the cleared calculator", () => {
  ["", "abc", "1+1", "NaN", "Infinity", "1e", "--1", ".5", "5.", null, undefined, 5, {}, "0x10"].forEach((t) => assert.strictEqual(E.display(E.load(t)), "0", String(t)));
});

console.log("== the equals counter (history hook) ==");
test("eq increases on each evaluation and on repeated equals, not on typing", () => {
  assert.strictEqual(run("2+3").eq, 0);
  assert.strictEqual(run("2+3=").eq, 1);
  assert.strictEqual(run("2+3===").eq, 3);
  assert.strictEqual(run("2+3=4+5=").eq, 2);
});

console.log("== purity and hostile input ==");
test("input() never mutates the state it was given", () => {
  const s0 = E.create(), snap = ser(s0);
  const s1 = E.input(s0, "digit", "5");
  assert.strictEqual(ser(s0), snap);
  assert.notStrictEqual(s0, s1);
  const s2 = run("2+3");
  const snap2 = ser(s2);
  ["digit", "dot", "op", "equals", "percent", "sign", "clear", "clearEntry", "backspace"].forEach((n) => E.input(s2, n, n === "digit" ? "4" : "+"));
  assert.strictEqual(ser(s2), snap2);
});
test("unknown names, bad digits and bad operators are ignored", () => {
  const s = run("12+3");
  const d = E.display(s), x = E.expression(s);
  [["bogus"], ["digit", "x"], ["digit", "10"], ["digit", ""], ["digit", undefined], ["digit", null], ["op", "^"], ["op", ""], ["op", "**"], ["op", undefined], ["op", "constructor"], ["op", "__proto__"], [""]]
    .forEach((a) => { const t = E.input(s, a[0], a[1]); assert.strictEqual(E.display(t), d); assert.strictEqual(E.expression(t), x); });
});
test("the display aliases of the operators are accepted", () => {
  assert.strictEqual(E.display(E.input(E.input(E.input(run("6"), "op", "÷"), "digit", "3"), "equals")), "2");
  assert.strictEqual(E.display(E.input(E.input(E.input(run("6"), "op", "×"), "digit", "3"), "equals")), "18");
  assert.strictEqual(E.display(E.input(E.input(E.input(run("6"), "op", "−"), "digit", "3"), "equals")), "3");
});
test("a very long run of key presses stays consistent and fast", () => {
  let s = E.create();
  for (let i = 0; i < 5000; i++) s = E.input(s, i % 7 === 0 ? "op" : i % 11 === 0 ? "equals" : "digit", i % 7 === 0 ? "+" : String(i % 10));
  assert.ok(typeof E.display(s) === "string" && E.display(s).length > 0);
});
test("format() and parse() round-trip", () => {
  ["0", "1", "-1", "0.5", "-0.25", "123456789012", "0.000001", "1e+12", "1.5e+15", "-9.99999999998e+23", "1e-07", "1e-98", "1e+99"].forEach((t) => assert.strictEqual(E.format(E.parse(t)), t, t));
  assert.strictEqual(E.format(E.parse("0.10")), "0.1");
  assert.strictEqual(E.format(E.parse("-0.0")), "0");
  assert.strictEqual(E.format(E.parse("007")), "7");
});
test("the engine source contains no eval, Function constructor, new Function or dynamic code", () => {
  const src = fs.readFileSync(FILE, "utf8");
  assert.ok(!/\beval\s*\(/.test(src));
  assert.ok(!/\bFunction\s*\(/.test(src));
  assert.ok(!/new\s+Function/.test(src));
  assert.ok(!/setTimeout\s*\(\s*["'`]/.test(src));
});

console.log(`\nCalculator engine: ${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
