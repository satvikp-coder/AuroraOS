/* aurora-glass/prototype/strings-catalog.js - the message catalogs (window.AuroraStringsCatalog).
 *
 * SIMULATION ONLY. Only ENGLISH is verified. Hindi (hi) and Gujarati (gu) are PLACEHOLDERS: their catalogs are deliberately EMPTY so every string falls back to
 * English, and the interface never shows an unreviewed or machine-made translation. A placeholder becomes "draft" only when a fluent person has supplied
 * the strings, and "verified" only after a fluent reviewer has checked them in the layout (tests/unit/strings.test.js enforces that placeholders are empty
 * and that only English is marked verified). "en-XA" is a TEST-ONLY pseudo-locale generated from English (accented letters, about 40% longer) used to stress
 * layouts with long strings; it is never offered to people.
 * Only the strings listed here are translatable so far (dock labels, the welcome tour, the shortcut sheet chrome, the Index result count). The rest of the
 * prototype is English-only and says nothing different: see docs/agents/AURORA_UI_TASKS.md (M8-G) for the migration list.
 * Keys are dot-separated, always written as string literals in the code that uses them (so a test can find unused or missing keys), and use {name}
 * placeholders. Plural messages use the suffixes .one and .other.
 */
(function (root, factory) {
  if (typeof module === "object" && module.exports) module.exports = factory();
  else root.AuroraStringsCatalog = factory();
})(typeof self !== "undefined" ? self : this, function () {
  "use strict";

  const en = {
    "dock.back": "Back", "dock.home": "Home", "dock.spaces": "Spaces", "dock.pages": "Pages", "dock.ask": "Ask", "dock.ask.aria": "Ask Aura / Search",

    "index.results.one": "{n} result", "index.results.other": "{n} results", "index.noResults": "No results",

    "welcome.title": "Welcome to Aurora Glass",
    "welcome.stepLabel": "Step {n} of {total}",
    "welcome.back": "Back", "welcome.next": "Next", "welcome.finish": "Finish", "welcome.skip": "Skip tour",
    "welcome.note": "This is a browser prototype. Everything is simulated here: no cellular, audio, camera, location or other phone hardware is used, and Notes are saved in this browser only.",
    "welcome.step1.title": "Spaces keep your day organised",
    "welcome.step1.body": "Aurora Glass groups what you do into Spaces: Study, Work, Projects and Personal, plus Spaces you create.",
    "welcome.step1.p1": "Each Space has its own apps and open pages.",
    "welcome.step1.p2": "Switch Space from Home or with the Spaces button in the Dock.",
    "welcome.step2.title": "Apps, Pages and getting around",
    "welcome.step2.body": "Open an app from Home, the Dock or Index. Press Ctrl+K (Cmd+K on a Mac) to search.",
    "welcome.step2.p1": "Every open app is a Page. Pages shows what is running and lets you pause or close it.",
    "welcome.step2.p2": "Back, or the Escape key, always goes one step back.",
    "welcome.step3.title": "Aura asks before it acts",
    "welcome.step3.body": "Aura can propose actions, such as starting a timer or moving a file. You review each one before anything happens.",
    "welcome.step3.p1": "Passport shows what each app is allowed to do.",
    "welcome.step3.p2": "Trail records every action and lets you undo it.",
    "welcome.step3.p3": "Deleting needs an extra press-and-hold to confirm.",

    "shortcuts.title": "Keyboard shortcuts",
    "shortcuts.filter": "Filter shortcuts",
    "shortcuts.intro": "Only shortcuts this prototype really has. On a Mac, Cmd is used where Windows shows Ctrl.",
    "shortcuts.none": "No shortcut matches “{query}”.",
    "shortcuts.onThisScreen": "On this screen: {group}",
    "shortcuts.close": "Close",
  };

  /** status: verified (reviewed by a fluent person, English only today) | draft | placeholder (EMPTY, falls back to English) | test-only. */
  const locales = {
    en: { name: "English", status: "verified", messages: en },
    hi: { name: "हिन्दी (Hindi)", status: "placeholder", messages: {} },
    gu: { name: "ગુજરાતી (Gujarati)", status: "placeholder", messages: {} },
    "en-XA": { name: "Pseudo-locale (test only)", status: "test-only", pseudo: true, messages: {} },
  };

  return { locales, DEFAULT_LOCALE: "en" };
});
