/**
 * tests/ui-golden/trail.test.js — M3: Trail.
 * Trail has no history of its own: every row is a receipt the prototype Action Broker recorded. These tests produce that
 * history with REAL broker operations (AuroraSystem.request through the trusted review dialog) and then check what Trail
 * shows and what Undo/Restore really do. Nothing here inserts fake receipts.
 */
const { test, expect } = require("@playwright/test");

const BASE_URL = "http://localhost:3000";
const ev = (page, fn, arg) => page.evaluate(fn, arg);

/** Send a request through the real review surface and drive its dialog like a person: approve, hold (S3) or cancel. */
async function ask(page, payload, opts) {
  const o = Object.assign({ approve: true, source: "Test", requestText: undefined }, opts || {});
  await ev(page, (a) => { window.__res = null; window.AuroraSystem.request(a.p, { source: a.s, requestText: a.t }).then((r) => { window.__res = r; }); }, { p: payload, s: o.source, t: o.requestText });
  await page.waitForFunction(() => window.__res || document.querySelector("#review-dialog"));
  if (await page.locator("#review-dialog").count()) {
    if (!o.approve) await page.locator("#btn-review-cancel").click();
    else if (await page.locator(".hold-alt").count()) { await page.locator(".hold-alt").click(); await page.locator(".hold-alt").click(); }
    else await page.locator("#btn-review-approve").click();
  }
  await page.waitForFunction(() => window.__res);
  return ev(page, () => window.__res);
}
const move = (id, destination) => ({ type: "file.move", arguments: { id, destination } });
const del = (id) => ({ type: "file.delete", arguments: { id } });

async function start(page) {
  await page.goto(BASE_URL);
  await ev(page, () => { window.AuroraSystem.dev.stepDelayMs = 0; });
}
async function openTrail(page) { await ev(page, () => window.AuroraShell.openApp("trail")); await expect(page.locator("#screen-trail h1")).toHaveText("Trail"); }
const rows = (page) => page.locator("#screen-trail .trail-row");
const word = (w) => new RegExp("^" + w + "$");
const rowFor = (page, text) => page.locator("#screen-trail .trail-row", { hasText: text });
const files = (page) => ev(page, () => window.AuroraSystem.files.list().map((f) => f.id + ":" + f.space).sort());

test.describe("Trail: getting there and empty state", () => {
  test("is a real screen: opened by openApp, Index and the Glass Corner; empty before anything happens", async ({ page }) => {
    await start(page);
    expect(await ev(page, () => window.AuroraShell.openApp("trail"))).toMatchObject({ ok: true, id: "trail" });
    await expect(page.locator("#screen-trail")).toBeVisible();
    await expect(page.locator("#screen-trail .empty-title").first()).toHaveText("Nothing has happened yet");
    await expect(rows(page)).toHaveCount(0);
    await ev(page, () => window.AuroraShell.showScreen("screen-home"));
    await ev(page, () => window.AuroraShell.openIndex("trail"));
    await expect(page.locator("#index-results .index-result").first()).toContainText("Open Trail");
    await expect(page.locator("#index-results .index-result").first()).not.toContainText("Not yet");
    await ev(page, () => window.AuroraShell.showScreen("screen-home"));
    await page.locator("#glass-corner").click();
    await expect(page.locator("#btn-gc-trail")).toBeEnabled();
    await page.locator("#btn-gc-trail").click();
    await expect(page.locator("#screen-trail")).toBeVisible();
  });

  test("the footer says history lasts only for this page and the retention setting stores nothing yet", async ({ page }) => {
    await start(page); await openTrail(page);
    const f = page.locator("#trail-footer");
    await expect(f).toContainText("only until you reload");
    await expect(f).toContainText("does not keep anything yet");
  });

  test("reloading really does clear the history (the disclosure is true)", async ({ page }) => {
    await start(page);
    await ask(page, move("timetable", "work"));
    await openTrail(page);
    await expect(rows(page)).toHaveCount(1);
    await page.reload();
    await ev(page, () => window.AuroraShell.openApp("trail"));
    await expect(rows(page)).toHaveCount(0);
    expect(await ev(page, () => window.AuroraSystem.receipts().length)).toBe(0);
  });
});

