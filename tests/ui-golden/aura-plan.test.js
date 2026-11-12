/**
 * tests/ui-golden/aura-plan.test.js — M1 / S8: the Aura plan and Action Broker boundary, end to end.
 * Every case asserts real state (receipts, Space, Focus, timers, mock files), not just that a panel is visible.
 * Browser prototype only; the "files" are in-memory mock objects.
 */
const { test, expect } = require("@playwright/test");

const BASE_URL = "http://localhost:3000";
const ev = (page, fn, arg) => page.evaluate(fn, arg);
const state = (page) => ev(page, () => ({
  space: window.AuroraShell.state.activeSpace, focus: window.AuroraModels.qs.get("focus"), timer: window.AuroraSystem.world.timer && window.AuroraSystem.world.timer.seconds,
  notes: (window.AuroraModels.pages.list().find((p) => p.id === "page-notes") || {}).state, receipts: window.AuroraSystem.receipts().length,
  settingsFocus: window.AuroraSettingsModel.get("notifications.focus"),
}));

/** From Work with Notes paused, so every step of the study plan has a visible, checkable effect. */
async function fresh(page, delay) {
  await page.goto(BASE_URL);
  await ev(page, (d) => { window.AuroraSystem.dev.stepDelayMs = d; window.AuroraShell.switchSpace("work", { quiet: true }); window.AuroraModels.pages.pause("page-notes"); }, delay === undefined ? 20 : delay);
}
async function review(page) { await page.locator("#btn-aura-study").click(); await expect(page.locator("#screen-aura .plan-step")).toHaveCount(4); }
async function approveAndRun(page) { await page.locator("#btn-plan-confirm").click(); await page.locator("#btn-plan-run").click(); }
const skip = (page, n) => page.locator("#screen-aura .plan-step").nth(n - 1).locator(".step-skip");

test.describe("Aura plan: review before anything runs", () => {
  test("shows the request and four steps, and nothing changes until approval", async ({ page }) => {
    await fresh(page);
    const before = await state(page);
    expect(before).toMatchObject({ space: "work", focus: false, timer: null, notes: "paused", receipts: 0 });
    await review(page);
    await expect(page.locator("#plan-request-text")).toHaveText("“Prepare my phone for studying”");
    await page.waitForTimeout(1200);
    expect(await state(page)).toEqual(before);
    await expect(page.locator("#screen-aura")).toContainText("Nothing has run yet");
  });

  test("Confirm opens a dialog; Cancel there runs nothing", async ({ page }) => {
    await fresh(page);
    const before = await state(page);
    await review(page);
    await page.locator("#btn-plan-confirm").click();
    await expect(page.locator("#plan-confirmation")).toBeVisible();
    await expect(page.locator("#plan-confirmation .confirm-list li")).toHaveCount(4);
    await page.locator("#btn-plan-cancel").click();
    await page.waitForTimeout(600);
    expect(await state(page)).toEqual(before);
    await expect(page.locator("#btn-plan-confirm")).toBeFocused();
  });

  test("Escape in the confirmation dialog runs nothing", async ({ page }) => {
    await fresh(page);
    const before = await state(page);
    await review(page);
    await page.locator("#btn-plan-confirm").click();
    await page.keyboard.press("Escape");
    await page.waitForTimeout(600);
    expect(await state(page)).toEqual(before);
    await expect(page.locator("#screen-aura")).toBeVisible();
  });
});

