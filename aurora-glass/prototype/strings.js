/* aurora-glass/prototype/strings.js - language support glue (window.AuroraStrings).
 *
 * SIMULATION ONLY. Wraps the DOM-free model and catalog. Use AuroraStrings.t("key", {name: value}) and AuroraStrings.tn("key", n) for text, always with a
 * literal key (tests scan the code for keys). Static HTML opts in with data-i18n="key" (text) and data-i18n-attr="aria-label:key;title:key2" (attributes).
 * The language comes from ?lang=xx (only a language that exists in the catalog; anything else is ignored) and defaults to English. There is no language
 * picker yet because only English is verified: Hindi and Gujarati are placeholders that show English (see strings-catalog.js), and en-XA is a test-only
 * pseudo-locale for long-string layout checks. <html lang> says "en" while a placeholder is selected, because the text really is English.
 */
(function () {
  "use strict";
  const C = window.AuroraStringsCatalog, M = window.AuroraStringsModel;
  if (!C || !M) return;
  const S = M.create({ catalogs: C.locales, fallback: C.DEFAULT_LOCALE });

  function fromUrl() {
    try { const l = new URLSearchParams(location.search).get("lang"); return l && S.status(l) ? l : null; } catch (e) { return null; }
  }

  /** Re-translate every opted-in element under `rootNode` (default: the whole document). */
  function apply(rootNode) {
    const r = rootNode || document;
    r.querySelectorAll("[data-i18n]").forEach((n) => { n.textContent = S.t(n.getAttribute("data-i18n")); });
    r.querySelectorAll("[data-i18n-attr]").forEach((n) => {
      n.getAttribute("data-i18n-attr").split(";").forEach((pair) => {
        const i = pair.indexOf(":"); if (i < 1) return;
        n.setAttribute(pair.slice(0, i).trim(), S.t(pair.slice(i + 1).trim()));
      });
    });
  }

  function setLocale(l) {
    if (!S.setLocale(l)) return false;
    const root = document.documentElement, st = S.status(l);
    root.setAttribute("lang", st === "placeholder" ? "en" : l);         // a placeholder shows English text, so it must not claim another language
    root.setAttribute("data-locale", l); root.setAttribute("data-locale-status", st);
    apply();
    document.dispatchEvent(new CustomEvent("aurora:locale", { detail: { locale: l, status: st } }));
    return true;
  }

  window.AuroraStrings = { t: S.t, tn: S.tn, locale: S.locale, setLocale, locales: S.locales, status: S.status, isVerified: S.isVerified, missing: S.missing, clearMissing: S.clearMissing, apply };
  setLocale(fromUrl() || C.DEFAULT_LOCALE);
})();
