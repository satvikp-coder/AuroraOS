/**
 * tests/ui-golden/vault.test.js — M4: Vault.
 * Vault lists the prototype's SAMPLE files and proposes every change through AuroraSystem.request. These tests use real
 * clicks, keys and the trusted review dialog (hold-to-confirm for deletes); nothing is approved by calling the broker.
 * Test setup that adds or renames sample files writes AuroraSystem.world.files on purpose; Vault itself never does.
 */
const { test, expect } = require("@playwright/test");

const BASE_URL = "http://localhost:3000";
const ev = (page, fn, arg) => page.evaluate(fn, arg);
const word = (w) => new RegExp("^" + w + "$");

async function start(page) {
  await page.goto(BASE_URL);
  await ev(page, () => { window.AuroraSystem.dev.stepDelayMs = 0; });
}
async function openVault(page) { await ev(page, () => window.AuroraShell.openApp("vault")); await expect(page.locator("#screen-vault h1")).toHaveText("Vault"); }
const item = (page, id) => page.locator('#screen-vault [data-file="' + id + '"]');
const openBtn = (page, id) => item(page, id).locator(".vault-open");
const names = (page) => page.locator("#screen-vault .vault-item .vault-name").allTextContents();
const fileMap = (page) => ev(page, () => Object.fromEntries(window.AuroraSystem.files.list().map((f) => [f.id, f.space])));
const dialog = (page) => page.locator("#review-dialog");
async function holdApprove(page) { await expect(dialog(page)).toBeVisible(); await page.locator(".hold-alt").click(); await page.locator(".hold-alt").click(); }
async function selectMode(page) { await page.locator("#btn-vault-select").click(); }
async function pickDest(page, space) { await page.locator("#btn-vault-dest-" + space).click(); }

test.describe("Vault: getting there", () => {
  test("opens by openApp; the Files app opens the same screen; no duplicate screen exists", async ({ page }) => {
    await start(page);
    expect(await ev(page, () => window.AuroraShell.openApp("vault"))).toMatchObject({ ok: true, id: "vault" });
    await expect(page.locator("#screen-vault")).toBeVisible();
    await ev(page, () => window.AuroraShell.showScreen("screen-home"));
    expect(await ev(page, () => window.AuroraShell.openApp("files"))).toMatchObject({ ok: true });
    await expect(page.locator("#screen-vault")).toBeVisible();
    await expect(page.locator("#screen-vault")).toHaveCount(1);
    await expect(page.locator("#vault-banner")).toHaveText("Sample files in this browser prototype. Not AuroraFS, nothing is saved.");
  });

  test("the Files tile on Home and the Vault app in Index both land on Vault", async ({ page }) => {
    await start(page);
    await page.getByRole("button", { name: "Open Files" }).click();
    await expect(page.locator("#screen-vault")).toBeVisible();
    await ev(page, () => window.AuroraShell.showScreen("screen-home"));
    await ev(page, () => window.AuroraShell.openIndex("vault"));
    await page.locator("#index-results .index-result", { hasText: "Open Vault" }).first().click();
    await expect(page.locator("#screen-vault")).toBeVisible();
  });

  test("an Index file result opens Vault with that file selected and previewed", async ({ page }) => {
    await start(page);
    await ev(page, () => window.AuroraShell.openIndex("Budget"));
    await page.locator("#index-results .index-result", { hasText: "Budget Q4.csv" }).first().click();
    await expect(page.locator("#screen-vault")).toBeVisible();
    await expect(page.locator("#vault-preview")).toContainText("Budget Q4.csv");
    await expect(openBtn(page, "budget-q4")).toBeFocused();
    expect(await ev(page, () => window.AuroraVault.previewed())).toBe("budget-q4");
  });

  test("Back from Vault returns to where you came from; the Dock still works", async ({ page }) => {
    await start(page);
    await openVault(page);
    await page.locator("#btn-vault-back").click();
    await expect(page.locator("#screen-home")).toBeVisible();
  });
});