test.describe("Aura plan: running, receipts and undo", () => {
  test("approval runs every step with real effects and a receipt", async ({ page }) => {
    await fresh(page);
    await review(page); await approveAndRun(page);
    await expect(page.locator("#plan-receipt")).toBeVisible({ timeout: 8000 });
    await expect(page.locator("#plan-receipt h2")).toHaveText("Plan complete");
    expect(await state(page)).toEqual({ space: "study", focus: true, timer: 1500, notes: "running", receipts: 4, settingsFocus: true });
    await expect(page.locator('#plan-receipt [data-status="done"]')).toHaveCount(4);
    await page.locator("#btn-plan-trail").click();
    await expect(page.locator("#toast")).toContainText("Trail arrives in a later milestone");     // honest, not a dead end
  });

  test("per-step Undo reverts only that step, once", async ({ page }) => {
    await fresh(page);
    await review(page); await approveAndRun(page);
    await expect(page.locator("#plan-receipt")).toBeVisible({ timeout: 8000 });
    await page.getByRole("button", { name: "Undo: Turn Focus mode on" }).click();
    await expect.poll(() => ev(page, () => window.AuroraModels.qs.get("focus"))).toBe(false);
    expect(await state(page)).toMatchObject({ space: "study", timer: 1500, notes: "running" });          // the rest is untouched
    await expect(page.getByRole("button", { name: "Undo: Turn Focus mode on" })).toHaveCount(0);        // cannot be undone twice
    await expect(page.locator("#plan-receipt .chip", { hasText: /^Undone$/ })).toHaveCount(1);
    expect(await ev(page, () => window.AuroraSystem.receipts().filter((r) => r.undoState === "used").length)).toBe(1);
  });

  test("Undo all reverts what can be undone and labels what cannot", async ({ page }) => {
    await fresh(page);
    await review(page); await approveAndRun(page);
    await expect(page.locator("#plan-receipt")).toBeVisible({ timeout: 8000 });
    await page.locator("#btn-plan-undo-all").click();
    await expect.poll(() => state(page)).toMatchObject({ space: "work", focus: false, timer: null });
    await expect(page.locator("#plan-receipt .chip", { hasText: /^Undone$/ })).toHaveCount(3);
    await expect(page.locator("#plan-receipt .chip", { hasText: "Can't be undone" })).toHaveCount(1);    // resuming a paused page has no undo
    await expect(page.locator("#btn-plan-undo-all")).toHaveCount(0);
  });

  test("a failed Undo is reported honestly and the receipt records it", async ({ page }) => {
    await fresh(page);
    await ev(page, () => window.AuroraSystem.permissions.set("files.delete", "ask"));
    const run = ev(page, () => { window.__d = window.AuroraSystem.request({ type: "file.delete", arguments: { id: "timetable" } }, { source: "test" }); });
    await run;
    await page.locator(".hold-alt").click(); await page.locator(".hold-alt").click();
    await ev(page, () => window.__d);
    expect(await ev(page, () => window.AuroraSystem.files.get("timetable"))).toBeNull();
    await ev(page, () => { delete window.AuroraSystem.world.deleted["timetable"]; });                    // Trail no longer has it
    const u = await ev(page, () => window.AuroraSystem.undo(window.AuroraSystem.receipts()[0].id));
    expect(u.ok).toBe(false);
    expect(u.text).toBe("Undo did not work. Nothing else was changed.");
    expect(await ev(page, () => window.AuroraSystem.receipts()[0].undoState)).toBe("failed");
    expect(await ev(page, () => window.AuroraSystem.files.get("timetable"))).toBeNull();                // nothing was restored
  });
});

