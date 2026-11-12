/**
 * tests/ui-golden/passport.test.js — M2: Passport.
 * Passport changes the REAL prototype permission state, only through AuroraSystem.permissions.set / resetAll.
 * It never runs an action, and no path can set an S3 permission to Allow.
 */
const { test, expect } = require("@playwright/test");

const BASE_URL = "http://localhost:3000";
const ev = (page, fn, arg) => page.evaluate(fn, arg);
const row = (page, id) => page.locator('[id="perm-' + id + '"]');   // ids contain dots
const radio = (page, id, name) => row(page, id).getByRole("radio", { name, exact: true });
const get = (page, id) => ev(page, (i) => window.AuroraSystem.permissions.get(i), id);
const logOf = (page) => ev(page, () => window.AuroraSystem.permissions.log().map((e) => e.permission + ":" + e.from + ">" + e.to));

async function openPassport(page) {
  await page.goto(BASE_URL);
  await page.getByRole("button", { name: "Open Passport" }).click();
  await expect(page.locator("#screen-passport")).toBeVisible();
}
const tab = (page, name) => page.getByRole("tab", { name });

test.describe("Passport: getting there", () => {
  test("is pinned on Home, found by Index, and opens its own screen", async ({ page }) => {
    await page.goto(BASE_URL);
    await expect(page.getByRole("button", { name: "Open Passport" })).toBeVisible();
    await ev(page, () => window.AuroraShell.openIndex("passport"));
    await expect(page.locator("#index-results .index-result").first()).toContainText("Open Passport");
    await expect(page.locator("#index-results .index-result").first()).not.toContainText("Not yet");
    await page.keyboard.press("Enter");
    await expect(page.locator("#screen-passport h1")).toHaveText("Passport");
    expect(await ev(page, () => window.AuroraShell.openApp("passport"))).toMatchObject({ ok: true, id: "passport", created: false });
  });

  test("Open Passport in a blocked review dialog goes there end to end", async ({ page }) => {
    await page.goto(BASE_URL);
    await ev(page, () => window.AuroraSystem.permissions.set("files.delete", "deny"));
    await ev(page, () => { window.AuroraSystem.request({ type: "file.delete", arguments: { id: "timetable" } }, { source: "test" }); });
    await page.locator("#btn-review-passport").click();
    await expect(page.locator("#screen-passport")).toBeVisible();
    await expect(radio(page, "files.delete", "Deny")).toHaveAttribute("aria-checked", "true");
    expect(await ev(page, () => !!window.AuroraSystem.files.get("timetable"))).toBe(true);
  });

  test("Back returns to where you came from", async ({ page }) => {
    await openPassport(page);
    await page.locator("#btn-passport-back").click();
    await expect(page.locator("#screen-home")).toBeVisible();
  });
});

test.describe("Passport: Overview", () => {
  test.beforeEach(async ({ page }) => { await openPassport(page); });

  test("summarises the real state and lists every permission with its explanation and risk", async ({ page }) => {
    await expect(page.locator("#passport-summary")).toHaveText("4 allowed, 3 ask each time, 1 blocked");
    await expect(page.locator(".perm-row")).toHaveCount(8);
    const cat = await ev(page, () => window.AuroraSystem.permissions.catalog().map((c) => [c.id, c.label, c.explain, c.risk, c.decision]));
    for (const [id, label, explain, risk, decision] of cat) {
      await expect(row(page, id)).toContainText(label);
      await expect(row(page, id)).toContainText(explain);
      await expect(row(page, id).locator(".risk")).toHaveAttribute("data-risk", risk);
      await expect(row(page, id).getByRole("radiogroup", { name: label + " permission" })).toBeVisible();
      await expect(radio(page, id, decision === "allow" ? "Allow" : decision === "ask" ? "Ask" : "Deny")).toHaveAttribute("aria-checked", "true");
    }
  });

  test("an action the prototype cannot do says so", async ({ page }) => {
    await expect(row(page, "external.send")).toContainText("Not available in this prototype");
    await expect(row(page, "files.delete")).not.toContainText("Not available in this prototype");
  });

  test("S3 permissions cannot be set to Allow: the option is disabled with a visible reason", async ({ page }) => {
    for (const id of ["files.delete", "external.send"]) {
      const allow = radio(page, id, "Allow");
      await expect(allow).toBeDisabled();
      await expect(allow).toHaveAccessibleDescription("Always asks: cannot be pre-approved");
      await expect(row(page, id)).toContainText("Always asks: cannot be pre-approved");
    }
    await expect(radio(page, "files.edit", "Allow")).toBeEnabled();
  });

  test("forcing the S3 Allow button through the DOM still cannot change the permission", async ({ page }) => {
    await ev(page, () => { const b = document.querySelector('[id="perm-files.delete"] [data-value="allow"]'); b.disabled = false; b.removeAttribute("aria-describedby"); b.click(); });
    await page.waitForTimeout(300);
    expect(await get(page, "files.delete")).toBe("ask");
    expect(await ev(page, () => window.AuroraSystem.permissions.set("files.delete", "allow"))).toEqual({ ok: false, reason: "always-ask" });
    expect(await ev(page, () => window.AuroraSystem.permissions.set("external.send", "allow"))).toEqual({ ok: false, reason: "always-ask" });
    expect(await logOf(page)).toEqual([]);
    await expect(page.locator("#passport-confirm")).toHaveCount(0);
  });

  test("arrow keys skip the disabled Allow option", async ({ page }) => {
    await radio(page, "files.delete", "Ask").focus();
    await page.keyboard.press("ArrowRight");                               // Ask -> Deny
    await expect.poll(() => get(page, "files.delete")).toBe("deny");
    await page.keyboard.press("ArrowRight");                               // Deny -> (Allow is disabled) -> Ask
    await expect.poll(() => get(page, "files.delete")).toBe("ask");
    expect(await logOf(page)).toEqual(["files.delete:ask>deny", "files.delete:deny>ask"]);
  });
});