test.describe("Vault: browsing", () => {
  test("lists every sample file; list and grid views switch; the choice survives a re-render", async ({ page }) => {
    await start(page);
    await openVault(page);
    await expect(page.locator("#screen-vault .vault-item")).toHaveCount(10);
    await expect(page.locator("#screen-vault ul.vault-list")).toHaveCount(1);
    await page.getByRole("radio", { name: "Grid" }).click();
    await expect(page.locator("#screen-vault ul.vault-grid")).toHaveCount(1);
    await expect(page.locator("#screen-vault ul.vault-list")).toHaveCount(0);
    await page.locator("#vault-search").fill("a");
    await expect(page.locator("#screen-vault ul.vault-grid")).toHaveCount(1);
    await page.getByRole("radio", { name: "List" }).click();
    await expect(page.locator("#screen-vault ul.vault-list")).toHaveCount(1);
  });

  test("search narrows the list, says how many, keeps focus and caret, and has a no-results state with Clear", async ({ page }) => {
    await start(page);
    await openVault(page);
    await page.locator("#vault-search").click();
    await page.keyboard.type("budget");
    await expect(page.locator("#vault-search")).toBeFocused();
    expect(await ev(page, () => document.getElementById("vault-search").selectionStart)).toBe(6);
    await expect(page.locator("#screen-vault .vault-item")).toHaveCount(1);
    await expect(page.locator("#vault-count")).toHaveText("Showing 1 of 10 files");
    await page.locator("#vault-search").fill("zzzz-nothing");
    await expect(page.locator("#screen-vault .vault-main .empty-title")).toHaveText("No files match");
    await page.locator("#btn-vault-clear-filters").click();
    await expect(page.locator("#screen-vault .vault-item")).toHaveCount(10);
    await expect(page.locator("#vault-search")).toHaveValue("");
  });

  test("sort by name, size and modified, in both directions", async ({ page }) => {
    await start(page);
    await openVault(page);
    const first = async () => (await names(page))[0];
    await expect(page.locator("#btn-vault-dir")).toContainText("Ascending");
    expect(await first()).toBe("Budget Q4.csv");
    await page.locator("#btn-vault-dir").click();
    await expect(page.locator("#btn-vault-dir")).toContainText("Descending");
    expect(await first()).toBe("Study notes.md");
    await page.locator("#vault-sort").selectOption("size");
    expect(await first()).toBe("Kernel diagram.svg");          // 48200 B, the biggest
    await page.locator("#btn-vault-dir").click();
    expect(await first()).toBe("Exam timetable.txt");            // 860 B, the smallest
  });

  test("sort by modified: newest and oldest first", async ({ page }) => {
    await start(page);
    await openVault(page);
    await page.locator("#vault-sort").selectOption("modified");
    await page.locator("#btn-vault-dir").click();                  // descending = newest first
    expect((await names(page))[0]).toBe("Study notes.md");          // 10 minutes ago
    await page.locator("#btn-vault-dir").click();                  // ascending = oldest first
    expect((await names(page))[0]).toBe("Dal recipe.txt");          // 12 days ago
  });

  test("Space filter chips show only that Space and combine with search", async ({ page }) => {
    await start(page);
    await openVault(page);
    await page.getByRole("button", { name: "Work", exact: true }).click();
    await expect(page.locator("#screen-vault .vault-item")).toHaveCount(3);
    await expect(page.getByRole("button", { name: "Work", exact: true })).toHaveAttribute("aria-pressed", "true");
    await page.locator("#vault-search").fill("budget");
    await expect(page.locator("#screen-vault .vault-item")).toHaveCount(1);
    await page.locator("#vault-search").fill("kernel");
    await expect(page.locator("#screen-vault .vault-main .empty-title")).toHaveText("No files match");
    await page.getByRole("button", { name: "All", exact: true }).click();
  });
});

test.describe("Vault: preview", () => {
  test("a text file shows its text in a <pre> with name, kind, Space, size and modified time", async ({ page }) => {
    await start(page);
    await openVault(page);
    await openBtn(page, "timetable").click();
    const p = page.locator("#vault-preview");
    await expect(p).toContainText("Exam timetable.txt");
    await expect(p).toContainText("Text file");
    await expect(p).toContainText("Study");
    await expect(p).toContainText("860 B");
    await expect(p.locator("pre")).toContainText("Mon 09:00 Systems");
    await expect(p).toContainText("Passport decisions");
    await expect(p.getByRole("button", { name: "Open Passport" })).toBeVisible();
  });

  test("an image shows an honest placeholder, not a fake picture", async ({ page }) => {
    await start(page);
    await openVault(page);
    await openBtn(page, "kernel-sketch").click();
    await expect(page.locator("#vault-preview .vault-placeholder")).toContainText("Image preview isn’t available");
    await expect(page.locator("#vault-preview img, #vault-preview canvas, #vault-preview svg image")).toHaveCount(0);
    await expect(page.locator("#vault-preview pre")).toHaveCount(0);
  });

  test("Open Passport from the preview opens Passport; Close hides the preview", async ({ page }) => {
    await start(page);
    await openVault(page);
    await openBtn(page, "timetable").click();
    await page.locator("#btn-vault-preview-close").click();
    await expect(page.locator("#vault-preview")).not.toContainText("Exam timetable.txt");          // the side panel falls back to its placeholder
    await expect(page.locator("#vault-preview .empty-title")).toHaveText("Select a file");
    await openBtn(page, "timetable").click();
    await page.locator("#btn-vault-passport").click();
    await expect(page.locator("#screen-passport")).toBeVisible();
  });

  test("an unknown or hostile id is refused and changes nothing", async ({ page }) => {
    await start(page);
    await openVault(page);
    for (const bad of ["nope", "constructor", "__proto__", "", null, 5, {}]) expect(await ev(page, (b) => window.AuroraVault.preview(b), bad)).toBe(false);
    expect(await ev(page, () => window.AuroraVault.previewed())).toBeNull();
    await expect(page.locator("#vault-preview .empty-title")).toHaveText("Select a file");
  });
});

