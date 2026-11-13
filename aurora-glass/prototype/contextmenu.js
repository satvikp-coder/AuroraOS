/* aurora-glass/prototype/contextmenu.js - the shared context menu (window.AuroraContextMenu).
 *
 * SIMULATION ONLY. One primitive for every "actions for this item" menu. It opens from a right-click, Shift+F10 / the Menu key on a focused row, or a
 * long-press on touch, and it is placed so it always stays inside the viewport. It is a launcher and nothing more: an item only calls the callback its
 * owner supplied, exactly as the owner's own button would, so a menu can never approve, skip a review, or satisfy a hold-to-confirm. Anything risky
 * (for example Delete in Vault) still opens the normal review dialog from the item.
 * Behaviour: role="menu"; Up/Down/Home/End/type-ahead move; Enter or Space runs the item; Escape closes and returns focus to the row; Tab closes;
 * a click outside only closes (it does not click through). It never opens over a dialog.
 */
(function () {
  "use strict";
  const UI = window.AuroraUI, M = window.AuroraContextMenuModel;
  if (!UI || !M) return;
  const { el, icon } = UI;

  let cur = null, typed = "", typedAt = 0, seq = 0, keyOpenedAt = 0, openedAt = 0;

  function close(restore) {
    if (!cur) return false;
    const c = cur; cur = null;
    document.removeEventListener("keydown", c.onKey, true);
    window.removeEventListener("resize", c.onDismiss);
    window.removeEventListener("scroll", c.onDismiss, true);
    c.layer.remove();
    if (restore && c.opener && c.opener.isConnected) { try { c.opener.focus({ preventScroll: true }); } catch (e) { /* ignore */ } }
    return true;
  }

  /**
   * Open a menu. o = { items, x, y, opener, label }. Items: { label, icon?, danger?, disabled?, reason?, hint?, action }.
   * Returns { close } or null if nothing was opened (empty list, or a dialog is open).
   */
  function open(o) {
    const items = M.normalise(o && o.items);
    if (!items.length || document.querySelector(".scrim")) return null;
    close(false);
    const id = "ctx-menu-" + (++seq);
    const opener = o.opener || document.activeElement;
    const nodes = [];
    const menu = el("div", { class: "ctx-menu", id: "ctx-menu", role: "menu", "aria-orientation": "vertical", "aria-label": o.label || "Actions" });
    items.forEach((it, i) => {
      if (it.separator) { const s = el("div", { class: "ctx-sep", role: "separator" }); menu.appendChild(s); nodes[i] = null; return; }
      const note = it.disabled ? it.reason : it.hint;
      const noteId = note ? id + "-note-" + i : undefined;
      const b = el("button", { type: "button", class: "ctx-item" + (it.danger ? " is-danger" : "") + (it.disabled ? " is-disabled" : ""), role: "menuitem", tabindex: "-1", "data-ctx": it.id || String(i),
        "aria-disabled": it.disabled ? "true" : undefined, "aria-describedby": noteId }, [
        it.icon ? icon(it.icon) : el("span", { class: "ctx-icon-gap", "aria-hidden": "true" }),
        el("span", { class: "ctx-text" }, [el("span", { class: "ctx-label", text: it.label }), note ? el("span", { class: "ctx-note", id: noteId, text: note }) : null]),
      ]);
      b.addEventListener("click", (e) => {
        e.stopPropagation();
        if (it.disabled) return;                                     // a disabled item can be read but never runs
        close(true);
        try { it.action(); } catch (err) { console.error(err); }
      });
      b.addEventListener("pointermove", () => { if (document.activeElement !== b) b.focus({ preventScroll: true }); });
      nodes[i] = b; menu.appendChild(b);
    });
    const layer = el("div", { class: "ctx-layer", id: "ctx-layer" }, [menu]);
    layer.addEventListener("click", (e) => { if (e.target === layer) close(false); });          // outside: close, never click through
    layer.addEventListener("contextmenu", (e) => { e.preventDefault(); if (e.target === layer) close(false); });
    document.body.appendChild(layer);

    const vw = document.documentElement.clientWidth, vh = document.documentElement.clientHeight;
    menu.style.left = "0px"; menu.style.top = "0px";
    const r = menu.getBoundingClientRect();
    const p = M.place(o.x, o.y, r.width, r.height, vw, vh);
    menu.style.left = p.left + "px"; menu.style.top = p.top + "px";
    menu.style.maxHeight = p.maxHeight + "px"; menu.style.maxWidth = p.maxWidth + "px";
    if (p.scrolls) menu.classList.add("is-scrolling");

    const focusAt = (i) => { if (i >= 0 && nodes[i]) { nodes.forEach((n) => { if (n) n.tabIndex = -1; }); nodes[i].tabIndex = 0; nodes[i].focus({ preventScroll: false }); } };
    const index = () => nodes.indexOf(document.activeElement);
    function onKey(e) {
      if (!cur || cur.layer !== layer) return;
      const k = e.key;
      if (k === "Escape") { e.preventDefault(); e.stopPropagation(); e.stopImmediatePropagation(); close(true); return; }     // the menu is the top layer: nothing else sees this Escape
      if (k === "Tab") { e.preventDefault(); close(true); return; }
      if (k === "ArrowDown") { e.preventDefault(); e.stopPropagation(); focusAt(M.step(items, index(), 1)); return; }
      if (k === "ArrowUp") { e.preventDefault(); e.stopPropagation(); focusAt(M.step(items, index(), -1)); return; }
      if (k === "Home") { e.preventDefault(); e.stopPropagation(); focusAt(M.firstIndex(items)); return; }
      if (k === "End") { e.preventDefault(); e.stopPropagation(); focusAt(M.lastIndex(items)); return; }
      if (k === "ContextMenu" || (k === "F10" && e.shiftKey)) { e.preventDefault(); e.stopPropagation(); return; }
      if (k.length === 1 && !e.ctrlKey && !e.metaKey && !e.altKey && k !== " ") {
        e.stopPropagation();                                         // typing in a menu is never a page shortcut (for example ? must not open the sheet)
        const now = Date.now(); typed = now - typedAt > 700 ? k : typed + k; typedAt = now;
        const hit = M.typeahead(items, index(), typed); if (hit >= 0) { e.preventDefault(); focusAt(hit); }
        return;
      }
      e.stopPropagation();                                           // Enter / Space run the focused button natively; other keys do not leak to the page
    }
    const onDismiss = (e) => {
      if (e && e.type === "scroll") {
        if (e.target && menu.contains(e.target)) return;             // the menu scrolling itself is fine
        if (Date.now() - openedAt < 250) return;                      // scrolling caused by opening (the row scrolled into view) must not close it at once
      }
      close(false);
    };
    document.addEventListener("keydown", onKey, true);
    window.addEventListener("resize", onDismiss);
    window.addEventListener("scroll", onDismiss, true);
    cur = { layer, menu, opener, onKey, onDismiss }; openedAt = Date.now();
    focusAt(M.firstIndex(items));
    UI.announce((o.label || "Actions") + " menu, " + items.filter((x) => !x.separator).length + " actions");
    return { close: () => close(true), element: menu };
  }

  /**
   * Make every element matching `selector` inside `root` open a menu. opts: { selector, items: (target, event) => [..], label: (target) => string,
   * focusTarget: (target) => element to focus afterwards }. Delegated, so it survives re-rendering of the rows.
   */
  function attach(root, opts) {
    const press = M.longPress();
    let lastLongPress = 0;
    const targetOf = (n) => { const t = n && n.closest ? n.closest(opts.selector) : null; return t && root.contains(t) ? t : null; };
    function show(t, x, y, ev) {
      const items = opts.items(t, ev);
      const opener = (opts.focusTarget && opts.focusTarget(t)) || t.querySelector("button, [tabindex]") || t;
      return open({ items, x, y, opener, label: opts.label ? opts.label(t) : undefined });
    }
    root.addEventListener("contextmenu", (e) => {
      const t = targetOf(e.target); if (!t) return;
      e.preventDefault();
      if (cur) return;                                               // a long-press already opened it (touch also fires contextmenu), or a key did
      const keyboardLike = e.button === 0 && e.pointerType !== "touch" && e.pointerType !== "pen";   // a real mouse right-click has button 2; the keys give 0
      if (keyboardLike && Date.now() - keyOpenedAt < 600) return;     // the key press that opened a menu also produces a contextmenu event
      const synthetic = keyboardLike;                                  // keyboard-originated: anchor to the row instead of the corner
      if (synthetic) { const a = M.anchorFor(t.getBoundingClientRect()); show(t, a.x, a.y, e); } else show(t, e.clientX, e.clientY, e);
    });
    root.addEventListener("keydown", (e) => {
      if (!(e.key === "ContextMenu" || (e.key === "F10" && e.shiftKey)) || e.defaultPrevented) return;
      const t = targetOf(e.target); if (!t) return;
      e.preventDefault(); keyOpenedAt = Date.now();
      const a = M.anchorFor(t.getBoundingClientRect()); show(t, a.x, a.y, e);
    });
    root.addEventListener("pointerdown", (e) => {
      if (e.pointerType !== "touch" && e.pointerType !== "pen") return;
      const t = targetOf(e.target); if (!t) return;
      press.start(e.clientX, e.clientY, (x, y) => { lastLongPress = Date.now(); show(t, x, y, e); });
    });
    root.addEventListener("pointermove", (e) => press.move(e.clientX, e.clientY));
    ["pointerup", "pointercancel", "pointerleave"].forEach((n) => root.addEventListener(n, () => press.end()));
    root.addEventListener("scroll", () => press.cancel(), true);
    root.addEventListener("click", (e) => { if (lastLongPress && Date.now() - lastLongPress < 800) { lastLongPress = 0; e.preventDefault(); e.stopPropagation(); } }, true);        // the release after a long-press is not a tap
  }

  window.AuroraContextMenu = { open, attach, close: () => close(true), isOpen: () => !!cur };
})();
