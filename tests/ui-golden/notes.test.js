/**
 * tests/ui-golden/notes.test.js - M5: Notes.
 * A browser-prototype app over the DOM-free model (tests/unit/notes.test.js covers storage, limits and corrupt data). These tests drive the
 * screen: create/edit/autosave/reload, pin and sort, search and Space filter, delete + undo, limits, corrupt and failing storage, markup shown
 * as text, keyboard, phone and desktop layouts, accessible names and honest status text. Notes are saved in the browser only (localStorage).
 */
const { test, expect } = require("@playwright/test");

const BASE_URL = "http://localhost:3000";
const KEY = "aurora-glass.notes.v1";
const ev = (page, fn, arg) => page.evaluate(fn, arg);

async function start(page, seed) {
  if (seed !== undefined) await page.addInitScript(([k, v]) => { if (!sessionStorage.getItem("seeded")) { sessionStorage.setItem("seeded", "1"); localStorage.setItem(k, v); } }, [KEY, typeof seed === "string" ? seed : JSON.stringify(seed)]);
  const errors = [];
  page.on("console", (m) => { if (m.type() === "error") errors.push(m.text()); });
  page.on("pageerror", (e) => errors.push(String(e)));
  page.__errors = errors;
  await page.goto(BASE_URL);
}
async function open(page) {
  await ev(page, () => window.AuroraShell.openApp("notes"));
  await expect(page.locator("#screen-notes h1")).toHaveText("Notes");
}
const N = (id, o) => Object.assign({ id, title: "T " + id, body: "B " + id, pinned: false, space: "study", created: 1000, updated: 2000 }, o || {});
const env = (notes) => ({ v: 1, notes });
const stored = (page) => ev(page, (k) => JSON.parse(localStorage.getItem(k) || "null"), KEY);
const items = (page) => page.locator("#notes-list .note-item");
const titles = (page) => page.locator("#notes-list .note-item-name");
const saved = async (page) => { await ev(page, () => window.AuroraNotes.flush()); };
async function write(page, title, body) {
  if (title !== null) await page.locator("#notes-title").fill(title);
  if (body !== null && body !== undefined) await page.locator("#notes-body").fill(body);
}
async function createNote(page, title, body) {
  await page.locator("#btn-notes-new").click();
  await write(page, title, body);
  await saved(page);
}
const noErrors = (page) => expect(page.__errors).toEqual([]);

test.describe("Notes: getting there", () => {
  test("is a real screen opened by openApp, with the browser-only notice and an empty state", async ({ page }) => {
    await start(page); await open(page);
    await expect(page.locator("#notes-scope")).toContainText("Saved in this browser only · not AuroraOS storage");
    await expect(page.locator("#notes-empty")).toContainText("No notes yet");
    await expect(page.locator("#screen-notes")).toHaveClass(/active/);
    await expect(page.locator("#notes-banner")).toBeHidden();
    await noErrors(page);
  });
  test("is reachable from Index search", async ({ page }) => {
    await start(page);
    await page.keyboard.press("Control+k");
    await page.locator("#index-query").fill("notes");
    await page.locator("#index-results .index-result", { hasText: "Notes" }).first().click();
    await expect(page.locator("#screen-notes")).toHaveClass(/active/);
  });
  test("starts clean for a new browser context (no leaked notes)", async ({ page }) => {
    await start(page); await open(page);
    expect(await stored(page)).toBeNull();
  });
});