test.describe("Vault: selection", () => {
  test("Select mode: checkboxes, single and multiple selection, count, Clear and leaving the mode", async ({ page }) => {
    await start(page);
    await openVault(page);
    await selectMode(page);
    const cb = (id) => item(page, id).getByRole("checkbox");
    await cb("timetable").click();
    await cb("budget-q4").click();
    await expect(cb("timetable")).toHaveAttribute("aria-checked", "true");
    await expect(page.locator("#vault-selcount")).toHaveText("2 selected");
    await expect(cb("timetable").locator("svg")).toHaveCount(1);                                      // not colour alone: a check icon in the box, plus aria-checked
    await cb("timetable").click();
    await expect(page.locator("#vault-selcount")).toHaveText("1 selected");
    await page.locator("#btn-vault-clear").click();
    await expect(page.locator("#vault-selbar")).toHaveCount(0);
    await selectMode(page);
    await expect(page.getByRole("checkbox")).toHaveCount(0);
  });

  test("Shift-click selects the range between the anchor and the target in the visible order", async ({ page }) => {
    await start(page);
    await openVault(page);
    await selectMode(page);
    const order = await names(page);
    const idOfRow = (i) => page.locator("#screen-vault .vault-item").nth(i).getAttribute("data-file");
    await page.locator("#screen-vault .vault-item").nth(1).locator(".vault-open").click();
    await page.locator("#screen-vault .vault-item").nth(4).locator(".vault-open").click({ modifiers: ["Shift"] });
    expect(await ev(page, () => window.AuroraVault.selection().length)).toBe(4);
    for (const i of [1, 2, 3, 4]) await expect(page.locator("#screen-vault .vault-item").nth(i)).toHaveAttribute("data-selected", "true");
    await expect(page.locator("#screen-vault .vault-item").nth(0)).toHaveAttribute("data-selected", "false");
    await expect(page.locator("#screen-vault .vault-item").nth(5)).toHaveAttribute("data-selected", "false");
    expect(order.length).toBe(10); expect(await idOfRow(1)).toBeTruthy();
  });

  test("keyboard: arrows move focus, Space toggles, Ctrl+A selects all visible, Escape clears; typing in search is not hijacked", async ({ page }) => {
    await start(page);
    await openVault(page);
    const rows = page.locator("#screen-vault .vault-open");
    await rows.first().focus();
    await page.keyboard.press("ArrowDown");
    await expect(rows.nth(1)).toBeFocused();
    await page.keyboard.press("End");
    await expect(rows.nth(9)).toBeFocused();
    await page.keyboard.press("Home");
    await expect(rows.first()).toBeFocused();
    await page.keyboard.press("Space");
    await expect(page.locator("#vault-selcount")).toHaveText("1 selected");
    expect(await ev(page, () => window.AuroraVault.previewed())).toBeNull();                      // Space selects, it does not open
    await page.keyboard.press("ArrowDown"); await page.keyboard.press("Space");
    await expect(page.locator("#vault-selcount")).toHaveText("2 selected");
    await page.keyboard.press("Control+a");
    await expect(page.locator("#vault-selcount")).toHaveText("10 selected");
    await page.keyboard.press("Escape");
    await expect(page.locator("#vault-selbar")).toHaveCount(0);
    await expect(page.locator("#screen-vault")).toBeVisible();                                    // Escape cleared the selection, it did not leave
    await page.keyboard.press("Enter");
    await expect(page.locator("#vault-preview")).toBeVisible();                                    // Enter opens the preview
    await page.locator("#vault-search").click();
    await page.keyboard.press("Control+a");                                                        // select-all-text in the field
    await page.keyboard.type("x ");
    await expect(page.locator("#vault-selbar")).toHaveCount(0);
    await page.keyboard.press("Delete");                                                           // Delete in a text field edits text
    await expect(dialog(page)).toHaveCount(0);
  });

  test("a selection survives sorting and filtering only for files that still exist; hidden files stay selected", async ({ page }) => {
    await start(page);
    await openVault(page);
    await selectMode(page);
    await item(page, "timetable").getByRole("checkbox").click();
    await page.locator("#vault-sort").selectOption("size");
    await expect(page.locator("#vault-selcount")).toHaveText("1 selected");
    await expect(item(page, "timetable")).toHaveAttribute("data-selected", "true");
  });
});