test.describe("Trail: what the broker really recorded", () => {
  test("a single successful action: one row, source, time, risk, status, undo state", async ({ page }) => {
    await start(page);
    const r = await ask(page, move("timetable", "work"), { source: "Vault" });
    expect(r.ok).toBe(true);
    await openTrail(page);
    const row = rows(page).first();
    await expect(row).toContainText("Move “Exam timetable.txt” to Work");
    await expect(row).toContainText("Vault");
    await expect(row).toContainText("just now");
    await expect(row.locator(".risk")).toHaveAttribute("data-risk", "S2");
    await expect(row.locator(".chip", { hasText: word("Done") })).toBeVisible();
    await expect(row.locator(".chip", { hasText: "Can be undone" })).toBeVisible();
    const rec = await ev(page, () => window.AuroraSystem.receipts()[0]);
    await expect(row).toHaveAttribute("data-receipt", rec.id);
  });

  test("every row corresponds to a broker receipt and vice versa", async ({ page }) => {
    await start(page);
    await ask(page, move("timetable", "work")); await ask(page, move("reading-list", "projects")); await ask(page, { type: "focus.set", arguments: { enabled: true } });
    await ask(page, { type: "search.query", arguments: { query: "notes" } });
    await openTrail(page);
    const shown = await rows(page).evaluateAll((els) => els.map((e) => e.dataset.receipt).sort());
    const real = await ev(page, () => window.AuroraSystem.receipts().map((r) => r.id).sort());
    expect(shown).toEqual(real);
    expect(real.length).toBeGreaterThanOrEqual(4);
  });

  test("a multi-step plan is one group with its request text, outcome chip and every step", async ({ page }) => {
    await start(page);
    const plan = { steps: [move("timetable", "work"), move("budget-q4", "study"), { type: "focus.set", arguments: { enabled: true } }] };
    const r = await ask(page, plan, { source: "Aura", requestText: "tidy my files" });
    expect(r.ok).toBe(true);
    await openTrail(page);
    const g = page.locator("#screen-trail .trail-group");
    await expect(g).toHaveCount(1);
    await expect(g).toHaveAttribute("data-kind", "plan");
    await expect(g.locator(".section-title")).toHaveText("tidy my files");
    await expect(g.locator(".trail-group-head .chip")).toHaveText("Complete");
    await expect(g.locator(".trail-row")).toHaveCount(3);
    await expect(g).toContainText("3 of 3 steps ran");
  });

  test("a partially failed plan says Partial, names the failure, and does not claim the missing steps", async ({ page }) => {
    await start(page);
    await ev(page, () => { window.AuroraSystem.dev.failNextType = "file.move"; });
    const plan = { steps: [{ type: "focus.set", arguments: { enabled: true } }, move("timetable", "work"), move("budget-q4", "study")] };
    const r = await ask(page, plan, { source: "Aura", requestText: "try a plan" });
    expect(r.ok).toBe(false);
    await openTrail(page);
    const g = page.locator("#screen-trail .trail-group");
    await expect(g.locator(".trail-group-head .chip")).toHaveText("Partial");
    await expect(g).toContainText("2 of 3 steps ran");
    await expect(g).toContainText("1 did not run");
    await expect(g.locator(".trail-row")).toHaveCount(2);
    expect(await files(page)).toContain("budget-q4:work");           // the step that never ran changed nothing
    await rowFor(page, "Move “Exam timetable.txt”").locator(".trail-row-main").click();
    await expect(page.locator("#trail-detail")).toContainText("Simulated failure (developer option).");
    await expect(rowFor(page, "Move “Exam timetable.txt”").locator(".chip", { hasText: word("Failed") })).toBeVisible();
    await expect(rowFor(page, "Move “Exam timetable.txt”").locator(".trail-undo")).toHaveCount(0);
  });

  test("a stopped plan says Stopped", async ({ page }) => {
    await page.goto(BASE_URL);
    await ev(page, () => { window.AuroraSystem.dev.stepDelayMs = 600; window.AuroraShell.switchSpace("work", { quiet: true }); window.AuroraModels.pages.pause("page-notes"); });
    await page.locator("#btn-aura-study").click();
    await page.locator("#btn-plan-confirm").click(); await page.locator("#btn-plan-run").click();
    await expect(page.locator("#plan-running")).toBeVisible();
    await page.locator("#btn-plan-stop").click();
    await expect(page.locator("#plan-failure")).toBeVisible({ timeout: 8000 });
    await openTrail(page);
    const g = page.locator("#screen-trail .trail-group");
    await expect(g.locator(".trail-group-head .chip")).toHaveText("Stopped");
    await expect(g).toContainText("did not run");
  });

  test("a permission denied while a plan runs is recorded as Blocked, with the reason, and offers no undo", async ({ page }) => {
    await start(page);
    await ev(page, () => { window.AuroraSystem.dev.stepDelayMs = 300; });
    await ev(page, () => { window.__res = null; window.AuroraSystem.request({ steps: [{ type: "focus.set", arguments: { enabled: true } }, { type: "file.move", arguments: { id: "timetable", destination: "work" } }] }, { source: "Aura", requestText: "deny mid-run" }).then((r) => { window.__res = r; }); });
    await page.locator("#btn-review-approve").click();
    await ev(page, () => window.AuroraSystem.permissions.set("files.edit", "deny"));
    await page.waitForFunction(() => window.__res);
    await openTrail(page);
    const row = rowFor(page, "Move “Exam timetable.txt”");
    await expect(row.locator(".chip", { hasText: word("Blocked") })).toBeVisible();
    await row.locator(".trail-row-main").click();
    await expect(page.locator("#trail-detail")).toContainText("Blocked by Passport");
    await expect(row.locator(".trail-undo")).toHaveCount(0);
    expect(await files(page)).toContain("timetable:study");
    await page.getByRole("button", { name: "Failed or blocked", exact: true }).click();
    await expect(rows(page)).toHaveCount(1);
  });

  test("a request Passport refuses at review time never ran, so Trail records nothing for it", async ({ page }) => {
    await start(page);
    await ev(page, () => { window.AuroraSystem.permissions.set("files.edit", "deny"); });
    await ev(page, () => { window.__res = null; window.AuroraSystem.request({ type: "file.move", arguments: { id: "timetable", destination: "work" } }, { source: "Vault" }).then((r) => { window.__res = r; }); });
    await page.locator("#btn-review-passport").waitFor();
    await page.keyboard.press("Escape");
    await page.waitForFunction(() => window.__res);
    await openTrail(page);
    await expect(rows(page)).toHaveCount(0);
    expect(await files(page)).toContain("timetable:study");
  });

  test("stale approval: a permission that changes during a plan is recorded as 'Needs review again'", async ({ page }) => {
    await start(page);
    await ev(page, () => { window.AuroraSystem.dev.stepDelayMs = 300; window.AuroraSystem.permissions.set("files.edit", "allow"); });
    await ev(page, () => { window.__res = null; window.AuroraSystem.request({ steps: [{ type: "focus.set", arguments: { enabled: true } }, { type: "file.move", arguments: { id: "timetable", destination: "work" } }] }, { source: "Aura", requestText: "stale" }).then((r) => { window.__res = r; }); });
    await page.locator("#btn-review-approve").click();
    await ev(page, () => window.AuroraSystem.permissions.set("files.edit", "ask"));       // tightened while the plan runs
    await page.waitForFunction(() => window.__res);
    await openTrail(page);
    const row = rowFor(page, "Move “Exam timetable.txt”");
    await expect(row.locator(".chip", { hasText: word("Needs review again") })).toBeVisible();
    expect(await files(page)).toContain("timetable:study");
    await expect(row.locator(".trail-undo")).toHaveCount(0);
  });
});