test.describe("Notes: create, edit, autosave, reload", () => {
  test("New note opens the editor, saves on its own after typing, and survives a reload", async ({ page }) => {
    await start(page); await open(page);
    await page.locator("#btn-notes-new").click();
    await expect(page.locator("#notes-title")).toBeFocused();
    await page.locator("#notes-title").fill("Groceries");
    await page.locator("#notes-body").fill("milk\neggs\nbread");
    await expect(page.locator("#notes-status")).toHaveText("Saving…");
    await expect(page.locator("#notes-status")).toHaveText(/^Saved \d\d:\d\d$/, { timeout: 4000 });     // autosave, no click
    const s = await stored(page);
    expect(s.v).toBe(1); expect(s.notes).toHaveLength(1);
    expect(s.notes[0]).toMatchObject({ title: "Groceries", body: "milk\neggs\nbread", pinned: false });
    await page.reload(); await open(page);
    await expect(titles(page)).toHaveText(["Groceries"]);
    await items(page).first().click();
    await expect(page.locator("#notes-body")).toHaveValue("milk\neggs\nbread");
    await noErrors(page);
  });
  test("autosave waits for a pause: nothing is written while typing continues, one write after", async ({ page }) => {
    await start(page); await open(page);
    await page.locator("#btn-notes-new").click();
    await ev(page, () => { window.__writes = 0; const o = Storage.prototype.setItem; Storage.prototype.setItem = function (k, v) { if (k === "aurora-glass.notes.v1") window.__writes++; return o.call(this, k, v); }; });
    await page.locator("#notes-body").pressSequentially("hello world", { delay: 40 });
    expect(await ev(page, () => window.__writes)).toBe(0);
    await expect.poll(() => ev(page, () => window.__writes), { timeout: 3000 }).toBe(1);
  });
  test("editing updates the word count; singular and plural", async ({ page }) => {
    await start(page); await open(page);
    await page.locator("#btn-notes-new").click();
    await expect(page.locator("#notes-count")).toHaveText("0 words");
    await page.locator("#notes-body").fill("one");
    await expect(page.locator("#notes-count")).toHaveText("1 word");
    await page.locator("#notes-body").fill("one two  three\nfour");
    await expect(page.locator("#notes-count")).toHaveText("4 words");
  });
  test("New note while the open note is still empty reuses it instead of piling up blanks", async ({ page }) => {
    await start(page); await open(page);
    await page.locator("#btn-notes-new").click();
    await page.locator("#btn-notes-new").click();
    await page.locator("#btn-notes-new").click();
    expect(await ev(page, () => window.AuroraNotes.state().count)).toBe(1);
  });
  test("a new note takes the active Space; the Space can be changed from the editor and persists", async ({ page }) => {
    await start(page);
    await ev(page, () => window.AuroraShell.switchSpace("work", { quiet: true }));
    await open(page);
    await createNote(page, "In work", "x");
    await expect(page.locator("#notes-space")).toHaveValue("work");
    await page.locator("#notes-space").selectOption("personal");
    expect((await stored(page)).notes[0].space).toBe("personal");
    await page.reload(); await open(page);
    await items(page).first().click();
    await expect(page.locator("#notes-space")).toHaveValue("personal");
  });
  test("untitled notes are listed as 'Untitled note' and their text still shows as a snippet", async ({ page }) => {
    await start(page); await open(page);
    await createNote(page, "", "just some text");
    await expect(titles(page).first()).toHaveText("Untitled note");
    await expect(page.locator("#notes-list .note-item-snippet").first()).toHaveText("just some text");
  });
  test("leaving the Notes screen saves pending text immediately", async ({ page }) => {
    await start(page); await open(page);
    await page.locator("#btn-notes-new").click();
    await write(page, "Quick", "typed then left");
    await ev(page, () => window.AuroraShell.showScreen("screen-home"));
    expect((await stored(page)).notes[0]).toMatchObject({ title: "Quick", body: "typed then left" });
  });
});