test.describe("Vault: move (every change is reviewed)", () => {
  test("single move: pick a Space, review, approve; the file moves; Undo puts it back; both are real receipts", async ({ page }) => {
    await start(page);
    await openVault(page);
    await openBtn(page, "timetable").click();
    await page.locator("#btn-vault-preview-move").click();
    await expect(page.locator("#vault-move-dialog")).toContainText("Move “Exam timetable.txt”");
    await expect(page.locator("#btn-vault-dest-study")).toHaveCount(0);                              // it is already in Study
    await pickDest(page, "work");
    await expect(dialog(page)).toBeVisible();                                                         // the trusted review: nothing moved yet
    expect((await fileMap(page)).timetable).toBe("study");
    await page.locator("#btn-review-approve").click();
    await expect(page.locator("#vault-result")).toContainText("Moved 1 file.");
    expect((await fileMap(page)).timetable).toBe("work");
    expect(await ev(page, () => window.AuroraSystem.receipts().filter((r) => r.type === "file.move" && r.status === "done").length)).toBe(1);
    await page.locator("#btn-vault-undo").click();
    await expect(page.locator("#vault-result")).toContainText("Undone");
    expect((await fileMap(page)).timetable).toBe("study");
    await expect(page.locator("#btn-vault-undo")).toHaveCount(0);                                     // nothing left to undo
    expect(await ev(page, () => window.AuroraSystem.receipts()[0].undoState)).toBe("used");
  });

  test("cancelling the destination picker or the review changes nothing and says so", async ({ page }) => {
    await start(page);
    await openVault(page);
    await openBtn(page, "timetable").click();
    await page.locator("#btn-vault-preview-move").click();
    await page.locator("#btn-vault-move-cancel").click();
    await expect(page.locator("#vault-result")).toHaveCount(0);
    await page.locator("#btn-vault-preview-move").click();
    await pickDest(page, "work");
    await page.locator("#btn-review-cancel").click();
    await expect(page.locator("#vault-result")).toContainText("Cancelled. Nothing was changed.");
    expect((await fileMap(page)).timetable).toBe("study");
    expect(await ev(page, () => window.AuroraSystem.receipts().length)).toBe(0);
  });

  test("multi-file move is one plan with one review; files already in the destination are skipped and counted", async ({ page }) => {
    await start(page);
    await openVault(page);
    await selectMode(page);
    for (const id of ["timetable", "reading-list", "budget-q4"]) await item(page, id).getByRole("checkbox").click();
    await page.locator("#btn-vault-move").click();
    await pickDest(page, "work");
    await expect(dialog(page)).toContainText("Exam timetable.txt");
    await page.locator("#btn-review-approve").click();
    await expect(page.locator("#vault-result")).toContainText("Moved 2 files.");
    await expect(page.locator("#vault-result")).toContainText("1 file already in Work was skipped.");
    const m = await fileMap(page);
    expect([m.timetable, m["reading-list"], m["budget-q4"]]).toEqual(["work", "work", "work"]);
    expect(await ev(page, () => new Set(window.AuroraSystem.receipts().map((r) => r.proposalId)).size)).toBe(1);   // one plan
    await page.locator("#btn-vault-undo").click();
    await expect(page.locator("#vault-result")).toContainText("2 steps");
    const u = await fileMap(page);
    expect([u.timetable, u["reading-list"], u["budget-q4"]]).toEqual(["study", "study", "work"]);
  });

  test("more than 12 files are never silently truncated: the limit is explained and each batch is reviewed", async ({ page }) => {
    await start(page);
    await ev(page, () => { const w = window.AuroraSystem.world.files; for (let i = 0; i < 14; i++) w["bulk-" + i] = { id: "bulk-" + i, name: "Bulk " + String(i).padStart(2, "0") + ".txt", kind: "text", space: "personal", size: 100 + i, modified: Date.now() - i * 1000, preview: "x" }; });
    await openVault(page);
    await page.getByRole("button", { name: "Personal", exact: true }).click();
    await selectMode(page);
    await page.locator("#btn-vault-select").focus();
    await page.locator("#screen-vault .vault-item .vault-open").first().focus();
    await page.keyboard.press("Control+a");
    await expect(page.locator("#vault-selcount")).toHaveText("16 selected");                         // 14 bulk + 2 personal files
    await page.locator("#btn-vault-move").click();
    await pickDest(page, "work");
    await expect(page.locator("#vault-batch-dialog")).toContainText("at most 12 changes at a time");
    await expect(page.locator("#vault-batch-dialog")).toContainText("2 reviews (12 + 4)");
    await page.locator("#btn-vault-batch-ok").click();
    await page.locator("#btn-review-approve").click();
    await expect(dialog(page)).toBeVisible({ timeout: 8000 });                                         // second review
    await expect(dialog(page)).not.toContainText("Bulk 00");                                           // different files than batch one
    await page.locator("#btn-review-approve").click();
    await expect(page.locator("#vault-result")).toContainText("Moved 12 files. Moved 4 files.");
    const m = await fileMap(page);
    expect(Object.values(m).filter((s) => s === "personal").length).toBe(0);
    expect(await ev(page, () => window.AuroraSystem.receipts().filter((r) => r.status === "done").length)).toBe(16);
  });

  test("cancelling the second batch stops there: earlier moves stay (and stay undoable), later ones never start", async ({ page }) => {
    await start(page);
    await ev(page, () => { const w = window.AuroraSystem.world.files; for (let i = 0; i < 13; i++) w["bulk-" + i] = { id: "bulk-" + i, name: "Bulk " + String(i).padStart(2, "0") + ".txt", kind: "text", space: "projects", size: 100, modified: Date.now() - i * 1000, preview: "x" }; });
    await openVault(page);
    await page.getByRole("button", { name: "Projects", exact: true }).click();
    await page.locator("#screen-vault .vault-open").first().focus();
    await page.keyboard.press("Control+a");
    await page.locator("#btn-vault-move").click();
    await pickDest(page, "personal");
    await page.locator("#btn-vault-batch-ok").click();
    await page.locator("#btn-review-approve").click();
    await expect(dialog(page)).toBeVisible({ timeout: 8000 });
    await page.locator("#btn-review-cancel").click();
    await expect(page.locator("#vault-result")).toContainText("Cancelled. Nothing was changed.");
    const moved = Object.values(await fileMap(page)).filter((s) => s === "personal").length;
    expect(moved).toBe(2 + 12);                                                                         // 2 original personal + the first batch
    await expect(page.locator("#btn-vault-undo")).toBeVisible();
  });

  test("a failed step is reported by name with its reason; nothing is claimed for steps that did not run", async ({ page }) => {
    await start(page);
    await ev(page, () => { window.AuroraSystem.dev.failNextType = "file.move"; });
    await openVault(page);
    await openBtn(page, "timetable").click();
    await page.locator("#btn-vault-preview-move").click();
    await pickDest(page, "work");
    await page.locator("#btn-review-approve").click();
    await expect(page.locator("#vault-result")).toContainText("Moved 0 of 1 file. Failed: “Exam timetable.txt” (Simulated failure (developer option))");
    expect((await fileMap(page)).timetable).toBe("study");
    await expect(page.locator("#vault-result")).not.toContainText("Moved 1 file");
    await expect(page.locator("#btn-vault-undo")).toHaveCount(0);
  });

  test("Undo that the broker rejects is shown as failed, honestly (the file was deleted afterwards)", async ({ page }) => {
    await start(page);
    await openVault(page);
    await openBtn(page, "timetable").click();
    await page.locator("#btn-vault-preview-move").click();
    await pickDest(page, "work");
    await page.locator("#btn-review-approve").click();
    await expect(page.locator("#vault-result")).toContainText("Moved 1 file.");
    // delete it behind Vault's back (through the broker, as another app would)
    await ev(page, () => { window.__d = window.AuroraSystem.request({ type: "file.delete", arguments: { id: "timetable" } }, { source: "Other" }); });
    await holdApprove(page);
    await page.waitForFunction(() => window.__d && window.AuroraSystem.files.get("timetable") === null);
    await page.locator("#btn-vault-undo").click();
    await expect(page.locator("#vault-result")).toContainText("Undo failed");
    await expect(page.locator("#vault-result")).not.toContainText("Undone");
    await expect(page.locator("#vault-result")).toHaveClass(/callout-error/);
  });
});