test.describe("Trail: real Undo", () => {
  test("individual Undo waits for the broker, restores the file, and then shows Undone", async ({ page }) => {
    await start(page);
    await ask(page, move("timetable", "work"));
    expect(await files(page)).toContain("timetable:work");
    await openTrail(page);
    await ev(page, () => { window.AuroraSystem.dev.stepDelayMs = 0; });
    const row = rows(page).first();
    await row.locator(".trail-undo").click();
    await expect(row.locator(".chip", { hasText: word("Undone") })).toBeVisible();
    await expect(row.locator(".trail-undo")).toHaveCount(0);
    expect(await files(page)).toContain("timetable:study");
    await expect(page.locator("#trail-notice")).toContainText("Undone: Move");
    expect(await ev(page, () => window.AuroraSystem.receipts()[0].undoState)).toBe("used");
  });

  test("while one undo runs every Undo button is disabled and reads Undoing…; the UI claims nothing early", async ({ page }) => {
    await start(page);
    await ask(page, move("timetable", "work")); await ask(page, move("reading-list", "work"));
    await openTrail(page);
    // Hold the broker's undo open so the busy state can be observed. Wrapping AuroraSystem.undo changes timing only; the real broker undo still runs.
    await ev(page, () => { const real = window.AuroraSystem.undo; window.__release = null; window.AuroraSystem.undo = (id) => new Promise((res) => { window.__release = () => res(real(id)); }); });
    const first = rows(page).first();
    await first.locator(".trail-undo").click();
    await expect(first.locator(".trail-undo")).toContainText("Undoing…");
    await expect(first.locator(".trail-undo")).toBeDisabled();
    await expect(rows(page).nth(1).locator(".trail-undo")).toBeDisabled();
    await expect(first.locator(".chip", { hasText: word("Undone") })).toHaveCount(0);
    expect(await files(page)).toContain("reading-list:work");
    await ev(page, () => window.__release());
    await expect(first.locator(".chip", { hasText: word("Undone") })).toBeVisible();
    await expect(rows(page).nth(1).locator(".trail-undo")).toBeEnabled();
  });

  test("Undo all on a plan undoes every available step and says how many", async ({ page }) => {
    await start(page);
    await ask(page, { steps: [move("timetable", "work"), move("budget-q4", "study"), { type: "focus.set", arguments: { enabled: true } }] }, { requestText: "three things" });
    await openTrail(page);
    await page.getByRole("button", { name: /Undo all \(3\)/ }).click();
    await expect(page.locator("#trail-notice")).toContainText("Undid 3 steps");
    await expect(page.locator("#screen-trail .chip", { hasText: word("Undone") })).toHaveCount(3);
    expect(await files(page)).toEqual(expect.arrayContaining(["timetable:study", "budget-q4:work"]));
    await expect(page.getByRole("button", { name: /Undo all/ })).toHaveCount(0);
  });

  test("an undo the adapter rejects stays failed, is explained, and is not retried", async ({ page }) => {
    await start(page);
    await ask(page, move("timetable", "work"));
    await ask(page, del("timetable"));                          // the file is gone: undoing the move must fail
    await openTrail(page);
    const mv = rowFor(page, "Move “Exam timetable.txt”");
    await mv.locator(".trail-undo").click();
    await expect(page.locator("#trail-notice")).toContainText("Not undone");
    await expect(page.locator("#trail-notice")).toContainText("Undo did not work");
    await expect(mv.locator(".chip", { hasText: word("Undone") })).toHaveCount(0);
    await expect(mv.locator(".chip", { hasText: word("Undo failed") })).toBeVisible();
    await expect(mv.locator(".trail-undo")).toHaveCount(0);
    expect(await ev(page, () => window.AuroraSystem.receipts().find((r) => r.type === "file.move").undoState)).toBe("failed");
    await mv.locator(".trail-row-main").click();
    await expect(page.locator("#trail-detail")).toContainText("Undo failed: Undo did not work");
  });

  test("an already-undone action is labelled Undone and cannot be undone again (also refused by the broker)", async ({ page }) => {
    await start(page);
    await ask(page, move("timetable", "work"));
    const id = await ev(page, () => window.AuroraSystem.receipts()[0].id);
    expect((await ev(page, (i) => window.AuroraSystem.undo(i), id)).ok).toBe(true);
    await openTrail(page);
    await expect(rows(page).first().locator(".chip", { hasText: word("Undone") })).toBeVisible();
    await expect(rows(page).first().locator(".trail-undo")).toHaveCount(0);
    expect(await ev(page, (i) => window.AuroraSystem.undo(i), id)).toMatchObject({ ok: false, reason: "already-undone" });
    await rows(page).first().locator(".trail-row-main").click();
    await expect(page.locator("#trail-detail")).toContainText("Already undone");
  });

  test("actions that cannot be undone say why, and offer no Undo", async ({ page }) => {
    await start(page);
    await ask(page, { type: "search.query", arguments: { query: "notes" } });
    await openTrail(page);
    await rows(page).first().locator(".trail-row-main").click();
    await expect(page.locator("#trail-detail")).toContainText("Can’t be undone: searching changes nothing.");
    await expect(rows(page).first().locator(".trail-undo")).toHaveCount(0);
  });
});