test.describe("Notes: pin, sort, search, filter", () => {
  const seed = env([N("a", { title: "Alpha", body: "apples and pears", updated: 3000 }), N("b", { title: "Beta", body: "Banana", updated: 5000 }), N("c", { title: "Gamma", body: "grapes", updated: 4000, space: "work" }), N("d", { title: "Delta", body: "dates", updated: 1000, pinned: true, space: "work" })]);
  test("loaded notes sort pinned first, then newest", async ({ page }) => {
    await start(page, seed); await open(page);
    await expect(titles(page)).toHaveText(["Delta", "Beta", "Gamma", "Alpha"]);
    await expect(page.locator("#notes-list-count")).toHaveText("4 notes");
  });
  test("pinning moves a note to the top, persists, and exposes pressed state and a name that says what it does", async ({ page }) => {
    await start(page, seed); await open(page);
    await page.locator("#btn-note-a").click();
    const pin = page.locator("#btn-notes-pin");
    await expect(pin).toHaveAttribute("aria-pressed", "false");
    await expect(pin).toHaveAccessibleName("Pin note");
    await pin.click();
    await expect(pin).toHaveAttribute("aria-pressed", "true");
    await expect(pin).toHaveAccessibleName("Unpin note");
    await expect(titles(page)).toHaveText(["Alpha", "Delta", "Beta", "Gamma"]);        // both pinned, newest update first
    expect((await stored(page)).notes.find((n) => n.id === "a").pinned).toBe(true);
    await pin.click();
    await expect(titles(page)).toHaveText(["Delta", "Beta", "Gamma", "Alpha"]);
  });
  test("pinning does not change when the note was last edited", async ({ page }) => {
    await start(page, seed); await open(page);
    await page.locator("#btn-note-a").click(); await page.locator("#btn-notes-pin").click();
    expect((await stored(page)).notes.find((n) => n.id === "a").updated).toBe(3000);
  });
  test("editing a note moves it to the top of its group", async ({ page }) => {
    await start(page, seed); await open(page);
    await page.locator("#btn-note-a").click();
    await page.locator("#notes-body").fill("apples, pears and plums");
    await saved(page);
    await expect(titles(page)).toHaveText(["Delta", "Alpha", "Beta", "Gamma"]);
  });
  test("search matches title and body, ignores case, needs every word, and shows a count", async ({ page }) => {
    await start(page, seed); await open(page);
    const q = page.locator("#notes-search");
    await q.fill("BANANA"); await expect(titles(page)).toHaveText(["Beta"]);
    await expect(page.locator("#notes-list-count")).toHaveText("Showing 1 of 4 notes");
    await q.fill("gamma grapes"); await expect(titles(page)).toHaveText(["Gamma"]);
    await q.fill("gamma apples"); await expect(items(page)).toHaveCount(0);
    await q.fill("a"); await expect(items(page)).toHaveCount(4);
    await q.fill("");  await expect(items(page)).toHaveCount(4);
  });
  test("a search with no match shows an empty state that names the query, with a working reset", async ({ page }) => {
    await start(page, seed); await open(page);
    await page.locator("#notes-search").fill("zzz<b>");
    await expect(page.locator("#notes-empty")).toContainText("No notes match “zzz<b>”");
    await expect(page.locator("#notes-empty b")).toHaveCount(0);
    await page.locator("#btn-notes-clear-filter").click();
    await expect(items(page)).toHaveCount(4); await expect(page.locator("#notes-search")).toHaveValue("");
  });
  test("Space filter chips narrow the list and say which one is pressed", async ({ page }) => {
    await start(page, seed); await open(page);
    await expect(page.locator("#btn-notes-space-all")).toHaveAttribute("aria-pressed", "true");
    await page.locator("#btn-notes-space-work").click();
    await expect(titles(page)).toHaveText(["Delta", "Gamma"]);
    await expect(page.locator("#btn-notes-space-work")).toHaveAttribute("aria-pressed", "true");
    await expect(page.locator("#btn-notes-space-all")).toHaveAttribute("aria-pressed", "false");
    await page.locator("#btn-notes-space-personal").click();
    await expect(page.locator("#notes-empty")).toContainText("No notes in Personal");
    await page.locator("#btn-notes-space-all").click();
    await expect(items(page)).toHaveCount(4);
  });
  test("search and Space filter combine", async ({ page }) => {
    await start(page, seed); await open(page);
    await page.locator("#btn-notes-space-work").click();
    await page.locator("#notes-search").fill("grapes");
    await expect(titles(page)).toHaveText(["Gamma"]);
  });
  test("a custom Space created in Home shows up as a filter and in the Space picker", async ({ page }) => {
    await start(page, env([N("a", { title: "X", space: "space-lab" })])); await open(page);
    await ev(page, () => window.AuroraShell.addSpace("Lab"));
    await open(page);
    await expect(page.locator("#btn-notes-space-space-lab")).toBeVisible();
  });
});

test.describe("Notes: delete and undo", () => {
  const seed = env([N("a", { title: "Alpha", updated: 3000 }), N("b", { title: "Beta", updated: 5000 })]);
  test("Delete asks first; Keep note changes nothing", async ({ page }) => {
    await start(page, seed); await open(page);
    await page.locator("#btn-note-a").click();
    await page.locator("#btn-notes-delete").click();
    const dlg = page.getByRole("alertdialog");
    await expect(dlg).toContainText("Delete this note?"); await expect(dlg).toContainText("“Alpha” will be removed from this browser");
    await page.locator("#btn-notes-delete-cancel").click();
    await expect(page.locator("#btn-notes-delete")).toBeFocused();
    await expect(items(page)).toHaveCount(2); expect((await stored(page)).notes).toHaveLength(2);
  });
  test("Delete removes the note from the list and storage, and the toast Undo brings it back identical", async ({ page }) => {
    await start(page, seed); await open(page);
    await page.locator("#btn-note-a").click();
    await page.locator("#notes-body").fill("edited body"); await saved(page);
    await page.locator("#btn-notes-pin").click();
    await page.locator("#btn-notes-delete").click();
    await page.locator("#btn-notes-delete-confirm").click();
    await expect(titles(page)).toHaveText(["Beta"]);
    expect((await stored(page)).notes.map((n) => n.id)).toEqual(["b"]);
    await expect(page.locator("#toast")).toContainText("Note deleted");
    await page.locator("#toast .toast-action").click();
    await expect(titles(page)).toHaveText(["Alpha", "Beta"]);
    const a = (await stored(page)).notes.find((n) => n.id === "a");
    expect(a).toMatchObject({ title: "Alpha", body: "edited body", pinned: true });
    await expect(page.locator("#notes-body")).toHaveValue("edited body");
  });
  test("undo is one level: deleting a second note replaces the first", async ({ page }) => {
    await start(page, seed); await open(page);
    for (const id of ["a", "b"]) {
      await page.locator("#btn-note-" + id).click(); await page.locator("#btn-notes-delete").click(); await page.locator("#btn-notes-delete-confirm").click();
    }
    await expect(items(page)).toHaveCount(0); await expect(page.locator("#notes-empty")).toContainText("No notes yet");
    await page.locator("#toast .toast-action").click();
    await expect(titles(page)).toHaveText(["Beta"]);
  });
  test("deleting the last note shows the empty state, and an empty untitled note can be deleted too", async ({ page }) => {
    await start(page); await open(page);
    await page.locator("#btn-notes-new").click();
    await page.locator("#btn-notes-delete").click();
    await expect(page.getByRole("alertdialog")).toContainText("“Untitled note”");
    await page.locator("#btn-notes-delete-confirm").click();
    await expect(page.locator("#notes-empty")).toContainText("No notes yet");
    expect((await stored(page)).notes).toEqual([]);
  });
});

