/* aurora-glass/prototype/strings-model.js - DOM-free message lookup (window.AuroraStringsModel).
 *
 * SIMULATION ONLY. create({ catalogs, locale }) gives t(key, params) and tn(key, n, params). Lookup order: the locale, its base language ("en-XA" -> "en"),
 * then English; an unknown key returns the key itself (visibly wrong, so tests notice) and is recorded in missing(). {name} placeholders are filled from
 * params; one that has no value is left as {name} and recorded. Plurals use Intl.PluralRules of the locale with .one/.other suffixes.
 * pseudo(text) makes the test-only "en-XA" strings: accented letters, wrapped in brackets, about 40% longer with short filler words so lines still wrap
 * like real text; {placeholders} are left intact. Loadable in the browser and in Node.
 */
(function (root, factory) {
  if (typeof module === "object" && module.exports) module.exports = factory();
  else root.AuroraStringsModel = factory();
})(typeof self !== "undefined" ? self : this, function () {
  "use strict";

  const ACCENT = { a: "å", b: "ƀ", c: "ç", d: "ð", e: "é", f: "ƒ", g: "ĝ", h: "ĥ", i: "î", j: "ĵ", k: "ķ", l: "ļ", m: "ḿ", n: "ñ", o: "ö", p: "þ", q: "ǫ", r: "ŕ", s: "š", t: "ţ", u: "û", v: "ṽ", w: "ẃ", x: "ẋ", y: "ý", z: "ž",
    A: "Å", B: "Ɓ", C: "Ç", D: "Ð", E: "É", F: "Ƒ", G: "Ĝ", H: "Ĥ", I: "Î", J: "Ĵ", K: "Ķ", L: "Ļ", M: "Ḿ", N: "Ñ", O: "Ö", P: "Þ", Q: "Ǫ", R: "Ŕ", S: "Š", T: "Ţ", U: "Û", V: "Ṽ", W: "Ẃ", X: "Ẋ", Y: "Ý", Z: "Ž" };
  const FILLER = ["ööö", "åååå", "ééé"];
  const PLACEHOLDER = /\{(\w+)\}/g;

  /** Test-only pseudo-localisation. Keeps {placeholders}; adds ~40% length as separate short words so wrapping stays realistic. */
  function pseudo(text) {
    const s = String(text === undefined || text === null ? "" : text);
    if (!s) return s;
    const accented = s.split(/(\{\w+\})/).map((part) => (/^\{\w+\}$/.test(part) ? part : part.replace(/[A-Za-z]/g, (c) => ACCENT[c] || c))).join("");
    const extra = Math.max(1, Math.ceil(s.replace(PLACEHOLDER, "").length * 0.4));
    let pad = "", i = 0;
    while (pad.length < extra) { pad += " " + FILLER[i % FILLER.length]; i++; }
    return "[" + accented + pad + "]";
  }

  /** Fill {name} placeholders. Returns { text, unfilled: [names] }. */
  function format(template, params) {
    const unfilled = [];
    const text = String(template).replace(PLACEHOLDER, (m, name) => {
      if (params && Object.prototype.hasOwnProperty.call(params, name) && params[name] !== undefined && params[name] !== null) return String(params[name]);
      unfilled.push(name); return m;
    });
    return { text, unfilled };
  }

  function create(opts) {
    const o = opts || {};
    const catalogs = o.catalogs || {};
    const fallback = o.fallback || "en";
    const miss = new Set();
    let locale = fallback;

    const known = (l) => Object.prototype.hasOwnProperty.call(catalogs, l);
    function chain(l) {
      const out = [];
      if (known(l)) out.push(l);
      const base = String(l).split("-")[0];
      if (base !== l && known(base)) out.push(base);
      if (out.indexOf(fallback) < 0) out.push(fallback);
      return out;
    }
    function raw(key) {
      const english = known(fallback) ? catalogs[fallback].messages[key] : undefined;
      for (const l of chain(locale)) {
        const c = catalogs[l];
        if (c.pseudo) { if (typeof english === "string") return pseudo(english); continue; }
        const v = c.messages && c.messages[key];
        if (typeof v === "string" && v) return v;
      }
      return typeof english === "string" ? english : undefined;
    }
    function note(key, why) { miss.add(why ? key + " (" + why + ")" : key); }

    function t(key, params) {
      const v = raw(key);
      if (v === undefined) { note(key); return key; }
      const f = format(v, params);
      f.unfilled.forEach((n) => note(key, "no value for {" + n + "}"));
      return f.text;
    }
    function tn(key, n, params) {
      // Plural rules follow the language the text really comes from: a locale that has no own plural message falls back to English text AND English rules
      // (Hindi counts 0 as "one"; applying that to English strings would print "0 result").
      const own = known(locale) && !catalogs[locale].pseudo && catalogs[locale].messages && typeof catalogs[locale].messages[key + ".other"] === "string" && catalogs[locale].messages[key + ".other"];
      let cat = "other";
      try { cat = new Intl.PluralRules(own ? locale : fallback).select(n); } catch (e) { cat = n === 1 ? "one" : "other"; }
      const pick = [key + "." + cat, key + ".other", key].find((k) => raw(k) !== undefined);        // a plain (non-plural) key is accepted too
      if (pick === undefined) { note(key); return key; }
      return t(pick, Object.assign({ n }, params));
    }
    function setLocale(l) { if (!known(l)) return false; locale = l; return true; }

    return {
      t, tn, setLocale,
      locale: () => locale,
      has: (key) => raw(key) !== undefined,
      locales: () => Object.keys(catalogs).map((id) => ({ id, name: catalogs[id].name, status: catalogs[id].status })),
      status: (l) => (known(l) ? catalogs[l].status : null),
      isVerified: (l) => known(l) && catalogs[l].status === "verified",
      missing: () => Array.from(miss).sort(),
      clearMissing: () => miss.clear(),
    };
  }

  return { create, format, pseudo };
});