test.describe("Aura plan: editing, skipping, stopping, failing", () => {
  test("skipping a step removes it from the confirmation and from the run", async ({ page }) => {
    await fresh(page);
    await review(page);
    await skip(page, 2).click();
    await expect(page.locator("#screen-aura .plan-step").nth(1)).toHaveClass(/is-skipped/);
    await page.locator("#btn-plan-confirm").click();
    await expect(page.locator("#plan-confirmation .confirm-list li")).toHaveCount(3);
    await page.locator("#btn-plan-run").click();
    await expect(page.locator("#plan-receipt")).toBeVisible({ timeout: 8000 });
    expect(await state(page)).toMatchObject({ space: "study", focus: false, timer: 1500, receipts: 3 });
    expect(await ev(page, () => window.AuroraSystem.receipts().map((r) => r.type).sort())).toEqual(["app.open", "space.switch", "timer.start"]);
  });

  test("a skipped step can be included again", async ({ page }) => {
    await fresh(page);
    await review(page);
    await skip(page, 2).click();
    await page.getByRole("button", { name: /Include step 2/ }).click();
    await approveAndRun(page);
    await expect(page.locator("#plan-receipt")).toBeVisible({ timeout: 8000 });
    expect((await state(page)).focus).toBe(true);
  });

  test("editing the timer changes the step and what actually runs", async ({ page }) => {
    await fresh(page);
    await review(page);
    await page.locator("#btn-plan-edit").click();
    await page.locator("#step-edit-3").fill("10");
    await page.locator("#step-edit-3").dispatchEvent("change");
    await expect(page.locator("#screen-aura .plan-step").nth(3)).toContainText("Start a timer for 10 minutes");
    await approveAndRun(page);
    await expect(page.locator("#plan-receipt")).toBeVisible({ timeout: 8000 });
    expect((await state(page)).timer).toBe(600);
  });

  test("an invalid edit is refused and the plan is unchanged", async ({ page }) => {
    await fresh(page);
    await review(page);
    await page.locator("#btn-plan-edit").click();
    await page.locator("#step-edit-3").fill("99999");
    await page.locator("#step-edit-3").dispatchEvent("change");
    await expect(page.locator("#toast")).toContainText("Enter 1 to 1440 minutes");
    await expect(page.locator("#screen-aura .plan-step").nth(3)).toContainText("25 minutes");
  });

  test("Stop ends the plan after the current step; later steps do not run", async ({ page }) => {
    await fresh(page, 600);
    await review(page); await approveAndRun(page);
    await expect(page.locator("#plan-running")).toBeVisible();
    await page.locator("#btn-plan-stop").click();
    await expect(page.locator("#plan-failure")).toBeVisible({ timeout: 8000 });
    await expect(page.locator("#plan-failure h2")).toHaveText("Plan stopped");
    await expect(page.locator("#plan-failure .chip", { hasText: "Not run" }).first()).toBeVisible();
    const s = await state(page);
    expect(s.receipts).toBeLessThan(4);
    expect(s.timer).toBeNull();                                         // the last step never ran
  });

  test("a failing step stops the plan, explains why, and leaves earlier steps undoable", async ({ page }) => {
    await fresh(page);
    await ev(page, () => { window.AuroraSystem.dev.failNextType = "app.open"; });
    await review(page); await approveAndRun(page);
    await expect(page.locator("#plan-failure")).toBeVisible({ timeout: 8000 });
    await expect(page.locator("#failure-text")).toContainText("Step 3");
    await expect(page.locator('#plan-failure [data-status="failed"]')).toContainText("Simulated failure (developer option).");
    await expect(page.locator('#plan-failure [data-status="done"]')).toHaveCount(2);
    expect(await state(page)).toMatchObject({ space: "study", focus: true, timer: null });
    await expect(page.getByRole("button", { name: "Undo: Switch to Study Space" })).toBeVisible();
    await expect(page.getByRole("button", { name: "Undo: Turn Focus mode on" })).toBeVisible();
    await page.locator("#btn-plan-undo-all").click();
    await expect.poll(() => state(page)).toMatchObject({ space: "work", focus: false });
  });
});

test.describe("Aura plan: permissions", () => {
  test("a step Passport denies blocks the plan until it is skipped", async ({ page }) => {
    await fresh(page);
    await ev(page, () => window.AuroraSystem.permissions.set("settings.write", "deny"));
    await review(page);
    await expect(page.locator("#plan-blocked")).toBeVisible();
    await expect(page.locator("#plan-blocked")).toContainText("Step 2 is blocked by Passport");
    await expect(page.locator("#btn-plan-confirm")).toBeDisabled();
    await skip(page, 2).click();
    await expect(page.locator("#plan-blocked")).toHaveCount(0);
    await expect(page.locator("#btn-plan-confirm")).toBeEnabled();
    await approveAndRun(page);
    await expect(page.locator("#plan-receipt")).toBeVisible({ timeout: 8000 });
    expect((await state(page)).focus).toBe(false);                      // the denied step never ran
  });

  test("a permission flipped to deny after review but before Run refuses the run; nothing executes", async ({ page }) => {
    await fresh(page);
    const before = await state(page);
    await review(page);
    await page.locator("#btn-plan-confirm").click();
    await ev(page, () => window.AuroraSystem.permissions.set("settings.write", "deny"));
    await page.locator("#btn-plan-run").click();
    await expect(page.locator("#toast")).toContainText(/Passport|permission|Review it again/);
    await page.waitForTimeout(600);
    expect(await state(page)).toEqual(before);
    await expect(page.locator("#plan-receipt")).toHaveCount(0);
    await expect(page.locator("#plan-blocked")).toBeVisible();            // the screen re-previews: the blocked step is shown
    await expect(page.locator("#btn-plan-confirm")).toBeDisabled();       // and the person cannot repeat the same refused approval
  });

  test("permission policy: S3 can never be Allow, unknown ids and values are refused, every change is logged", async ({ page }) => {
    await page.goto(BASE_URL);
    const r = await ev(page, () => {
      const P = window.AuroraSystem.permissions;
      const out = { s3: P.set("files.delete", "allow"), ext: P.set("external.send", "allow"), unknown: P.set("nope.nothing", "allow"), proto: P.set("__proto__", "allow"), badValue: P.set("files.edit", "yes"), same: P.set("files.edit", "ask") };
      out.ok1 = P.set("files.edit", "allow"); out.ok2 = P.set("files.edit", "deny"); out.s3deny = P.set("files.delete", "deny");
      out.log = P.log().map((e) => e.permission + ":" + e.from + ">" + e.to);
      out.after = P.get("files.delete");
      return out;
    });
    expect(r.s3).toEqual({ ok: false, reason: "always-ask" });
    expect(r.ext).toEqual({ ok: false, reason: "always-ask" });
    expect(r.unknown).toEqual({ ok: false, reason: "unknown-permission" });
    expect(r.proto).toEqual({ ok: false, reason: "unknown-permission" });
    expect(r.badValue).toEqual({ ok: false, reason: "invalid-decision" });
    expect(r.same).toEqual({ ok: true, unchanged: true });
    expect(r.log).toEqual(["files.edit:ask>allow", "files.edit:allow>deny", "files.delete:ask>deny"]);
    expect(r.after).toBe("deny");
  });
});