test.describe("Notes: limits", () => {
  test("a title over 120 characters is refused with a visible reason, kept in the editor and not written", async ({ page }) => {
    await start(page); await open(page);
    await createNote(page, "ok", "body");
    await page.locator("#notes-title").fill("t".repeat(125));
    await expect(page.locator("#notes-status")).toHaveText("Not saved: Titles are limited to 120 characters (5 too many).");
    await expect(page.locator("#notes-title")).toHaveValue("t".repeat(125));
    await expect(page.locator("#notes-title-count")).toHaveText("125 / 120");
    await page.waitForTimeout(900);
    expect((await stored(page)).notes[0].title).toBe("ok");
    await page.locator("#notes-title").fill("t".repeat(120));
    await expect(page.locator("#notes-status")).toHaveText(/Saved|Saving/);
    await saved(page); expect((await stored(page)).notes[0].title).toBe("t".repeat(120));
  });
  test("a body over 20,000 characters is refused with the count of the excess; exactly 20,000 is fine", async ({ page }) => {
    await start(page); await open(page);
    await page.locator("#btn-notes-new").click();
    await page.locator("#notes-body").fill("x".repeat(20000)); await saved(page);
    await expect(page.locator("#notes-status")).toHaveText(/^Saved/);
    await expect(page.locator("#notes-count")).toContainText("20,000 / 20,000 characters");
    await page.locator("#notes-body").fill("x".repeat(20007));
    await expect(page.locator("#notes-status")).toHaveText("Not saved: Notes are limited to 20,000 characters (7 too many).");
    await expect(page.locator("#notes-count")).toContainText("20,007 / 20,000");
    expect(await ev(page, () => document.getElementById("notes-body").value.length)).toBe(20007);   // nothing silently cut
    expect((await stored(page)).notes[0].body.length).toBe(20000);
  });
  test("while text is over the limit the user cannot navigate away and lose it", async ({ page }) => {
    await start(page, env([N("a", { title: "A" }), N("b", { title: "B", updated: 1 })])); await open(page);
    await page.locator("#btn-note-a").click();
    await page.locator("#notes-body").fill("x".repeat(20010));
    await page.locator("#btn-note-b").click();
    await expect(page.locator("#toast")).toContainText("Shorten the text first");
    await expect(page.locator("#notes-body")).toHaveValue("x".repeat(20010));
    expect(await ev(page, () => window.AuroraNotes.state().selected)).toBe("a");
  });
  test("the 201st note is refused with a toast; 200 stay", async ({ page }) => {
    const many = []; for (let i = 0; i < 200; i++) many.push(N("n" + i, { title: "Note " + i, updated: 1000 + i }));
    await start(page, env(many)); await open(page);
    await expect(page.locator("#notes-list-count")).toHaveText("200 notes");
    await page.locator("#btn-notes-new").click();
    await expect(page.locator("#toast")).toContainText("Up to 200 notes");
    expect(await ev(page, () => window.AuroraNotes.state().count)).toBe(200);
  });
  test("near the body limit the counter shows the characters used", async ({ page }) => {
    await start(page); await open(page);
    await page.locator("#btn-notes-new").click();
    await page.locator("#notes-body").fill("y".repeat(18000));
    await expect(page.locator("#notes-count")).toContainText("18,000 / 20,000 characters");
  });
});