test.describe("Passport: tightening is immediate and undoable", () => {
  test.beforeEach(async ({ page }) => { await openPassport(page); });

  test("Allow -> Ask applies at once, is logged, and Undo restores it", async ({ page }) => {
    await radio(page, "apps.open", "Ask").click();
    await expect.poll(() => get(page, "apps.open")).toBe("ask");
    await expect(radio(page, "apps.open", "Ask")).toHaveAttribute("aria-checked", "true");
    await expect(page.locator("#toast")).toContainText("Open apps: Allow → Ask");
    await expect(page.locator("#passport-confirm")).toHaveCount(0);        // no confirmation for tightening
    await page.locator("#toast .toast-action").click();
    await expect.poll(() => get(page, "apps.open")).toBe("allow");
    await expect(radio(page, "apps.open", "Allow")).toHaveAttribute("aria-checked", "true");
    expect(await logOf(page)).toEqual(["apps.open:allow>ask", "apps.open:ask>allow"]);
  });

  test("Ask -> Deny blocks the action for real: the Aura plan refuses to run that step", async ({ page }) => {
    await radio(page, "settings.write", "Deny").click();
    await expect.poll(() => get(page, "settings.write")).toBe("deny");
    await ev(page, () => window.AuroraShell.showScreen("screen-home"));
    await page.locator("#btn-aura-study").click();
    await expect(page.locator("#plan-blocked")).toContainText("Step 2 is blocked by Passport");
    await expect(page.locator("#btn-plan-confirm")).toBeDisabled();
  });

  test("the summary and the focus stay with the control after a change", async ({ page }) => {
    await radio(page, "apps.open", "Deny").click();
    await expect(page.locator("#passport-summary")).toHaveText("3 allowed, 3 ask each time, 2 blocked");
    await expect(radio(page, "apps.open", "Deny")).toBeFocused();
  });

  test("changing the same value again does nothing and logs nothing", async ({ page }) => {
    await radio(page, "files.edit", "Ask").click();
    await page.waitForTimeout(200);
    expect(await logOf(page)).toEqual([]);
    await expect(page.locator("#toast")).not.toHaveClass(/show/);
  });
});