test.describe("Trail: deleted files", () => {
  test("Restore invokes the delete receipt's own undo and the file comes back", async ({ page }) => {
    await start(page);
    expect((await ask(page, del("timetable"))).ok).toBe(true);
    expect(await files(page)).not.toContain("timetable:study");
    await openTrail(page);
    const d = page.locator('[data-deleted-file="timetable"]');
    await expect(d).toContainText("Exam timetable.txt");
    await d.getByRole("button", { name: "Restore Exam timetable.txt" }).click();
    await expect(page.locator("#trail-notice")).toContainText("Restored “Exam timetable.txt”");
    await expect(d).toHaveCount(0);
    expect(await files(page)).toContain("timetable:study");
    expect(await ev(page, () => window.AuroraSystem.receipts().find((r) => r.type === "file.delete").undoState)).toBe("used");
  });

  test("without a valid receipt there is no Restore, only the reason; Trail does not edit the world", async ({ page }) => {
    await start(page);
    await ask(page, del("timetable"));
    const id = await ev(page, () => window.AuroraSystem.receipts().find((r) => r.type === "file.delete").id);
    await ev(page, (i) => window.AuroraSystem.undo(i), id);          // use the receipt up...
    await ev(page, () => { window.AuroraSystem.world.deleted.timetable = { file: Object.assign({}, window.AuroraSystem.files.get("timetable")), at: Date.now() }; delete window.AuroraSystem.world.files.timetable; });   // ...then put the file back in the trash with NO usable receipt (test setup only)
    await openTrail(page);
    const d = page.locator('[data-deleted-file="timetable"]');
    await expect(d).toContainText("already undone");
    await expect(d.getByRole("button")).toHaveCount(0);
    expect(await files(page)).not.toContain("timetable:study");
  });

  test("a file deleted behind the broker's back has no receipt: no Restore, honest reason", async ({ page }) => {
    await start(page);
    await ev(page, () => { const w = window.AuroraSystem.world; w.deleted["budget-q4"] = { file: w.files["budget-q4"], at: Date.now() }; delete w.files["budget-q4"]; });
    await openTrail(page);
    const d = page.locator('[data-deleted-file="budget-q4"]');
    await expect(d).toContainText("No delete receipt was found");
    await expect(d.getByRole("button")).toHaveCount(0);
  });

  test("a held-then-cancelled delete leaves the file and Trail empty", async ({ page }) => {
    await start(page);
    const r = await ask(page, del("timetable"), { approve: false });
    expect(r).toMatchObject({ ok: false, cancelled: true });
    await openTrail(page);
    await expect(rows(page)).toHaveCount(0);
    await expect(page.locator(".trail-deleted")).toHaveCount(0);
    expect(await files(page)).toContain("timetable:study");
  });
});