test.describe("Destructive actions need a deliberate hold (S3)", () => {
  async function requestDelete(page) {
    await ev(page, () => { window.__d = window.AuroraSystem.request({ type: "file.delete", arguments: { id: "timetable" } }, { source: "test" }); });
    await expect(page.locator("#review-dialog")).toBeVisible();
  }
  const exists = (page) => ev(page, () => !!window.AuroraSystem.files.get("timetable"));

  test("a plain click on the confirm button does nothing", async ({ page }) => {
    await fresh(page);
    await requestDelete(page);
    await expect(page.locator("#review-dialog")).toContainText("Delete “Exam timetable.txt”");
    await page.locator("#btn-review-approve").click();
    await page.waitForTimeout(400);
    expect(await exists(page)).toBe(true);
    await expect(page.locator("#review-dialog")).toBeVisible();
  });

  test("releasing early cancels the hold", async ({ page }) => {
    await fresh(page);
    await requestDelete(page);
    const b = await page.locator("#btn-review-approve").boundingBox();
    await page.mouse.move(b.x + b.width / 2, b.y + b.height / 2); await page.mouse.down();
    await page.waitForTimeout(500); await page.mouse.up();
    await page.waitForTimeout(900);
    expect(await exists(page)).toBe(true);
  });

  test("a full pointer hold deletes it to Trail, and Undo restores it", async ({ page }) => {
    await fresh(page);
    await requestDelete(page);
    const b = await page.locator("#btn-review-approve").boundingBox();
    await page.mouse.move(b.x + b.width / 2, b.y + b.height / 2); await page.mouse.down();
    await page.waitForTimeout(1500); await page.mouse.up();
    const out = await ev(page, () => window.__d);
    expect(out.ok).toBe(true);
    expect(await exists(page)).toBe(false);
    expect(await ev(page, () => window.AuroraSystem.files.deleted().map((f) => f.id))).toEqual(["timetable"]);
    const rec = await ev(page, () => window.AuroraSystem.receipts()[0]);
    expect(rec).toMatchObject({ type: "file.delete", status: "done", undoState: "available" });
    expect((await ev(page, () => window.AuroraSystem.undo(window.AuroraSystem.receipts()[0].id))).ok).toBe(true);
    expect(await exists(page)).toBe(true);
    expect(await ev(page, () => window.AuroraSystem.files.deleted().length)).toBe(0);
  });

  test("a keyboard hold works", async ({ page }) => {
    await fresh(page);
    await requestDelete(page);
    await page.locator("#btn-review-approve").focus();
    await page.keyboard.down("Space");
    await page.waitForTimeout(1500);
    await page.keyboard.up("Space");
    expect((await ev(page, () => window.__d)).ok).toBe(true);
    expect(await exists(page)).toBe(false);
  });

  test("the visible two-step alternative works, and one press alone does not", async ({ page }) => {
    await fresh(page);
    await requestDelete(page);
    await page.locator(".hold-alt").click();
    await page.waitForTimeout(300);
    expect(await exists(page)).toBe(true);
    await expect(page.locator(".hold-alt")).toContainText("Step 2");
    await page.locator(".hold-alt").click();
    expect((await ev(page, () => window.__d)).ok).toBe(true);
    expect(await exists(page)).toBe(false);
  });

  test("Cancel and Escape both leave the file alone and report cancelled", async ({ page }) => {
    await fresh(page);
    await requestDelete(page);
    await page.locator("#btn-review-cancel").click();
    expect(await ev(page, () => window.__d)).toMatchObject({ ok: false, cancelled: true });
    expect(await exists(page)).toBe(true);
    await requestDelete(page);
    await page.keyboard.press("Escape");
    expect(await ev(page, () => window.__d)).toMatchObject({ ok: false, cancelled: true });
    expect(await exists(page)).toBe(true);
    expect(await ev(page, () => window.AuroraSystem.receipts().length)).toBe(0);
  });

  test("with delete denied in Passport the dialog says it cannot run and offers Passport, not a bypass", async ({ page }) => {
    await fresh(page);
    await ev(page, () => window.AuroraSystem.permissions.set("files.delete", "deny"));
    await ev(page, () => { window.__d = window.AuroraSystem.request({ type: "file.delete", arguments: { id: "timetable" } }, { source: "test" }); });
    await expect(page.locator("#review-dialog")).toContainText("can't run");
    await expect(page.locator("#btn-review-approve")).toHaveCount(0);
    await expect(page.locator("#btn-review-passport")).toBeVisible();
    await page.keyboard.press("Escape");
    expect(await ev(page, () => window.__d)).toMatchObject({ ok: false, blocked: true });
    expect(await exists(page)).toBe(true);
  });

  test("sending outside the device is unsupported: no approve button, nothing happens", async ({ page }) => {
    await fresh(page);
    await ev(page, () => { window.__d = window.AuroraSystem.request({ type: "share.send", arguments: { id: "timetable", to: "someone" } }, { source: "test" }); });
    await expect(page.locator("#review-dialog")).toContainText("can't run");
    await expect(page.locator("#btn-review-approve")).toHaveCount(0);
    await page.keyboard.press("Escape");
    expect(await ev(page, () => window.__d)).toMatchObject({ ok: false, blocked: true, reason: "unsupported" });
    expect(await ev(page, () => window.AuroraSystem.receipts().length)).toBe(0);
  });

  test("a move (S2) needs explicit approval and is undoable", async ({ page }) => {
    await fresh(page);
    await ev(page, () => { window.__d = window.AuroraSystem.request({ type: "file.move", arguments: { id: "timetable", destination: "work" } }, { source: "test" }); });
    await expect(page.locator("#review-dialog")).toContainText("Move “Exam timetable.txt” to Work");
    expect(await ev(page, () => window.AuroraSystem.files.get("timetable").space)).toBe("study");
    await page.locator("#btn-review-approve").click();
    expect((await ev(page, () => window.__d)).ok).toBe(true);
    expect(await ev(page, () => window.AuroraSystem.files.get("timetable").space)).toBe("work");
    expect((await ev(page, () => window.AuroraSystem.undo(window.AuroraSystem.receipts()[0].id))).ok).toBe(true);
    expect(await ev(page, () => window.AuroraSystem.files.get("timetable").space)).toBe("study");
  });
});