test.describe("Passport: loosening needs a confirmation", () => {
  test.beforeEach(async ({ page }) => { await openPassport(page); });

  test("moving to Allow asks first, says what will run unasked, and does not change anything until confirmed", async ({ page }) => {
    await radio(page, "files.edit", "Allow").click();
    const dlg = page.locator("#passport-confirm");
    await expect(dlg).toBeVisible();
    await expect(dlg).toContainText("Allow “Move and edit files”?");
    await expect(dlg).toContainText("You will still review every change of this kind before it runs");   // S2: Allow does not skip the review
    await expect(dlg).not.toContainText("without showing you a review first");
    await expect(dlg).toContainText("Aura, Notes, Files, Vault");
    expect(await get(page, "files.edit")).toBe("ask");                                  // not yet
    await expect(radio(page, "files.edit", "Ask")).toHaveAttribute("aria-checked", "true");   // the control shows the STORED value, not the click
    expect(await logOf(page)).toEqual([]);
    await page.locator("#btn-passport-confirm").click();
    await expect.poll(() => get(page, "files.edit")).toBe("allow");
    await expect(radio(page, "files.edit", "Allow")).toHaveAttribute("aria-checked", "true");
    await expect(radio(page, "files.edit", "Allow")).toBeFocused();
    expect(await logOf(page)).toEqual(["files.edit:ask>allow"]);
  });

  test("Cancel and Escape leave everything as it was and return focus", async ({ page }) => {
    await radio(page, "files.edit", "Allow").click();
    await page.locator("#btn-passport-cancel").click();
    await expect(page.locator("#passport-confirm")).toHaveCount(0);
    expect(await get(page, "files.edit")).toBe("ask");
    expect(await logOf(page)).toEqual([]);
    await expect(radio(page, "files.edit", "Ask")).toHaveAttribute("aria-checked", "true");
    await radio(page, "files.edit", "Allow").click();
    await page.keyboard.press("Escape");
    await expect(page.locator("#passport-confirm")).toHaveCount(0);
    expect(await get(page, "files.edit")).toBe("ask");
    await expect(page.locator("#screen-passport")).toBeVisible();                     // Escape closed only the dialog
  });

  test("the safe button has initial focus", async ({ page }) => {
    await radio(page, "settings.write", "Allow").click();
    await expect(page.locator("#btn-passport-cancel")).toBeFocused();
  });

  test("Deny -> Ask is not a confirmation (it still asks you each time)", async ({ page }) => {
    await radio(page, "external.send", "Ask").click();
    await expect(page.locator("#passport-confirm")).toHaveCount(0);
    await expect.poll(() => get(page, "external.send")).toBe("ask");
  });

  test("keyboard: an arrow towards Allow asks; an arrow towards Deny applies", async ({ page }) => {
    await radio(page, "files.edit", "Ask").focus();
    await page.keyboard.press("ArrowLeft");
    await expect(page.locator("#passport-confirm")).toBeVisible();
    await page.keyboard.press("Escape");
    expect(await get(page, "files.edit")).toBe("ask");
    await radio(page, "files.edit", "Ask").focus();
    await page.keyboard.press("ArrowRight");
    await expect.poll(() => get(page, "files.edit")).toBe("deny");
  });

  test("an S0 permission allowed here really lets the action run without a review dialog", async ({ page }) => {
    await radio(page, "timer.control", "Ask").click();                                 // tighten first (immediate)
    await expect.poll(() => get(page, "timer.control")).toBe("ask");
    await radio(page, "timer.control", "Allow").click();                               // loosen: confirmation, S0 wording
    await expect(page.locator("#passport-confirm")).toContainText("without showing you a review first");
    await page.locator("#btn-passport-confirm").click();
    await expect.poll(() => get(page, "timer.control")).toBe("allow");
    await ev(page, () => { window.AuroraSystem.dev.stepDelayMs = 0; });
    const out = await ev(page, () => window.AuroraSystem.request({ type: "timer.start", arguments: { seconds: 60 } }, { source: "test" }));
    expect(out.ok).toBe(true);
    await expect(page.locator("#review-dialog")).toHaveCount(0);
    expect(await ev(page, () => window.AuroraSystem.world.timer.seconds)).toBe(60);
  });

  test("an S1 permission set to Allow STILL shows a review dialog (the broker never skips review for changes)", async ({ page }) => {
    await radio(page, "settings.write", "Allow").click();
    await page.locator("#btn-passport-confirm").click();
    await expect.poll(() => get(page, "settings.write")).toBe("allow");
    await ev(page, () => { window.__r = window.AuroraSystem.request({ type: "focus.set", arguments: { enabled: true } }, { source: "test" }); });
    await expect(page.locator("#review-dialog")).toBeVisible();
    expect(await ev(page, () => window.AuroraModels.qs.get("focus"))).toBe(false);
    await page.locator("#btn-review-cancel").click();
  });
});