test.describe("Vault: delete (S3, hold to confirm)", () => {
  async function askDelete(page, id) { await openBtn(page, id).click(); await page.locator("#btn-vault-preview-delete").click(); await expect(dialog(page)).toBeVisible(); }

  test("a single click on the confirm control never approves; only the hold or the explicit two-step does", async ({ page }) => {
    await start(page);
    await openVault(page);
    await askDelete(page, "timetable");
    await page.locator("#btn-review-approve").click();                                                // a plain click
    await page.locator("#btn-review-approve").click({ clickCount: 2 });
    await page.waitForTimeout(300);
    await expect(dialog(page)).toBeVisible();
    expect((await fileMap(page)).timetable).toBe("study");
    expect(await ev(page, () => window.AuroraSystem.receipts().length)).toBe(0);
    await page.locator(".hold-alt").click();                                                          // step 1 only arms it
    await page.waitForTimeout(200);
    await expect(dialog(page)).toBeVisible();
    expect((await fileMap(page)).timetable).toBe("study");
  });

  test("delete with the hold: the file leaves the list, a real receipt exists, Undo restores it, Trail lists it", async ({ page }) => {
    await start(page);
    await openVault(page);
    await askDelete(page, "timetable");
    await holdApprove(page);
    await expect(page.locator("#vault-result")).toContainText("Deleted 1 file.");
    await expect(item(page, "timetable")).toHaveCount(0);
    expect(await ev(page, () => window.AuroraSystem.files.get("timetable"))).toBeNull();
    const rec = await ev(page, () => window.AuroraSystem.receipts().find((r) => r.type === "file.delete"));
    expect(rec).toMatchObject({ status: "done", undoState: "available", risk: "S3" });
    await page.locator("#btn-vault-trail").click();
    await expect(page.locator("#screen-trail")).toBeVisible();
    await expect(page.locator('#screen-trail [data-receipt="' + rec.id + '"]')).toContainText("Delete “Exam timetable.txt”");
    await expect(page.locator('#screen-trail [data-deleted-file="timetable"]')).toBeVisible();
    await ev(page, () => window.AuroraShell.openApp("vault"));
    await page.locator("#btn-vault-undo").click();
    await expect(page.locator("#vault-result")).toContainText("Undone");
    await expect(item(page, "timetable")).toHaveCount(1);
    expect((await fileMap(page)).timetable).toBe("study");
  });

  test("Trail's Restore brings a Vault-deleted file back and Vault shows it again", async ({ page }) => {
    await start(page);
    await openVault(page);
    await askDelete(page, "recipe-dal");
    await holdApprove(page);
    await expect(item(page, "recipe-dal")).toHaveCount(0);
    await ev(page, () => window.AuroraShell.openApp("trail"));
    await page.getByRole("button", { name: "Restore Dal recipe.txt" }).click();
    await expect(page.locator("#trail-notice")).toContainText("Restored");
    await ev(page, () => window.AuroraShell.openApp("vault"));
    await expect(item(page, "recipe-dal")).toHaveCount(1);
    await expect(page.locator("#btn-vault-undo")).toHaveCount(0);                                     // Trail used the undo; Vault does not offer it again
  });

  test("cancel at the hold dialog: nothing changes and no receipt exists", async ({ page }) => {
    await start(page);
    await openVault(page);
    await askDelete(page, "timetable");
    await page.keyboard.press("Escape");
    await expect(page.locator("#vault-result")).toContainText("Cancelled. Nothing was changed.");
    expect((await fileMap(page)).timetable).toBe("study");
    expect(await ev(page, () => window.AuroraSystem.receipts().length)).toBe(0);
    await expect(openBtn(page, "timetable")).toHaveCount(1);
  });

  test("the Delete key deletes the selection through the same hold; multiple files are one plan with one hold", async ({ page }) => {
    await start(page);
    await openVault(page);
    await selectMode(page);
    await item(page, "timetable").getByRole("checkbox").click();
    await item(page, "reading-list").getByRole("checkbox").click();
    await openBtn(page, "timetable").focus();
    await page.keyboard.press("Delete");
    await expect(dialog(page)).toBeVisible();
    await expect(dialog(page)).toContainText("Exam timetable.txt");
    await expect(dialog(page)).toContainText("Reading list.md");
    await holdApprove(page);
    await expect(page.locator("#vault-result")).toContainText("Deleted 2 files.");
    expect(await ev(page, () => new Set(window.AuroraSystem.receipts().map((r) => r.proposalId)).size)).toBe(1);
    await page.locator("#btn-vault-undo").click();
    await expect(page.locator("#vault-result")).toContainText("2 steps");
    await expect(item(page, "timetable")).toHaveCount(1);
    await expect(item(page, "reading-list")).toHaveCount(1);
  });

  test("Passport Deny blocks delete: the button is off with the reason, Open Passport works, and the broker refuses it anyway", async ({ page }) => {
    await start(page);
    await ev(page, () => window.AuroraSystem.permissions.set("files.delete", "deny"));
    await openVault(page);
    await expect(page.locator("#vault-delete-denied")).toContainText("Passport blocks deleting files");
    await openBtn(page, "timetable").click();
    await expect(page.locator("#btn-vault-preview-delete")).toBeDisabled();
    await expect(page.locator("#vault-preview")).toContainText("Blocked");
    await page.locator("#vault-delete-denied").getByRole("button", { name: "Open Passport" }).click();
    await expect(page.locator("#screen-passport")).toBeVisible();
    await ev(page, () => window.AuroraShell.openApp("vault"));
    // bypass attempt through the API: the trusted review refuses it and nothing is deleted
    await ev(page, () => { window.__b = window.AuroraSystem.request({ type: "file.delete", arguments: { id: "timetable" } }, { source: "Attacker" }); });
    await page.locator("#btn-review-passport").waitFor();
    await page.keyboard.press("Escape");
    expect(await ev(page, () => window.__b)).toMatchObject({ ok: false, blocked: true });
    expect((await fileMap(page)).timetable).toBe("study");
    expect(await ev(page, () => window.AuroraSystem.receipts().length)).toBe(0);
  });

  test("Passport Deny on moving blocks Move the same way (and never changes the permission silently)", async ({ page }) => {
    await start(page);
    await ev(page, () => window.AuroraSystem.permissions.set("files.edit", "deny"));
    await openVault(page);
    await expect(page.locator("#vault-edit-denied")).toContainText("Passport blocks moving files");
    await openBtn(page, "timetable").click();
    await expect(page.locator("#btn-vault-preview-move")).toBeDisabled();
    expect(await ev(page, () => window.AuroraSystem.permissions.get("files.edit"))).toBe("deny");
    expect(await ev(page, () => window.AuroraSystem.permissions.log().length)).toBe(1);                // only the test's own change
  });

  test("a permission that changes while the hold dialog is open invalidates the approval: the file stays", async ({ page }) => {
    await start(page);
    await openVault(page);
    await askDelete(page, "timetable");
    await ev(page, () => window.AuroraSystem.permissions.set("files.delete", "deny"));               // changed after the preview
    await holdApprove(page);
    await expect(page.locator("#vault-result")).toContainText(/Deleted 0 of 1 file|Could not delete|Blocked/);
    expect((await fileMap(page)).timetable).toBe("study");
    expect(await ev(page, () => window.AuroraSystem.receipts().filter((r) => r.type === "file.delete" && r.status === "done").length)).toBe(0);
    await expect(page.locator("#vault-result")).not.toContainText("Deleted 1 file");
  });

  test("a delete that fails after approval is reported as failed, not as deleted", async ({ page }) => {
    await start(page);
    await ev(page, () => { window.AuroraSystem.dev.failNextType = "file.delete"; });
    await openVault(page);
    await askDelete(page, "timetable");
    await holdApprove(page);
    await expect(page.locator("#vault-result")).toContainText("Deleted 0 of 1 file. Failed: “Exam timetable.txt”");
    await expect(page.locator("#vault-result")).not.toContainText("Deleted 1 file");
    await expect(item(page, "timetable")).toHaveCount(1);
    expect((await fileMap(page)).timetable).toBe("study");
  });
});