test.describe("Trail: search and filters", () => {
  async function seed(page) {
    await start(page);
    await ask(page, move("timetable", "work"), { source: "Vault" });
    await ask(page, move("budget-q4", "study"), { source: "Aura" });
    await ask(page, { type: "focus.set", arguments: { enabled: true } }, { source: "Settings" });
    await ask(page, { type: "search.query", arguments: { query: "notes" } });
    const id = await ev(page, () => window.AuroraSystem.receipts().find((r) => r.type === "file.move").id);
    await ev(page, (i) => window.AuroraSystem.undo(i), id);            // newest move: Undone
    await ev(page, () => { window.AuroraSystem.dev.failNextType = "file.move"; });
    await ask(page, move("reading-list", "work"), { source: "Vault" });
    await openTrail(page);
  }
  const chip = (page, name) => page.getByRole("button", { name, exact: true });

  test("status filters: All / Done / Failed or blocked / Undoable / Undone", async ({ page }) => {
    await seed(page);
    await expect(rows(page)).toHaveCount(5);
    await chip(page, "Failed or blocked").click();
    await expect(rows(page)).toHaveCount(1);
    await expect(rows(page).first()).toContainText("Reading list");
    await chip(page, "Undone").click();
    await expect(rows(page)).toHaveCount(1);
    await expect(rows(page).first().locator(".chip", { hasText: word("Undone") })).toBeVisible();
    await chip(page, "Undoable").click();
    await expect(rows(page)).toHaveCount(2);
    await chip(page, "Done").click();
    await expect(rows(page)).toHaveCount(4);
    await chip(page, "All").click();
    await expect(rows(page)).toHaveCount(5);
    await expect(chip(page, "All")).toHaveAttribute("aria-pressed", "true");
  });

  test("search, risk filter and combined filters", async ({ page }) => {
    await seed(page);
    await page.locator("#trail-search").fill("vault");
    await expect(rows(page)).toHaveCount(2);
    await page.locator("#trail-risk").selectOption("S2");
    await expect(rows(page)).toHaveCount(2);
    await chip(page, "Failed or blocked").click();
    await expect(rows(page)).toHaveCount(1);
    await page.locator("#trail-risk").selectOption("S1");
    await expect(rows(page)).toHaveCount(0);
    await expect(page.locator("#screen-trail .empty-title").first()).toHaveText("Nothing matches");
    await page.locator("#btn-trail-clear").click();
    await expect(rows(page)).toHaveCount(5);
    await expect(page.locator("#trail-search")).toHaveValue("");
  });

  test("typing in search keeps focus and the caret at the end", async ({ page }) => {
    await seed(page);
    await page.locator("#trail-search").click();
    await page.keyboard.type("move");
    await expect(page.locator("#trail-search")).toBeFocused();
    await expect(page.locator("#trail-search")).toHaveValue("move");
    expect(await ev(page, () => document.getElementById("trail-search").selectionStart)).toBe(4);
  });

  test("a filter that hides the selected row clears the selection and the detail panel", async ({ page }) => {
    await seed(page);
    await rowFor(page, "Reading list").locator(".trail-row-main").click();
    await expect(rowFor(page, "Reading list").locator(".trail-row-main")).toHaveAttribute("aria-expanded", "true");
    await chip(page, "Undone").click();
    expect(await ev(page, () => window.AuroraTrail.selected())).toBeNull();
    await chip(page, "All").click();
    await expect(rowFor(page, "Reading list").locator(".trail-row-main")).toHaveAttribute("aria-expanded", "false");
  });
});