test.describe("Passport: Apps", () => {
  test.beforeEach(async ({ page }) => { await openPassport(page); await tab(page, "Apps").click(); });

  test("says that permissions belong to capabilities, not apps", async ({ page }) => {
    await expect(page.locator("#passport-capability-note")).toContainText("belongs to a capability, not to one app");
  });

  test("lists each app with the capabilities it can ask for and their real decisions", async ({ page }) => {
    const notes = page.locator('.perm-app[data-app="notes"]');
    await expect(notes.locator(".perm-app-row")).toHaveCount(1);
    await expect(notes).toContainText("Move and edit files");
    await expect(notes.locator(".chip", { hasText: "Asks you" })).toBeVisible();
    await expect(page.locator('.perm-app[data-app="aura"] .perm-app-row')).toHaveCount(8);
    await expect(page.locator('.perm-app[data-app="calculator"]')).toContainText("Needs no permissions");
    await ev(page, () => window.AuroraSystem.permissions.set("files.edit", "deny"));
    await expect(notes.locator(".chip", { hasText: "Blocked" })).toBeVisible();      // live
  });

  test("camera, microphone and location are present and honest", async ({ page }) => {
    for (const k of ["camera", "microphone", "location"]) await expect(page.locator('[data-hardware="' + k + '"]')).toContainText("Hardware-dependent · not available");
  });

  test("Change jumps to that permission on Overview with focus on its current choice", async ({ page }) => {
    await page.getByRole("button", { name: "Change Notes permissions" }).click();
    await expect(tab(page, "Overview")).toHaveAttribute("aria-selected", "true");
    await expect(radio(page, "files.edit", "Ask")).toBeFocused();
  });

  test("Revoke all asks first, names what is blocked and every other app it reaches, then denies", async ({ page }) => {
    await page.getByRole("button", { name: "Revoke all for Files" }).click();
    const dlg = page.locator("#passport-revoke-confirm");
    await expect(dlg).toContainText("Revoke all for Files?");
    await expect(dlg).toContainText("Move and edit files");
    await expect(dlg).toContainText("Delete files");
    await expect(dlg).not.toContainText("Send outside this device");                   // already denied
    await expect(dlg).toContainText("Aura, Notes, Vault");
    expect(await get(page, "files.edit")).toBe("ask");
    await page.locator("#btn-passport-revoke-ok").click();
    await expect.poll(() => get(page, "files.edit")).toBe("deny");
    expect(await get(page, "files.delete")).toBe("deny");
    expect(await get(page, "timer.control")).toBe("allow");                            // an S0 capability the app does not use: untouched
    expect(await logOf(page)).toEqual(["files.edit:ask>deny", "files.delete:ask>deny"]);
    await expect(page.locator("#toast")).toContainText("Revoked 2 permissions for Files");
    await page.locator("#toast .toast-action").click();
    await expect.poll(() => get(page, "files.edit")).toBe("ask");
    expect(await get(page, "files.delete")).toBe("ask");
  });

  test("Cancelling Revoke all changes nothing; Revoke is disabled when there is nothing to revoke", async ({ page }) => {
    await page.getByRole("button", { name: "Revoke all for Notes" }).click();
    await page.locator("#btn-passport-revoke-cancel").click();
    expect(await get(page, "files.edit")).toBe("ask");
    expect(await logOf(page)).toEqual([]);
    await expect(page.getByRole("button", { name: "Revoke all for Clock" })).toBeDisabled();        // only S0 capabilities
    await expect(page.getByRole("button", { name: "Revoke all for Calculator" })).toBeDisabled();
  });

  test("S3 capabilities are never made Allow by anything on this tab", async ({ page }) => {
    await page.getByRole("button", { name: "Revoke all for Vault" }).click();
    await page.locator("#btn-passport-revoke-ok").click();
    await page.locator("#toast .toast-action").click();                                // Undo restores the previous (ask), never allow
    expect(await get(page, "files.delete")).toBe("ask");
    expect(await get(page, "external.send")).toBe("deny");
  });
});