test.describe("Aura mode and unsupported requests", () => {
  test("with Aura off, ask() refuses with a toast and nothing opens or runs", async ({ page }) => {
    await fresh(page);
    await ev(page, () => window.AuroraModels.qs.set("auraMode", "off"));
    const r = await ev(page, async () => [await window.AuroraReview.ask("prepare my phone for studying"), await window.AuroraReview.ask("start a 5 minute timer")]);
    expect(r).toEqual([{ ok: false, reason: "aura-off" }, { ok: false, reason: "aura-off" }]);
    await expect(page.locator("#toast")).toContainText("Aura is off");
    await expect(page.locator("#screen-aura")).toBeHidden();
    expect((await state(page)).receipts).toBe(0);
  });

  test("unrecognised text is refused without a broker call", async ({ page }) => {
    await fresh(page);
    expect(await ev(page, () => window.AuroraReview.ask("format the disk && open notes"))).toEqual({ ok: false, reason: "unrecognized" });
    expect((await state(page)).receipts).toBe(0);
  });

  test("the review surface cannot be taken a second time by another script", async ({ page }) => {
    await page.goto(BASE_URL);
    const r = await ev(page, () => { try { window.AuroraSystem.takeTrusted(() => {}); return "taken"; } catch (e) { return e.message; } });
    expect(r).toBe("trusted-surface-already-taken");
    expect(await ev(page, () => typeof window.AuroraSystem.proposer.approve)).toBe("undefined");          // propose/preview only
  });
});