test.describe("Trail: untrusted text and the broker boundary", () => {
  test("markup in a file name or request text is shown as text and creates no elements", async ({ page }) => {
    await start(page);
    const evil = "<img src=x onerror=window.__pwn=1><b>bold</b>";
    await ev(page, (n) => { window.AuroraSystem.world.files["timetable"].name = n; }, evil);
    await ask(page, move("timetable", "work"), { source: "<script>window.__pwn=2</script>", requestText: "<svg onload=window.__pwn=3>" });
    await ask(page, { steps: [move("budget-q4", "study"), move("reading-list", "work")] }, { requestText: "<u>x</u>" });
    await openTrail(page);
    await expect(page.locator("#screen-trail")).toContainText(evil);
    await expect(page.locator("#screen-trail img, #screen-trail script, #screen-trail svg[onload], #screen-trail u, #screen-trail b")).toHaveCount(0);
    expect(await ev(page, () => window.__pwn)).toBeUndefined();
    await rows(page).first().locator(".trail-row-main").click();
    expect(await ev(page, () => window.__pwn)).toBeUndefined();
  });

  test("trail.js has no unsafe sinks and never touches the world or approves anything", async ({ page }) => {
    await start(page);
    const src = await (await page.request.get(BASE_URL + "/trail.js")).text();
    const model = await (await page.request.get(BASE_URL + "/trail-model.js")).text();
    for (const s of [src, model]) for (const bad of ["innerHTML", "outerHTML", "insertAdjacentHTML", "eval(", "new Function", "document.write", "fetch(", "XMLHttpRequest", "localStorage", "sessionStorage"]) expect(s).not.toContain(bad);
    const code = src.replace(/\/\*[\s\S]*?\*\//g, "");
    for (const bad of ["takeTrusted", ".world", "permissions.set", "execute(", "approve("]) expect(code).not.toContain(bad);
    expect(src).toContain("System.undo(");
    expect(src).toContain("System.undoProposal(");
  });

  test("Trail cannot be used to undo something the broker says is unavailable", async ({ page }) => {
    await start(page);
    await ask(page, { type: "search.query", arguments: { query: "x" } });
    const id = await ev(page, () => window.AuroraSystem.receipts()[0].id);
    expect(await ev(page, (i) => window.AuroraSystem.undo(i), id)).toMatchObject({ ok: false, reason: "not-undoable" });
    await openTrail(page);
    await expect(page.locator("#screen-trail .trail-undo")).toHaveCount(0);
  });
});

test.describe("Trail: keyboard, focus and layout", () => {
  test("rows, chips, search and Undo are reachable and operable by keyboard; focus stays on the row after Undo", async ({ page }) => {
    await start(page);
    await ask(page, move("timetable", "work"));
    await openTrail(page);
    const main = rows(page).first().locator(".trail-row-main");
    await main.focus();
    await page.keyboard.press("Enter");
    await expect(main).toHaveAttribute("aria-expanded", "true");
    await expect(main).toBeFocused();
    await page.keyboard.press("Enter");
    await expect(main).toHaveAttribute("aria-expanded", "false");
    await page.keyboard.press("Tab");
    await expect(rows(page).first().locator(".trail-undo")).toBeFocused();
    await page.keyboard.press("Enter");
    await expect(rows(page).first().locator(".chip", { hasText: word("Undone") })).toBeVisible();
    await expect(main).toBeFocused();
    const chipBtn = page.getByRole("button", { name: "Undone", exact: true });
    await chipBtn.focus(); await page.keyboard.press("Space");
    await expect(chipBtn).toHaveAttribute("aria-pressed", "true");
  });

  test("phone: detail opens inline under the row; desktop: it opens in the side panel", async ({ page }) => {
    await start(page);
    await ask(page, move("timetable", "work"));
    await page.setViewportSize({ width: 390, height: 800 });
    await openTrail(page);
    await rows(page).first().locator(".trail-row-main").click();
    await expect(rows(page).first().locator(".trail-inline")).toBeVisible();
    await expect(page.locator("#trail-detail")).toHaveCount(0);
    await page.setViewportSize({ width: 1280, height: 800 });
    await expect(page.locator("#trail-detail")).toBeVisible();
    await expect(page.locator("#trail-detail")).toContainText("Exam timetable.txt");
    await expect(rows(page).first().locator(".trail-inline")).toHaveCount(0);
    await expect(page.locator("#trail-detail .trail-undo")).toBeVisible();
    await page.locator("#trail-detail .trail-undo").click();
    await expect(page.locator("#trail-detail")).toContainText("Already undone");
  });

  for (const w of [360, 390, 768, 1280]) {
    test("no horizontal overflow at " + w + " px with history, a plan, a failure and a deleted file", async ({ page }) => {
      await start(page);
      await page.setViewportSize({ width: w, height: 800 });
      await ask(page, { steps: [move("timetable", "work"), move("budget-q4", "study")] }, { requestText: "a fairly long request text that has to wrap nicely on small phones" });
      await ev(page, () => { window.AuroraSystem.dev.failNextType = "file.move"; });
      await ask(page, move("reading-list", "work"));
      await ask(page, del("recipe-dal"));
      await openTrail(page);
      await rows(page).first().locator(".trail-row-main").click();
      const over = await ev(page, () => { const s = document.getElementById("screen-trail"); return s.scrollWidth - s.clientWidth + (document.documentElement.scrollWidth - document.documentElement.clientWidth); });
      expect(over).toBeLessThanOrEqual(0);
    });
  }

  test("200% text and high contrast keep Trail usable", async ({ page }) => {
    await start(page);
    await ask(page, move("timetable", "work"));
    await ev(page, () => { document.documentElement.dataset.textScale = "200"; document.documentElement.dataset.contrast = "high"; });
    await page.setViewportSize({ width: 390, height: 800 });
    await openTrail(page);
    await expect(rows(page).first().locator(".trail-undo")).toBeVisible();
    const over = await ev(page, () => document.documentElement.scrollWidth - document.documentElement.clientWidth);
    expect(over).toBeLessThanOrEqual(0);
  });
});