test.describe("Passport: Activity", () => {
  test.beforeEach(async ({ page }) => { await openPassport(page); await tab(page, "Activity").click(); });

  test("starts empty with explanations", async ({ page }) => {
    await expect(page.locator(".perm-log")).toHaveCount(0);
    await expect(page.locator("#ppanel-activity")).toContainText("No changes yet");
    await expect(page.locator("#ppanel-activity")).toContainText("Nothing was blocked");
  });

  test("lists permission changes newest first, with from to, and updates live", async ({ page }) => {
    await ev(page, () => { const P = window.AuroraSystem.permissions; P.set("files.edit", "allow"); P.set("settings.write", "deny"); });
    const items = page.locator(".perm-log .gc-item");
    await expect(items).toHaveCount(2);
    await expect(items.first()).toContainText("Change settings: Ask → Deny");
    await expect(items.last()).toContainText("Move and edit files: Ask → Allow");
    await expect(items.first()).toContainText("just now");
    await ev(page, () => window.AuroraSystem.permissions.set("apps.open", "ask"));
    await expect(items).toHaveCount(3);
    await expect(items.first()).toContainText("Open apps: Allow → Ask");
  });

  test("shows blocked and stale requests from the receipts, as text", async ({ page }) => {
    await ev(page, () => {
      const S = window.AuroraSystem, real = S.receipts;
      S.receipts = () => [
        { id: "r9", at: Date.now(), status: "denied", describe: { title: "Delete “<img src=x onerror=1>”" }, errorText: "Blocked by Passport: “Delete files” is set to Deny." },
        { id: "r8", at: Date.now(), status: "done", describe: { title: "Start a timer for 1 minute" } },
        { id: "r7", at: Date.now(), status: "needs-reapproval", describe: { title: "Turn Focus mode on" }, errorText: null },
      ].concat(real());
      S.emit("receipts");
    });
    await expect(page.locator(".perm-blocked .gc-item")).toHaveCount(2);
    await expect(page.locator(".perm-blocked .gc-item").first()).toContainText("Delete “<img src=x onerror=1>”");
    await expect(page.locator(".perm-blocked .gc-item").first().locator(".chip")).toHaveText("Blocked");
    await expect(page.locator(".perm-blocked .gc-item").last().locator(".chip")).toHaveText("Needs review again");
    await expect(page.locator("#ppanel-activity img")).toHaveCount(0);
  });

  test("Reset to defaults needs a confirmation; Cancel changes nothing; Reset restores every default and logs it", async ({ page }) => {
    await ev(page, () => { const P = window.AuroraSystem.permissions; P.set("files.edit", "allow"); P.set("settings.write", "deny"); });
    await page.locator("#btn-passport-reset").click();
    await page.locator("#btn-passport-reset-cancel").click();
    expect(await get(page, "files.edit")).toBe("allow");
    await page.locator("#btn-passport-reset").click();
    await expect(page.locator("#passport-reset-confirm")).toContainText("Every permission returns to its default");
    await page.locator("#btn-passport-reset-ok").click();
    await expect.poll(() => get(page, "files.edit")).toBe("ask");
    expect(await get(page, "settings.write")).toBe("ask");
    expect(await logOf(page)).toEqual(["files.edit:ask>allow", "settings.write:ask>deny", "settings.write:deny>ask", "files.edit:allow>ask"]);   // resetAll walks the catalogue in order
  });
});

test.describe("Passport: the security boundary", () => {
  test("no Passport flow runs an action, touches files or timers, or ever makes an S3 permission Allow", async ({ page }) => {
    await openPassport(page);
    const before = await ev(page, () => JSON.stringify({ files: window.AuroraSystem.files.list(), timer: window.AuroraSystem.world.timer, deleted: window.AuroraSystem.files.deleted() }));
    await radio(page, "files.edit", "Allow").click(); await page.locator("#btn-passport-confirm").click();
    await radio(page, "settings.write", "Deny").click();
    await tab(page, "Apps").click();
    await page.getByRole("button", { name: "Revoke all for Vault" }).click(); await page.locator("#btn-passport-revoke-ok").click();
    await tab(page, "Activity").click();
    await page.locator("#btn-passport-reset").click(); await page.locator("#btn-passport-reset-ok").click();
    expect(await ev(page, () => window.AuroraSystem.receipts().length)).toBe(0);
    expect(await ev(page, () => JSON.stringify({ files: window.AuroraSystem.files.list(), timer: window.AuroraSystem.world.timer, deleted: window.AuroraSystem.files.deleted() }))).toBe(before);
    for (const id of ["files.delete", "external.send"]) expect(await get(page, id)).not.toBe("allow");
    expect((await logOf(page)).every((l) => !/files\.delete:.*>allow|external\.send:.*>allow/.test(l))).toBe(true);
  });

  test("a change made in Passport while a plan awaits approval makes the broker refuse the stale approval", async ({ page }) => {
    await page.goto(BASE_URL);
    await ev(page, () => { window.AuroraSystem.dev.stepDelayMs = 0; });
    await page.locator("#btn-aura-study").click();
    await page.locator("#btn-plan-confirm").click();                                   // approval dialog is open for the plan
    await page.keyboard.press("Escape");                                               // ...the person goes off to check Passport
    await ev(page, () => window.AuroraShell.openApp("passport"));
    await radio(page, "settings.write", "Deny").click();
    await expect.poll(() => get(page, "settings.write")).toBe("deny");
    await page.locator("#btn-passport-back").click();                                  // back to the plan, whose preview is now stale
    await expect(page.locator("#screen-aura")).toBeVisible();
    await page.locator("#btn-plan-confirm").click();
    await page.locator("#btn-plan-run").click();
    await expect(page.locator("#toast")).toContainText(/Passport|Review it again/);
    expect(await ev(page, () => window.AuroraSystem.receipts().length)).toBe(0);       // nothing ran
    expect(await ev(page, () => window.AuroraModels.qs.get("focus"))).toBe(false);
    await expect(page.locator("#plan-blocked")).toBeVisible();                         // and the plan now shows the block
  });
});