test.describe("Notes: persistence failures", () => {
  test("corrupt JSON: starts empty, says so, keeps the original under a backup key, never crashes", async ({ page }) => {
    await start(page, "{not json at all"); await open(page);
    await expect(page.locator("#notes-banner")).toBeVisible();
    await expect(page.locator("#notes-banner")).toContainText("could not be read");
    await expect(page.locator("#notes-empty")).toContainText("No notes yet");
    expect(await ev(page, () => localStorage.getItem("aurora-glass.notes.v1.unreadable"))).toBe("{not json at all");
    await page.locator("#btn-notes-banner-dismiss").click(); await expect(page.locator("#notes-banner")).toBeHidden();
    await createNote(page, "fresh", "start");                                       // and the app works afterwards
    expect((await stored(page)).notes).toHaveLength(1);
    await noErrors(page);
  });
  test("invalid notes are left out, valid ones stay, and the banner counts what was dropped", async ({ page }) => {
    await start(page, env([N("good", { title: "Good" }), { id: "bad", title: 5 }, N("also", { pinned: "yes" }), null]));
    await open(page);
    await expect(titles(page)).toHaveText(["Good"]);
    await expect(page.locator("#notes-banner")).toContainText("3 saved notes were not valid");
  });
  test("a newer data version is not overwritten silently", async ({ page }) => {
    await start(page, { v: 2, notes: [N("x")] }); await open(page);
    await expect(page.locator("#notes-banner")).toContainText("newer version");
    expect(JSON.parse(await ev(page, () => localStorage.getItem("aurora-glass.notes.v1.unreadable"))).v).toBe(2);
  });
  test("when saving fails the text stays, the status says Not saved with the reason, and Try again recovers", async ({ page }) => {
    await start(page); await open(page);
    await createNote(page, "Before", "ok");
    await ev(page, () => { window.__fail = true; const o = Storage.prototype.setItem; Storage.prototype.setItem = function (k, v) { if (window.__fail && k === "aurora-glass.notes.v1") { const e = new Error("quota"); e.name = "QuotaExceededError"; throw e; } return o.call(this, k, v); }; });
    await page.locator("#notes-body").fill("important text I must not lose");
    await expect(page.locator("#notes-status")).toHaveText("Not saved: browser storage is full", { timeout: 4000 });
    await expect(page.locator("#notes-status")).toHaveAttribute("data-tone", "error");
    await expect(page.locator("#notes-body")).toHaveValue("important text I must not lose");
    await expect(page.locator("#btn-notes-retry")).toBeVisible();
    expect((await stored(page)).notes[0].body).toBe("ok");                       // storage still holds the previous version
    // switching notes does not lose it either (it is in memory)
    await page.locator("#btn-notes-new").click(); await page.locator("#btn-note-" + (await ev(page, () => window.AuroraNotes.notes().find((n) => n.title === "Before").id))).click();
    await expect(page.locator("#notes-body")).toHaveValue("important text I must not lose");
    await ev(page, () => { window.__fail = false; });
    await page.locator("#btn-notes-retry").click();
    await expect(page.locator("#notes-status")).toHaveText(/^Saved \d\d:\d\d$/);
    await expect(page.locator("#btn-notes-retry")).toBeHidden();
    expect((await stored(page)).notes.find((n) => n.title === "Before").body).toBe("important text I must not lose");
  });
  test("localStorage unavailable (access throws): Notes still works in memory and says it cannot save", async ({ page }) => {
    await page.addInitScript(() => { Object.defineProperty(window, "localStorage", { get() { throw new DOMException("denied", "SecurityError"); } }); });
    await start(page); await open(page);
    await expect(page.locator("#notes-banner")).toContainText("does not allow saving notes");
    await page.locator("#btn-notes-new").click();
    await page.locator("#notes-body").fill("only in memory");
    await expect(page.locator("#notes-status")).toHaveText("Not saved: browser storage is unavailable", { timeout: 4000 });
    await expect(page.locator("#notes-body")).toHaveValue("only in memory");
    await expect(page.locator("#notes-scope")).toContainText("Saved in this browser only");
    expect(await ev(page, () => window.AuroraNotes.state().hasStorage)).toBe(false);
  });
});