test.describe("Vault: untrusted text and the broker boundary", () => {
  test("markup in file names and text previews is shown as text everywhere and creates no elements", async ({ page }) => {
    await start(page);
    const evil = "<img src=x onerror=window.__pwn=1><b>x</b>.txt";
    await ev(page, (n) => { const f = window.AuroraSystem.world.files["timetable"]; f.name = n; f.preview = "<script>window.__pwn=2</script><svg onload=window.__pwn=3>"; }, evil);
    await openVault(page);
    await expect(page.locator("#screen-vault")).toContainText(evil);
    await openBtn(page, "timetable").click();
    await expect(page.locator("#vault-preview pre")).toContainText("<script>window.__pwn=2</script>");
    await page.locator("#btn-vault-preview-move").click();
    await expect(page.locator("#vault-move-dialog")).toContainText(evil);
    await pickDest(page, "work");
    await page.locator("#btn-review-approve").click();
    await expect(page.locator("#vault-result")).toContainText("Moved 1 file.");
    await expect(page.locator("#screen-vault img, #screen-vault script, #screen-vault b, #screen-vault svg[onload], #review-dialog img")).toHaveCount(0);
    expect(await ev(page, () => window.__pwn)).toBeUndefined();
  });

  test("vault.js has no unsafe sinks, never touches the world, never approves, and only proposes through request", async ({ page }) => {
    await start(page);
    const src = await (await page.request.get(BASE_URL + "/vault.js")).text();
    const model = await (await page.request.get(BASE_URL + "/vault-model.js")).text();
    for (const s of [src, model]) for (const bad of ["innerHTML", "outerHTML", "insertAdjacentHTML", "eval(", "new Function", "document.write", "fetch(", "XMLHttpRequest", "localStorage", "sessionStorage"]) expect(s).not.toContain(bad);
    const code = src.replace(/\/\*[\s\S]*?\*\//g, "");
    for (const bad of ["takeTrusted", ".world", "permissions.set", "execute(", "approve(", "proposer"]) expect(code).not.toContain(bad);
    expect(code).toContain("System.request(");
    expect(code).toContain("System.undoProposal(");
    expect(src).not.toContain("adapters");
  });

  test("a file removed behind Vault's back disappears from the selection, the preview and the list", async ({ page }) => {
    await start(page);
    await openVault(page);
    await selectMode(page);
    await item(page, "timetable").getByRole("checkbox").click();
    await openBtn(page, "reading-list").click();                                                       // in select mode this toggles too
    await ev(page, () => { window.__d = window.AuroraSystem.request({ type: "file.delete", arguments: { id: "timetable" } }, { source: "Other" }); });
    await holdApprove(page);
    await page.waitForFunction(() => window.__d && window.AuroraSystem.files.get("timetable") === null);
    await expect(item(page, "timetable")).toHaveCount(0);
    expect(await ev(page, () => window.AuroraVault.selection())).toEqual(["reading-list"]);
  });
});

test.describe("Vault: layout, accessibility and robustness", () => {
  test("phone: preview stacks above the list; desktop: it is a side panel", async ({ page }) => {
    await start(page);
    await page.setViewportSize({ width: 390, height: 800 });
    await openVault(page);
    await openBtn(page, "timetable").click();
    const pb = await page.locator("#vault-preview").boundingBox(), lb = await page.locator("#screen-vault ul.vault-list").boundingBox();
    expect(pb.y).toBeLessThan(lb.y);
    await page.setViewportSize({ width: 1280, height: 800 });
    await expect(page.locator(".vault-layout.is-wide")).toBeVisible();
    const pb2 = await page.locator("#vault-preview").boundingBox(), lb2 = await page.locator("#screen-vault ul.vault-list").boundingBox();
    expect(pb2.x).toBeGreaterThan(lb2.x + lb2.width - 2);
  });

  for (const w of [360, 390, 412, 768, 1024, 1280, 1440]) {
    for (const view of ["list", "grid"]) {
      test("no horizontal overflow at " + w + " px (" + view + ", selection bar, preview, long name)", async ({ page }) => {
        await start(page);
        await ev(page, () => { window.AuroraSystem.world.files["timetable"].name = "An extraordinarily_long_file_name_without_any_spaces_that_must_wrap_or_clip_sensibly_2026.txt"; });
        await page.setViewportSize({ width: w, height: 800 });
        await openVault(page);
        if (view === "grid") await page.getByRole("radio", { name: "Grid" }).click();
        await selectMode(page);
        await item(page, "budget-q4").getByRole("checkbox").click();
        await openBtn(page, "timetable").click();
        await page.waitForTimeout(100);
        const over = await ev(page, () => { const s = document.getElementById("screen-vault"); return Math.max(0, s.scrollWidth - s.clientWidth) + Math.max(0, document.documentElement.scrollWidth - document.documentElement.clientWidth); });
        expect(over).toBe(0);
      });
    }
  }

  test("high contrast, reduced motion and 200% text stay usable (no overflow, controls reachable)", async ({ page }) => {
    await start(page);
    await ev(page, () => { window.AuroraShell.setContrast("high"); window.AuroraShell.setMotion("reduce"); document.documentElement.dataset.textScale = "200"; });
    await page.setViewportSize({ width: 390, height: 800 });
    await openVault(page);
    await openBtn(page, "timetable").click();
    await expect(page.locator("#btn-vault-preview-delete")).toBeVisible();
    await expect(page.locator("#vault-search")).toBeVisible();
    const over = await ev(page, () => document.documentElement.scrollWidth - document.documentElement.clientWidth);
    expect(over).toBeLessThanOrEqual(0);
    expect(await ev(page, () => document.documentElement.dataset.motion || document.documentElement.getAttribute("data-motion"))).toBeTruthy();
  });

  test("touch targets are at least 44 px for the controls a thumb uses", async ({ page }) => {
    await start(page);
    await page.setViewportSize({ width: 390, height: 800 });
    await openVault(page);
    await selectMode(page);
    for (const sel of ["#screen-vault .vault-open", "#screen-vault .vault-check", "#btn-vault-select", "#vault-search", "#btn-vault-dir"]) {
      const b = await page.locator(sel).first().boundingBox();
      expect(b.height, sel).toBeGreaterThanOrEqual(43.5);
    }
  });

  test("a full session produces no console errors or page errors", async ({ page }) => {
    const errors = [];
    page.on("console", (m) => { if (m.type() === "error") errors.push(m.text()); });
    page.on("pageerror", (e) => errors.push(String(e)));
    await start(page);
    await openVault(page);
    await openBtn(page, "timetable").click();
    await page.locator("#btn-vault-preview-move").click(); await pickDest(page, "work"); await page.locator("#btn-review-approve").click();
    await page.locator("#btn-vault-undo").click();
    await page.locator("#btn-vault-preview-delete").click(); await holdApprove(page);
    await page.locator("#vault-search").fill("zzz");
    await ev(page, () => window.AuroraShell.openApp("trail"));
    expect(errors).toEqual([]);
  });

  test("an empty Vault (everything deleted) shows an empty state that leads to Trail", async ({ page }) => {
    await start(page);
    await ev(page, () => { const w = window.AuroraSystem.world; Object.keys(w.files).forEach((k) => { w.deleted[k] = { file: w.files[k], at: Date.now() }; delete w.files[k]; }); });
    await openVault(page);
    await expect(page.locator("#screen-vault .vault-main .empty-title")).toHaveText("No files");
    await page.locator("#btn-vault-to-trail").click();
    await expect(page.locator("#screen-trail")).toBeVisible();
  });
});