test.describe("Passport: accessibility and layout", () => {
  test("tabs follow the tab pattern: roving tabindex, arrows, Home and End, labelled panels", async ({ page }) => {
    await openPassport(page);
    await expect(page.getByRole("tablist", { name: "Passport sections" })).toBeVisible();
    await expect(tab(page, "Overview")).toHaveAttribute("aria-selected", "true");
    await expect(tab(page, "Overview")).toHaveAttribute("tabindex", "0");
    await expect(tab(page, "Apps")).toHaveAttribute("tabindex", "-1");
    await expect(page.getByRole("tabpanel")).toHaveAccessibleName("Overview");
    await tab(page, "Overview").focus();
    await page.keyboard.press("ArrowRight");
    await expect(tab(page, "Apps")).toHaveAttribute("aria-selected", "true");
    await expect(tab(page, "Apps")).toBeFocused();
    await page.keyboard.press("End");
    await expect(tab(page, "Activity")).toBeFocused();
    await page.keyboard.press("ArrowRight");
    await expect(tab(page, "Overview")).toBeFocused();
    await page.keyboard.press("ArrowLeft");
    await expect(tab(page, "Activity")).toHaveAttribute("aria-selected", "true");
    await page.keyboard.press("Home");
    await expect(tab(page, "Overview")).toHaveAttribute("aria-selected", "true");
    await expect(page.getByRole("tabpanel")).toHaveAttribute("aria-labelledby", "ptab-overview");
  });

  for (const [w, h] of [[360, 740], [390, 844], [768, 1024], [1280, 800]]) {
    for (const mode of ["default", "200%"]) {
      test(`no overflow, clipping or small targets at ${w}px (${mode} text) on every tab`, async ({ page }) => {
        await page.setViewportSize({ width: w, height: h });
        await openPassport(page);
        if (mode === "200%") await ev(page, () => document.documentElement.setAttribute("data-text-scale", "200"));
        for (const name of ["Overview", "Apps", "Activity"]) {
          await tab(page, name).click();
          const problems = await ev(page, () => {
            const out = [], vw = document.documentElement.clientWidth;
            if (document.documentElement.scrollWidth > vw + 1) out.push("page scrolls horizontally " + document.documentElement.scrollWidth);
            document.querySelectorAll("#screen-passport button, #screen-passport [role=radio]").forEach((b) => {
              const r = b.getBoundingClientRect(); if (!r.width) return;
              if (r.left < -1 || r.right > vw + 1) out.push("clipped " + (b.getAttribute("aria-label") || b.textContent.trim().slice(0, 20)));
              if (Math.min(r.width, r.height) < 44) out.push("small " + (b.getAttribute("aria-label") || b.textContent.trim().slice(0, 20)) + " " + Math.round(r.width) + "x" + Math.round(r.height));
            });
            return out;
          });
          expect(problems, name).toEqual([]);
        }
      });
    }
  }

  test("disabled Allow is not conveyed by colour alone and renders in Clear + high contrast without errors", async ({ page }) => {
    const errors = []; page.on("pageerror", (e) => errors.push(e.message)); page.on("console", (m) => { if (m.type() === "error") errors.push(m.text()); });
    await openPassport(page);
    await ev(page, () => { window.AuroraShell.setTier("clear"); window.AuroraShell.setContrast("high"); window.AuroraShell.setAppearance("dark"); });
    await expect(row(page, "files.delete")).toContainText("Always asks: cannot be pre-approved");
    expect(await radio(page, "files.delete", "Allow").evaluate((b) => getComputedStyle(b).textDecorationLine)).toContain("line-through");
    expect(errors).toEqual([]);
  });
});