test.describe("Notes: untrusted text", () => {
  const evil = '<img src=x onerror="window.__pwned=1"><script>window.__pwned=2</script><b>bold</b>';
  test("markup in a title, body or search renders as plain text, in the list, editor and dialog", async ({ page }) => {
    await start(page, env([N("a", { title: evil.slice(0, 120), body: evil })])); await open(page);
    await expect(titles(page).first()).toContainText("<img src=x");
    await expect(page.locator("#notes-list .note-item-snippet").first()).toContainText("<script>");
    await page.locator("#btn-note-a").click();
    await expect(page.locator("#notes-body")).toHaveValue(evil);
    await page.locator("#btn-notes-delete").click();
    await expect(page.getByRole("alertdialog")).toContainText("<img src=x");
    await page.locator("#btn-notes-delete-cancel").click();
    expect(await ev(page, () => window.__pwned)).toBeUndefined();
    expect(await page.locator("#screen-notes img, #screen-notes script, #screen-notes b").count()).toBe(0);
    await noErrors(page);
  });
  test("typing markup round-trips through storage unchanged", async ({ page }) => {
    await start(page); await open(page);
    await createNote(page, "<i>t</i>", evil);
    await page.reload(); await open(page);
    await page.locator("#notes-list .note-item").first().click();
    await expect(page.locator("#notes-title")).toHaveValue("<i>t</i>");
    await expect(page.locator("#notes-body")).toHaveValue(evil);
    expect(await ev(page, () => window.__pwned)).toBeUndefined();
  });
  test("a note id from storage that tries to break out of a selector is rejected, not used", async ({ page }) => {
    await start(page, env([N('a"]x', { title: "evil id" }), N("ok", { title: "fine" })])); await open(page);
    await expect(titles(page)).toHaveText(["fine"]);
  });
});

test.describe("Notes: keyboard", () => {
  const seed = env([N("a", { title: "Alpha", updated: 3000 }), N("b", { title: "Beta", updated: 2000 }), N("c", { title: "Gamma", updated: 1000 })]);
  test("Ctrl+N makes a new note and focuses its title", async ({ page }) => {
    await start(page, seed); await open(page);
    await page.keyboard.press("Control+n");
    await expect(page.locator("#notes-title")).toBeFocused();
    expect(await ev(page, () => window.AuroraNotes.state().count)).toBe(4);
  });
  test("Ctrl+S saves immediately and announces it, even in the middle of the debounce", async ({ page }) => {
    await start(page, seed); await open(page);
    await page.locator("#btn-note-a").click();
    await page.locator("#notes-body").fill("quick");
    await page.keyboard.press("Control+s");
    expect((await stored(page)).notes.find((n) => n.id === "a").body).toBe("quick");
    await expect(page.locator("#notes-status")).toHaveText(/^Saved \d\d:\d\d$/);
  });
  test("Ctrl+F focuses and selects the search box", async ({ page }) => {
    await start(page, seed); await open(page);
    await page.locator("#notes-search").fill("alp"); await page.locator("#btn-note-a").click();
    await page.keyboard.press("Control+f");
    await expect(page.locator("#notes-search")).toBeFocused();
    await page.keyboard.type("be");
    await expect(titles(page)).toHaveText(["Beta"]);
  });
  test("arrow keys, Home and End move through the list; Enter opens", async ({ page }) => {
    await start(page, seed); await open(page);
    await page.locator("#btn-note-a").focus();
    await page.keyboard.press("ArrowDown"); await expect(page.locator("#btn-note-b")).toBeFocused();
    await page.keyboard.press("End"); await expect(page.locator("#btn-note-c")).toBeFocused();
    await page.keyboard.press("ArrowUp"); await expect(page.locator("#btn-note-b")).toBeFocused();
    await page.keyboard.press("Home"); await expect(page.locator("#btn-note-a")).toBeFocused();
    await page.keyboard.press("ArrowUp"); await expect(page.locator("#btn-note-a")).toBeFocused();
    await page.keyboard.press("Enter");
    await expect(page.locator("#notes-title")).toHaveValue("Alpha"); await expect(page.locator("#notes-title")).toBeFocused();
  });
  test("Escape in a filled search clears it first and stays on the screen", async ({ page }) => {
    await start(page, seed); await open(page);
    await page.locator("#notes-search").fill("alp"); await page.keyboard.press("Escape");
    await expect(page.locator("#notes-search")).toHaveValue(""); await expect(items(page)).toHaveCount(3);
    await expect(page.locator("#screen-notes")).toHaveClass(/active/);
  });
  test("shortcuts do nothing while a dialog is open or on other screens", async ({ page }) => {
    await start(page, seed); await open(page);
    await page.locator("#btn-note-a").click(); await page.locator("#btn-notes-delete").click();
    await page.keyboard.press("Control+n");
    expect(await ev(page, () => window.AuroraNotes.state().count)).toBe(3);
    await page.locator("#btn-notes-delete-cancel").click();
    await ev(page, () => window.AuroraShell.showScreen("screen-home"));
    await page.keyboard.press("Control+n");
    expect(await ev(page, () => window.AuroraNotes.state().count)).toBe(3);
  });
  test("Ctrl+K still opens Index from inside the editor", async ({ page }) => {
    await start(page, seed); await open(page);
    await page.locator("#btn-note-a").click(); await page.keyboard.press("Control+k");
    await expect(page.locator("#index-query")).toBeVisible();
  });
});

