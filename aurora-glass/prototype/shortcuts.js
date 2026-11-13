/* aurora-glass/prototype/shortcuts.js - the keyboard shortcut reference (window.AuroraShortcuts).
 *
 * SIMULATION ONLY. Press ? (or search Index for "keyboard shortcuts") to see every shortcut the prototype REALLY implements. The table below is the single
 * list; each entry mirrors a handler in app.js, ui.js, notes.js, calculator.js, vault.js, passport.js or clock.js, and tests/ui-golden/shortcuts.test.js
 * exercises representative entries of every group so the list cannot drift into imaginary shortcuts. "Mod" is Ctrl on Windows/Linux and Cmd on a Mac.
 * The ? key never opens the sheet while typing in a field, while any dialog (including a trusted review or hold-to-confirm) is open, or with a modifier.
 */
(function () {
  "use strict";
  const UI = window.AuroraUI, Shell = window.AuroraShell;
  if (!UI || !Shell) return;
  const { el, icon } = UI;
  const S = window.AuroraStrings, t = (k, p) => S.t(k, p);

  const isMac = () => /Mac|iPhone|iPad|iPod/i.test((navigator.platform || "") + " " + (navigator.userAgent || ""));
  const MOD = () => (isMac() ? "Cmd" : "Ctrl");

  // keys: array of alternatives; each alternative is an array of key names ("Mod" becomes Ctrl or Cmd). screens: which screen ids make the group "On this screen".
  const GROUPS = [
    { id: "everywhere", title: "Everywhere", rows: [
      { keys: [["Mod", "K"]], does: "Open Index to search apps, files, settings and commands." },
      { keys: [["?"]], does: "Show this list (not while typing in a field or while a dialog is open)." },
      { keys: [["Esc"]], does: "Close the top dialog; otherwise close the Glass Corner panel; otherwise go Back one screen. Some apps use Esc first for their own job (see their group)." },
    ] },
    { id: "index", title: "Index", rows: [
      { keys: [["↑"], ["↓"]], does: "Move through the results." },
      { keys: [["Home"], ["End"]], does: "Jump to the first or last result (after using the arrow keys)." },
      { keys: [["Enter"]], does: "Open the selected result." },
    ] },
    { id: "dialogs", title: "Dialogs", rows: [
      { keys: [["Esc"]], does: "Close or cancel the dialog." },
      { keys: [["Tab"], ["Shift", "Tab"]], does: "Move between the dialog's buttons; focus stays inside the dialog." },
      { keys: [["Space"], ["Enter"]], does: "Press and hold on a hold-to-confirm button to confirm a high-risk action." },
    ] },
    { id: "notes", title: "Notes", screens: ["screen-notes"], rows: [
      { keys: [["Mod", "N"]], does: "New note." },
      { keys: [["Mod", "S"]], does: "Save now." },
      { keys: [["Mod", "F"]], does: "Search notes." },
      { keys: [["Esc"]], does: "Clear the search; on a phone, go from the editor back to the list." },
      { keys: [["↑"], ["↓"], ["Home"], ["End"]], does: "Move through the note list." },
    ] },
    { id: "calculator", title: "Calculator", screens: ["screen-calculator"], rows: [
      { keys: [["0–9"], ["."], [","]], does: "Enter digits and the decimal point." },
      { keys: [["+"], ["-"], ["*"], ["/"]], does: "Add, subtract, multiply, divide." },
      { keys: [["Enter"], ["="]], does: "Equals." },
      { keys: [["Backspace"]], does: "Delete the last digit." },
      { keys: [["Delete"]], does: "Clear the current entry." },
      { keys: [["Esc"]], does: "Clear everything." },
      { keys: [["%"]], does: "Percent." },
      { keys: [["F9"], ["_"]], does: "Change sign." },
    ] },
    { id: "vault", title: "Vault (Files)", screens: ["screen-vault"], rows: [
      { keys: [["↑"], ["↓"], ["←"], ["→"], ["Home"], ["End"]], does: "Move between files." },
      { keys: [["Space"]], does: "Select or deselect the focused file." },
      { keys: [["Mod", "A"]], does: "Select all visible files." },
      { keys: [["Delete"]], does: "Review deleting the selected files (nothing is deleted until you confirm)." },
      { keys: [["Esc"]], does: "Clear the selection, or close the preview." },
    ] },
    { id: "menu", title: "Actions menu (Vault files, Pages)", screens: ["screen-vault", "screen-pages"], rows: [
      { keys: [["Shift", "F10"], ["Menu"]], does: "Open the actions menu for the focused file or Page. Right-click or a long press does the same." },
      { keys: [["↑"], ["↓"], ["Home"], ["End"]], does: "Move through the menu; typing a letter jumps to a matching action." },
      { keys: [["Enter"], ["Space"]], does: "Run the action. Risky ones (Move, Delete) still open their review first." },
      { keys: [["Esc"]], does: "Close the menu and return to the item." },
    ] },
    { id: "tabs", title: "Tabs (Clock, Passport)", screens: ["screen-clock", "screen-passport"], rows: [
      { keys: [["←"], ["→"]], does: "Switch to the previous or next tab." },
      { keys: [["Home"], ["End"]], does: "Jump to the first or last tab." },
    ] },
  ];

  let isOpen = false;

  const keyEl = (name) => el("kbd", { class: "kbd", text: name === "Mod" ? MOD() : name });
  function keysNode(keys) {
    const wrap = el("span", { class: "kbd-keys" });
    keys.forEach((alt, i) => {
      if (i) wrap.appendChild(el("span", { class: "kbd-or", text: " or " }));
      alt.forEach((k, j) => { if (j) wrap.appendChild(el("span", { class: "kbd-plus", text: " + ", "aria-hidden": "true" })); wrap.appendChild(keyEl(k)); });
    });
    return wrap;
  }
  const keysText = (keys) => keys.map((alt) => alt.map((k) => (k === "Mod" ? MOD() : k)).join(" plus ")).join(" or ");

  /** Groups in display order: the group for the current screen first, titled "On this screen: ...". */
  function orderedGroups() {
    const cur = Shell.state.activeScreen;
    const here = GROUPS.filter((g) => g.screens && g.screens.includes(cur));
    return here.concat(GROUPS.filter((g) => !here.includes(g)));
  }

  function show() {
    if (isOpen || document.querySelector(".scrim")) return false;
    isOpen = true;
    const here = new Set(orderedGroups().filter((g) => g.screens && g.screens.includes(Shell.state.activeScreen)).map((g) => g.id));
    const filter = el("input", { type: "search", class: "field", id: "shortcuts-filter", placeholder: t("shortcuts.filter"), "aria-label": t("shortcuts.filter"), autocomplete: "off" });
    const list = el("div", { class: "kbd-groups", id: "shortcuts-list" });
    const none = el("p", { class: "row-meta", id: "shortcuts-none", role: "status", hidden: true });
    function render() {
      const q = filter.value.trim().toLowerCase();
      UI.clear(list);
      let shown = 0;
      orderedGroups().forEach((g) => {
        const rows = g.rows.filter((r) => !q || (g.title + " " + r.does + " " + keysText(r.keys)).toLowerCase().indexOf(q) !== -1);
        if (!rows.length) return;
        shown += rows.length;
        const title = here.has(g.id) ? t("shortcuts.onThisScreen", { group: g.title }) : g.title;
        list.appendChild(el("section", { class: "kbd-group", "data-group": g.id, "aria-label": title }, [
          el("h3", { class: "kbd-group-title", text: title }),
          el("ul", { class: "kbd-rows" }, rows.map((r) => el("li", { class: "kbd-row", "aria-label": keysText(r.keys) + ": " + r.does }, [keysNode(r.keys), el("span", { class: "kbd-does", text: r.does })]))),
        ]));
      });
      none.hidden = shown > 0;
      none.textContent = shown ? "" : t("shortcuts.none", { query: filter.value.trim() });
    }
    filter.addEventListener("input", render);
    render();
    const body = el("div", { class: "kbd-sheet" }, [
      el("p", { class: "row-meta", text: t("shortcuts.intro") }),
      el("div", { class: "search-field" }, [icon("search"), filter]), none, list,
    ]);
    UI.dialog({ id: "shortcuts-dialog", title: t("shortcuts.title"), icon: "keyboard", body, actions: [{ label: t("shortcuts.close"), kind: "accent", value: true, id: "btn-shortcuts-close" }], initialFocus: "#shortcuts-filter" })
      .then(() => { isOpen = false; });
    return true;
  }

  // ? opens the sheet, but only when the focus context allows it.
  function onKey(e) {
    if (e.key !== "?" || e.defaultPrevented || e.ctrlKey || e.metaKey || e.altKey) return;
    const t = e.target;
    if (t && t.closest && t.closest("input, textarea, select, [contenteditable='true'], [contenteditable=''], .scrim, [aria-modal='true']")) return;
    if (document.querySelector(".scrim")) return;
    e.preventDefault();
    show();
  }
  document.addEventListener("keydown", onKey);

  // Index entry: a real control for people who do not know the key.
  Shell.registerIndexSource((q) => {
    const s = String(q || "").trim().toLowerCase();
    if (s.length < 3 || !["keyboard shortcuts", "shortcuts", "hotkeys", "keys", "keybindings"].some((k) => k.startsWith(s) || s.startsWith(k))) return [];
    return [{ kind: "Action", label: "Keyboard shortcuts", meta: "Show every keyboard shortcut", icon: "keyboard", action: () => setTimeout(show, 0) }];
  });

  window.AuroraShortcuts = { show, isOpen: () => isOpen, groups: () => GROUPS.map((g) => ({ id: g.id, title: g.title, rows: g.rows.length })) };
})();
