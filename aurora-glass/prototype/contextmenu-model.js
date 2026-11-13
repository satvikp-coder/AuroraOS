/* aurora-glass/prototype/contextmenu-model.js - DOM-free logic for the shared context menu (window.AuroraContextMenuModel).
 *
 * SIMULATION ONLY. Pure functions: where a menu of a given size goes inside a viewport (flip, then clamp, never off screen), which item the arrow keys,
 * Home/End and type-ahead land on (skipping separators, wrapping), how a touch long-press is told apart from a scroll or a tap, and how the item list is
 * cleaned. Loadable in the browser and in Node. No menu item can authorise anything: an item only runs the callback its owner gave it.
 */
(function (root, factory) {
  if (typeof module === "object" && module.exports) module.exports = factory();
  else root.AuroraContextMenuModel = factory();
})(typeof self !== "undefined" ? self : this, function () {
  "use strict";

  const MARGIN = 8, LONG_PRESS_MS = 500, MOVE_TOLERANCE = 10;
  const num = (n, d) => (typeof n === "number" && isFinite(n) ? n : d);

  /**
   * Place a w x h menu with its top-left corner at the pointer/anchor (x, y) inside a vw x vh viewport.
   * If it would run off the right or bottom edge it opens the other way (right edge / bottom edge at the point), then it is clamped so it is always
   * fully inside the margin. If it is taller or wider than the space available it is shrunk (maxHeight / maxWidth) so it can scroll rather than clip.
   */
  function place(x, y, w, h, vw, vh, margin) {
    const m = num(margin, MARGIN);
    vw = Math.max(0, num(vw, 0)); vh = Math.max(0, num(vh, 0));
    const maxWidth = Math.max(0, vw - m * 2), maxHeight = Math.max(0, vh - m * 2);
    const width = Math.min(Math.max(0, num(w, 0)), maxWidth), height = Math.min(Math.max(0, num(h, 0)), maxHeight);
    x = num(x, m); y = num(y, m);
    let left = x, top = y, flippedX = false, flippedY = false;
    if (left + width > vw - m) { left = x - width; flippedX = true; }
    if (top + height > vh - m) { top = y - height; flippedY = true; }
    left = Math.min(Math.max(left, m), Math.max(m, vw - m - width));
    top = Math.min(Math.max(top, m), Math.max(m, vh - m - height));
    return { left, top, width, height, maxWidth, maxHeight, flippedX, flippedY, scrolls: num(h, 0) > maxHeight };
  }

  /** Clean a caller's item list: drop falsy entries, collapse repeated or leading/trailing separators, require a label and an action on real items. */
  function normalise(items) {
    const out = [];
    (Array.isArray(items) ? items : []).forEach((it) => {
      if (!it) return;
      if (it.separator) { if (out.length && !out[out.length - 1].separator) out.push({ separator: true }); return; }
      if (typeof it.label !== "string" || !it.label.trim()) return;
      if (typeof it.action !== "function" && !it.disabled) return;
      out.push(it);
    });
    while (out.length && out[out.length - 1].separator) out.pop();
    return out;
  }

  const isItem = (it) => !!it && !it.separator;
  /** Index of the first focusable item (disabled items stay focusable so their reason can be read), or -1. */
  function firstIndex(items) { for (let i = 0; i < items.length; i++) if (isItem(items[i])) return i; return -1; }
  function lastIndex(items) { for (let i = items.length - 1; i >= 0; i--) if (isItem(items[i])) return i; return -1; }
  /** Next item index in a direction (+1 / -1), wrapping and skipping separators. */
  function step(items, from, dir) {
    const n = items.length; if (!n || firstIndex(items) < 0) return -1;
    let i = typeof from === "number" && from >= 0 && from < n ? from : (dir > 0 ? -1 : n);
    for (let k = 0; k < n; k++) { i = (i + (dir > 0 ? 1 : -1) + n) % n; if (isItem(items[i])) return i; }
    return -1;
  }
  /** Type-ahead: the next item (after `from`, wrapping) whose label starts with the typed text, case-insensitively. A repeated single letter cycles. */
  function typeahead(items, from, typed) {
    const q = String(typed || "").toLowerCase(); if (!q) return -1;
    const n = items.length, start = typeof from === "number" && from >= 0 ? from : -1;
    const cycle = q.length > 1 && q.split("").every((c) => c === q[0]);
    const needle = cycle ? q[0] : q;
    for (let k = 1; k <= n; k++) {
      const i = (start + k + n) % n, it = items[i];
      if (isItem(it) && it.label.toLowerCase().indexOf(needle) === 0) return i;
    }
    return -1;
  }

  /** Long-press tracker for touch/pen. start() on pointerdown, move() on pointermove, end() on pointerup/cancel/scroll. Timer is injected for tests. */
  function longPress(opts) {
    const o = opts || {};
    const ms = num(o.ms, LONG_PRESS_MS), tol = num(o.tolerance, MOVE_TOLERANCE);
    const set = o.setTimeout || setTimeout, clear = o.clearTimeout || clearTimeout;
    let timer = null, sx = 0, sy = 0, fired = false;
    const cancel = () => { if (timer !== null) { clear(timer); timer = null; } };
    return {
      start(x, y, onFire) { cancel(); fired = false; sx = x; sy = y; timer = set(() => { timer = null; fired = true; onFire(sx, sy); }, ms); },
      move(x, y) { if (timer !== null && Math.hypot(x - sx, y - sy) > tol) cancel(); },
      end() { cancel(); },
      cancel,
      pending: () => timer !== null,
      fired: () => fired,
      reset() { fired = false; },
    };
  }

  /** Where a keyboard-opened menu anchors for an element's rectangle: just below its start, so the row stays visible. */
  function anchorFor(rect) { return { x: num(rect && rect.left, 0) + Math.min(24, Math.max(0, num(rect && rect.width, 0) / 2)), y: num(rect && rect.bottom, 0) - Math.min(8, Math.max(0, num(rect && rect.height, 0) / 4)) }; }

  return { MARGIN, LONG_PRESS_MS, MOVE_TOLERANCE, place, normalise, firstIndex, lastIndex, step, typeahead, longPress, anchorFor };
});