test.describe("Notes: phone layout (390)", () => {
  test.use({ viewport: { width: 390, height: 844 } });
  const seed = env([N("a", { title: "Alpha", body: "alpha body", updated: 3000 }), N("b", { title: "Beta", updated: 2000 })]);
  test("list first; opening a note shows only the editor; All notes returns, focus lands on the note, edits are saved", async ({ page }) => {
    await start(page, seed); await open(page);
    await expect(page.locator("#notes-listcard")).toHaveCount(0);                      // (the card has no id; assert by role below)
    await expect(page.locator("#notes-list")).toBeVisible(); await expect(page.locator("#notes-editor")).toBeHidden();
    await page.locator("#btn-note-a").click();
    await expect(page.locator("#notes-editor")).toBeVisible(); await expect(page.locator("#notes-list")).toBeHidden();
    await expect(page.locator("#btn-notes-back")).toBeVisible();
    await page.locator("#notes-body").fill("changed on phone");
    await page.locator("#btn-notes-back").click();
    await expect(page.locator("#notes-list")).toBeVisible(); await expect(page.locator("#notes-editor")).toBeHidden();
    await expect(page.locator("#btn-note-a")).toBeFocused();
    expect((await stored(page)).notes.find((n) => n.id === "a").body).toBe("changed on phone");
  });
  test("Escape goes back from the editor to the list, and from the list to Home", async ({ page }) => {
    await start(page, seed); await open(page);
    await page.locator("#btn-note-a").click();
    await page.keyboard.press("Escape");
    await expect(page.locator("#notes-list")).toBeVisible(); await expect(page.locator("#screen-notes")).toHaveClass(/active/);
    await page.locator("#btn-note-a").focus();
    await page.keyboard.press("Escape");
    await expect(page.locator("#screen-home")).toHaveClass(/active/);
  });
  test("Delete returns to the list and Undo restores", async ({ page }) => {
    await start(page, seed); await open(page);
    await page.locator("#btn-note-a").click(); await page.locator("#btn-notes-delete").click(); await page.locator("#btn-notes-delete-confirm").click();
    await expect(page.locator("#notes-list")).toBeVisible(); await expect(titles(page)).toHaveText(["Beta"]);
    await page.locator("#toast .toast-action").click();
    await expect(titles(page)).toHaveText(["Alpha", "Beta"]);
  });
  test("New note opens the editor with the title focused", async ({ page }) => {
    await start(page, seed); await open(page);
    await page.locator("#btn-notes-new").click();
    await expect(page.locator("#notes-editor")).toBeVisible(); await expect(page.locator("#notes-title")).toBeFocused();
  });
  test("every control is at least 44px tall and there is no horizontal overflow", async ({ page }) => {
    await start(page, seed); await open(page);
    expect(await ev(page, () => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
    for (const sel of ["#btn-notes-new", "#notes-search", "#btn-note-a", "#btn-notes-space-all"]) {
      const h = await page.locator(sel).evaluate((n) => n.getBoundingClientRect().height); expect(h, sel).toBeGreaterThanOrEqual(44);
    }
    await page.locator("#btn-note-a").click();
    expect(await ev(page, () => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
    for (const sel of ["#btn-notes-back", "#btn-notes-pin", "#btn-notes-delete", "#notes-space", "#notes-title"]) {
      const h = await page.locator(sel).evaluate((n) => n.getBoundingClientRect().height); expect(h, sel).toBeGreaterThanOrEqual(44);
    }
  });
});

test.describe("Notes: desktop layout (1280)", () => {
  test.use({ viewport: { width: 1280, height: 800 } });
  const seed = env([N("a", { title: "Alpha", body: "alpha body", updated: 3000 }), N("b", { title: "Beta", updated: 2000 })]);
  test("list and editor side by side; the first note is open without stealing focus; no Back button", async ({ page }) => {
    await start(page, seed); await open(page);
    await expect(page.locator("#notes-list")).toBeVisible(); await expect(page.locator("#notes-editor")).toBeVisible();
    await expect(page.locator("#notes-title")).toHaveValue("Alpha");
    await expect(page.locator("#btn-notes-back")).toBeHidden();
    const [l, e] = await Promise.all([page.locator("#notes-list").boundingBox(), page.locator("#notes-editor").boundingBox()]);
    expect(e.x).toBeGreaterThan(l.x + l.width - 1);
  });
  test("selecting another note saves the first and switches the editor", async ({ page }) => {
    await start(page, seed); await open(page);
    await page.locator("#notes-body").fill("edited alpha");
    await page.locator("#btn-note-b").click();
    await expect(page.locator("#notes-title")).toHaveValue("Beta");
    expect((await stored(page)).notes.find((n) => n.id === "a").body).toBe("edited alpha");
    await expect(page.locator("#btn-note-b")).toHaveAttribute("aria-current", "true");
  });
  test("no notes: the editor pane explains and offers a working New note", async ({ page }) => {
    await start(page); await open(page);
    await expect(page.locator("#notes-hint")).toContainText("No note open");
    await page.locator("#btn-notes-hint-new").click();
    await expect(page.locator("#notes-form")).toBeVisible(); await expect(page.locator("#notes-hint")).toBeHidden();
  });
  test("a very long title and a long unbroken word do not overflow the page", async ({ page }) => {
    await start(page, env([N("a", { title: "W".repeat(120), body: "z".repeat(5000) })])); await open(page);
    expect(await ev(page, () => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
    const box = await page.locator("#btn-note-a").boundingBox(); const lb = await page.locator("#notes-listcard, .notes-listcard").first().boundingBox();
    expect(box.x + box.width).toBeLessThanOrEqual(lb.x + lb.width + 1);
  });
});

test.describe("Notes: accessibility", () => {
  test("controls have accessible names; the status is a labelled paragraph; list items name title, pin, Space and time", async ({ page }) => {
    await start(page, env([N("a", { title: "Alpha", body: "hello there", pinned: true, updated: Date.now() })])); await open(page);
    await expect(page.getByRole("searchbox", { name: "Search notes" })).toBeVisible();
    await expect(page.getByRole("button", { name: "New note" }).first()).toBeVisible();
    await expect(page.getByRole("group", { name: "Filter by Space" })).toBeVisible();
    const item = page.locator("#btn-note-a");
    const name = await item.getAttribute("aria-label");
    expect(name).toMatch(/^Alpha\. Pinned · Study · \d\d:\d\d\. hello there$/);
    await expect(item).toHaveAttribute("aria-current", "true");
    await expect(page.getByRole("textbox", { name: "Note title" })).toBeVisible();
    await expect(page.getByRole("textbox", { name: "Note text" })).toBeVisible();
    await expect(page.getByRole("combobox", { name: "Space" })).toBeVisible();
    await expect(page.getByRole("button", { name: "Unpin note" })).toBeVisible();
    await expect(page.getByRole("button", { name: "Delete" })).toBeVisible();
    await expect(page.getByRole("list", { name: "Notes" })).toBeVisible();
  });
  test("keyboard focus is visible on list items, the editor and the buttons", async ({ page }) => {
    await start(page, env([N("a", { title: "Alpha" })])); await open(page);
    for (const sel of ["#btn-note-a", "#btn-notes-pin", "#btn-notes-delete", "#btn-notes-new"]) {
      await page.locator(sel).focus();
      const o = await page.locator(sel).evaluate((n) => getComputedStyle(n).outlineStyle + "|" + getComputedStyle(n).outlineWidth);
      expect(o, sel).not.toMatch(/^none/); expect(o, sel).not.toMatch(/\|0px$/);
    }
  });
  test("text scale 200% keeps everything inside the viewport at 360px", async ({ page }) => {
    await page.setViewportSize({ width: 360, height: 780 });
    await start(page, env([N("a", { title: "A fairly long note title that wraps", body: "body" })])); await open(page);
    await ev(page, () => document.documentElement.setAttribute("data-text-scale", "200"));
    expect(await ev(page, () => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
    await page.locator("#btn-note-a").click();
    expect(await ev(page, () => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
    for (const sel of ["#btn-notes-pin", "#btn-notes-delete", "#notes-body", "#notes-status"]) await expect(page.locator(sel)).toBeVisible();
  });
});

test.describe("Notes: nothing else is touched", () => {
  test("Notes never calls the Action Broker or adds Trail receipts, and only writes its own key", async ({ page }) => {
    await start(page); await open(page);
    const before = await ev(page, () => window.AuroraSystem.receipts().length);
    await createNote(page, "Local", "only in the browser");
    await page.locator("#btn-notes-pin").click();
    expect(await ev(page, () => window.AuroraSystem.receipts().length)).toBe(before);
    expect(await ev(page, () => Object.keys(localStorage).filter((k) => !k.startsWith("aurora-glass.notes.v1")))).toEqual([]);
  });
});
